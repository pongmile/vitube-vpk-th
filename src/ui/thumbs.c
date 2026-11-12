/* ViTube — async image cache (video thumbnails, channel avatars).
 * A worker thread downloads JPEG bytes for a caller-given URL; the main
 * thread decodes and uploads them as vita2d textures (GPU objects stay on
 * the render thread). Entries are keyed by an opaque string (video id,
 * channel id, ...) so the same URL is never re-queued while cached.
 *
 * The Downloads screen also shows thumbnails that were saved to the memory
 * card (vt_thumbs_get_file): the same slots, the same worker and the same
 * decode step, only the bytes come from a file instead of the network — so
 * the 48-slot cache, the retry budget and the netwatch reset all apply to
 * them unchanged, and a screen full of downloads works with no network. */
#include "ui/thumbs.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <vita_https.h>

#include "app.h"
#include "log.h"
#include "net/netwatch.h"

#define THUMB_SLOTS 48
#define THUMB_KEY_MAX 32
#define THUMB_URL_MAX 256 /* matches YT_THUMB_URL_MAX in yt/model.h */
#define THUMB_MAX_JPEG (256 * 1024)

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_DOWNLOADING,
    SLOT_DOWNLOADED,
    SLOT_READY,
    SLOT_FAILED
} SlotState;

typedef struct {
    char key[THUMB_KEY_MAX];
    char url[THUMB_URL_MAX]; /* a file path when is_file */
    int is_file;             /* the bytes are read from `url`, not fetched */
    SlotState state;
    unsigned char *jpeg;
    size_t jpeg_len;
    vita2d_texture *texture;
    unsigned last_used;
    /* Failures used to be permanent AND self-pinning: the key-hit branch
     * below refreshes last_used before looking at the state, so a failed
     * slot stayed most-recently-used and could never even be evicted for a
     * fresh attempt. One dropped request left that thumbnail black for the
     * rest of the session. These two allow a bounded retry instead. */
    unsigned attempts;
    unsigned failed_at;
} ThumbSlot;

/* Two further tries, spaced far enough apart that a row redrawn every frame
 * does not turn a retry into a request flood. g_tick counts vt_thumbs_get()
 * calls, so this is "a few hundred rows drawn", not wall-clock. */
#define THUMB_MAX_ATTEMPTS 3
#define THUMB_RETRY_TICKS 600

static ThumbSlot g_slots[THUMB_SLOTS];
/* vt_netwatch_generation() as of the last vt_thumbs_get(); see there. */
static unsigned g_net_generation;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker;
static volatile int g_running;
static volatile int g_cancel;
static unsigned g_tick;
static VitaHttpsClient *g_thumb_client;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} JpegBuf;

static size_t jpeg_write(const void *data, size_t size, void *opaque) {
    JpegBuf *buf = opaque;
    if (buf->len + size > buf->cap) return 0;
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    return size;
}

/* Normalises a thumbnail URL into something fetchable.
 *
 * 1. The TV client returns channel avatars PROTOCOL-RELATIVE — the parsed
 *    value really is "//yt3.googleusercontent.com/...", with no scheme
 *    (confirmed from the device log, 2026-08-28: 39 of 39 channels parsed an
 *    avatar and every request still failed). Video thumbnails come from a
 *    different host with a full https:// URL, which is why only the channel
 *    rows stayed empty. Prepend the scheme.
 * 2. googleusercontent encodes the wanted image format in the size suffix:
 *    "-rw" is WebP, "-rj" is JPEG, and vita2d decodes no WebP. The avatars
 *    observed so far already say "-rj", so this half is belt-and-braces. */
static void copy_url_as_jpeg(char *dst, size_t cap, const char *url) {
    if (url[0] == '/' && url[1] == '/')
        snprintf(dst, cap, "https:%s", url);
    else
        snprintf(dst, cap, "%s", url);
    /* 3. i.ytimg.com hands the Android client WebP for a good share of the
     *    videos — "/vi_webp/<id>/mqdefault.webp" — and which ones is decided
     *    per response, not per video, so the same clip can arrive as JPEG one
     *    time and WebP the next. vita2d_load_JPEG_buffer checks the magic and
     *    returns NULL without a word, which is exactly the "sometimes black"
     *    the user reported. Every such image also exists as JPEG under
     *    "/vi/<id>/mqdefault.jpg" (checked live), so ask for that instead. */
    char *seg = strstr(dst, "/vi_webp/");
    if (seg) {
        /* "/vi_webp/" -> "/vi/": five bytes shorter, move the tail down. */
        memmove(seg + 3, seg + 8, strlen(seg + 8) + 1);
        /* ".webp" -> ".jpg" wherever it sits — these URLs often carry an
         * "?sqp=..." query, so matching only at the end of the string would
         * silently leave exactly those requesting WebP again. */
        char *ext = strstr(dst, ".webp");
        if (ext) {
            memcpy(ext, ".jpg", 4);
            memmove(ext + 4, ext + 5, strlen(ext + 5) + 1);
        }
    }
    size_t n = strlen(dst);
    for (size_t i = 0; i + 2 < n; i++) {
        if (dst[i] != '-' || dst[i + 1] != 'r' || dst[i + 2] != 'w') continue;
        char next = dst[i + 3]; /* i+3 <= n, so at worst the terminator */
        if (next == '\0' || next == '-' || next == '?') dst[i + 2] = 'j';
    }
}

/* The file twin of download_jpeg(), for a thumbnail saved with a download.
 * Worker thread, under the app-wide card lock like every other ux0: access
 * (log.h). A file at or over the cap is refused rather than cut: half a JPEG
 * either fails to decode or shows a torn image. */
static int read_jpeg_file(const char *path, unsigned char **out,
                          size_t *out_len) {
    unsigned char *data = malloc(THUMB_MAX_JPEG);
    if (!data) return -1;
    size_t len = 0;
    vt_io_lock();
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        for (;;) {
            int n = sceIoRead(fd, data + len, THUMB_MAX_JPEG - len);
            if (n <= 0) break;
            len += (size_t)n;
            if (len >= THUMB_MAX_JPEG) break;
        }
        sceIoClose(fd);
    }
    vt_io_unlock();
    if (fd < 0 || len == 0 || len >= THUMB_MAX_JPEG) {
        free(data);
        return -1;
    }
    unsigned char *shrunk = realloc(data, len);
    if (shrunk) data = shrunk;
    *out = data;
    *out_len = len;
    return 0;
}

static int download_jpeg(const char *url, unsigned char **out, size_t *out_len) {
    JpegBuf buf = { malloc(THUMB_MAX_JPEG), 0, THUMB_MAX_JPEG };
    if (!buf.data) return -1;
    VitaHttpsRequest request = {
        .url = url,
        .write = jpeg_write,
        .write_opaque = &buf,
        .cancel_flag = &g_cancel,
    };
    VitaHttpsResponse response = {0};
    int ret = vita_https_perform(g_thumb_client, &request, &response);
    if (ret < 0 || response.status_code != 200 || buf.len == 0) {
        free(buf.data);
        return -1;
    }
    /* A typical thumbnail is 10-25 KiB; keeping the whole 256 KiB download
     * buffer pinned until the main thread pumps it (which it does not during
     * playback) parked up to 12 MiB of idle heap next to the decoder. */
    unsigned char *shrunk = realloc(buf.data, buf.len);
    if (shrunk) buf.data = shrunk;
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static void *worker_main(void *opaque) {
    (void)opaque;
    while (g_running) {
        char url[THUMB_URL_MAX] = {0};
        char key[THUMB_KEY_MAX] = {0};
        int slot_index = -1;
        int is_file = 0;
        pthread_mutex_lock(&g_lock);
        /* Most recently drawn first: that is what is on screen now. Queue
         * order was request order, so after a fast scroll the visible rows
         * waited behind every row scrolled past, each a full fetch. */
        for (int i = 0; i < THUMB_SLOTS; i++) {
            if (g_slots[i].state != SLOT_QUEUED) continue;
            if (slot_index < 0 ||
                g_slots[i].last_used > g_slots[slot_index].last_used)
                slot_index = i;
        }
        if (slot_index >= 0) {
            g_slots[slot_index].state = SLOT_DOWNLOADING;
            g_slots[slot_index].attempts++;
            memcpy(url, g_slots[slot_index].url, sizeof url);
            memcpy(key, g_slots[slot_index].key, sizeof key);
            is_file = g_slots[slot_index].is_file;
        }
        pthread_mutex_unlock(&g_lock);
        if (slot_index < 0) {
            sceKernelDelayThread(30000);
            continue;
        }
        unsigned char *jpeg = NULL;
        size_t len = 0;
        int ret = is_file ? read_jpeg_file(url, &jpeg, &len)
                          : download_jpeg(url, &jpeg, &len);
        pthread_mutex_lock(&g_lock);
        ThumbSlot *slot = &g_slots[slot_index];
        /* the slot may have been recycled for a different key while
         * downloading */
        if (slot->state == SLOT_DOWNLOADING &&
            strcmp(slot->key, key) == 0) {
            if (ret == 0) {
                slot->jpeg = jpeg;
                slot->jpeg_len = len;
                slot->state = SLOT_DOWNLOADED;
                jpeg = NULL;
            } else {
                slot->state = SLOT_FAILED;
                slot->failed_at = g_tick;
            }
        }
        pthread_mutex_unlock(&g_lock);
        /* Logged after the unlock: vt_log() writes to the memory card, and
         * holding g_lock across that would stall the worker and pair this
         * lock with the I/O lock. Download and decode failures were
         * indistinguishable before — both just left a black square. */
        if (ret != 0)
            vt_log(is_file ? "thumb file unreadable: %.150s"
                           : "thumb download failed: %.150s", url);
        free(jpeg);
    }
    return NULL;
}

int vt_thumbs_init(void) {
    VitaHttpsClientConfig config = {
        .user_agent = "ViTube/" VITUBE_VERSION,
        .connect_timeout_ms = 8000,
        .request_timeout_ms = 15000,
    };
    g_thumb_client = vita_https_client_create(&config);
    if (!g_thumb_client) return -1;
    g_cancel = 0;
    g_running = 1;
    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0) {
        g_running = 0;
        return -1;
    }
    return 0;
}

/* Safe to call while the worker is mid-download: a slot in SLOT_DOWNLOADING
 * owns no jpeg or texture yet (recycle_slot freed both before queueing), and
 * the worker re-checks state+key under the lock before storing its result —
 * a cleared slot fails that check, so the finished download is discarded and
 * freed by the worker itself. Main thread only: it frees GPU textures. */
void vt_thumbs_clear(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < THUMB_SLOTS; i++) {
        free(g_slots[i].jpeg);
        if (g_slots[i].texture) vita2d_free_texture(g_slots[i].texture);
        memset(&g_slots[i], 0, sizeof g_slots[i]);
    }
    pthread_mutex_unlock(&g_lock);
}

void vt_thumbs_term(void) {
    g_cancel = 1;
    g_running = 0;
    pthread_join(g_worker, NULL);
    vt_thumbs_clear();
    if (g_thumb_client) vita_https_client_destroy(g_thumb_client);
    g_thumb_client = NULL;
}

static void recycle_slot(ThumbSlot *slot, const char *key, const char *url,
                         int is_file) {
    free(slot->jpeg);
    slot->jpeg = NULL;
    slot->jpeg_len = 0;
    if (slot->texture) {
        vita2d_free_texture(slot->texture);
        slot->texture = NULL;
    }
    snprintf(slot->key, sizeof slot->key, "%s", key);
    slot->is_file = is_file;
    /* A path is used as it is; only a URL needs the JPEG rewrites. */
    if (is_file) snprintf(slot->url, sizeof slot->url, "%s", url);
    else copy_url_as_jpeg(slot->url, sizeof slot->url, url);
    slot->state = SLOT_QUEUED;
    slot->last_used = g_tick;
    /* A new image starts with a clean retry budget — without this it would
     * inherit the previous occupant's failures and could be given up on
     * after a single attempt. */
    slot->attempts = 0;
    slot->failed_at = 0;
}

/* The one lookup behind vt_thumbs_get() and vt_thumbs_get_file(): `source` is
 * a URL, or a file path when is_file. */
static vita2d_texture *thumbs_lookup(const char *key, const char *source,
                                     int is_file) {
    if (!key || !key[0] || !source || !source[0]) return NULL;
    vita2d_texture *texture = NULL;
    pthread_mutex_lock(&g_lock);
    g_tick++;
    /* The console woke from suspend or the network came back: give every
     * failed image a fresh retry budget. THUMB_MAX_ATTEMPTS tries are spaced
     * by g_tick, i.e. by rows drawn, and with a handful of rows on screen
     * that is about two seconds apart — so a list scrolled in the seconds
     * after a wake, while the Wi-Fi is still re-associating, used up all
     * three on a dead network and left those thumbnails black for the
     * rest of the session, the list loads having meanwhile recovered by
     * themselves (ui/net_task.c). */
    unsigned generation = vt_netwatch_generation();
    if (generation != g_net_generation) {
        g_net_generation = generation;
        for (int i = 0; i < THUMB_SLOTS; i++)
            if (g_slots[i].state == SLOT_FAILED) g_slots[i].attempts = 0;
    }
    ThumbSlot *victim = NULL;
    unsigned oldest = 0xFFFFFFFFu;
    int found = 0;
    for (int i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot *slot = &g_slots[i];
        if (slot->key[0] && strcmp(slot->key, key) == 0) {
            slot->last_used = g_tick;
            if (slot->state == SLOT_READY) texture = slot->texture;
            /* Give a failed slot another go: the causes seen in practice —
             * a dropped request, a format the decoder rejects — are not all
             * permanent, and without this the row can never recover. */
            else if (slot->state == SLOT_FAILED &&
                     slot->attempts < THUMB_MAX_ATTEMPTS &&
                     g_tick - slot->failed_at > THUMB_RETRY_TICKS)
                slot->state = SLOT_QUEUED;
            found = 1;
            break;
        }
        /* don't steal a slot the worker is writing to */
        if (slot->state != SLOT_DOWNLOADING && slot->last_used < oldest) {
            oldest = slot->last_used;
            victim = slot;
        }
    }
    if (!found && victim) recycle_slot(victim, key, source, is_file);
    pthread_mutex_unlock(&g_lock);
    return texture;
}

vita2d_texture *vt_thumbs_get(const char *key, const char *url) {
    return thumbs_lookup(key, url, 0);
}

/* Keyed apart from the network entries ("f:" + key): a list row and a
 * download row for the same video would otherwise be one slot with two
 * different sources, and a network failure on the row in Home would block the
 * saved file on the row in Downloads — exactly the offline case this exists
 * for. Two textures of one 152x85 picture are cheaper than that. */
vita2d_texture *vt_thumbs_get_file(const char *key, const char *path) {
    char file_key[THUMB_KEY_MAX];
    if (!key || !key[0]) return NULL;
    snprintf(file_key, sizeof file_key, "f:%s", key);
    return thumbs_lookup(file_key, path, 1);
}

void vt_thumbs_pump(void) {
    int uploads = 0;
    /* Details of one decode failure, carried out of the locked region so the
     * card write below cannot stall the worker. The first four bytes are the
     * decisive part: "46464952" is RIFF, i.e. a WebP the decoder will never
     * accept; "E0FFD8FF" is a real JPEG it rejected anyway, which would point
     * at the texture allocation rather than the format. */
    char failed_url[THUMB_URL_MAX] = {0};
    unsigned magic = 0, failed_len = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < THUMB_SLOTS && uploads < 2; i++) {
        ThumbSlot *slot = &g_slots[i];
        if (slot->state != SLOT_DOWNLOADED) continue;
        slot->texture = vita2d_load_JPEG_buffer(slot->jpeg, slot->jpeg_len);
        if (!slot->texture && !failed_url[0]) {
            snprintf(failed_url, sizeof failed_url, "%s", slot->url);
            failed_len = (unsigned)slot->jpeg_len;
            if (slot->jpeg_len >= 4) memcpy(&magic, slot->jpeg, 4);
        }
        free(slot->jpeg);
        slot->jpeg = NULL;
        slot->jpeg_len = 0;
        if (slot->texture) {
            slot->state = SLOT_READY;
        } else {
            slot->state = SLOT_FAILED;
            slot->failed_at = g_tick;
        }
        uploads++;
    }
    pthread_mutex_unlock(&g_lock);
    if (failed_url[0])
        vt_log("thumb decode failed: len=%u magic=%08X %.130s", failed_len,
               magic, failed_url);
}
