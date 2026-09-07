/* ViTube — persisted app state. See store.h. */
#include "store.h"

#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "app.h"
#include "log.h"

/* Generous next to the ~15 KB the full history actually serialises to, so a
 * file that grew under an older build still loads rather than being silently
 * treated as corrupt. */
#define STORE_READ_MAX (64 * 1024)

static yt_video_t g_history[VT_HISTORY_MAX];
static int g_history_count;
static char g_searches[VT_SEARCHES_MAX][128];
static int g_search_count;
static int g_channel_sort;
static unsigned g_revision;

/* Truncation that never cuts a UTF-8 sequence (see parse.c: json_string()
 * below rejects invalid UTF-8 and the whole entry would vanish). */
static void copy_utf8(char *dst, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { dst[0] = '\0'; return; }
    size_t n = strlen(s);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static void copy_field(char *dst, size_t cap, json_t *obj, const char *key) {
    copy_utf8(dst, cap, json_string_value(json_object_get(obj, key)));
}

/* Writes the whole file. Holds the I/O lock across the sceIo calls only —
 * vt_log() takes the same non-recursive lock, so nothing inside may log. */
static void save(void) {
    json_t *root = json_object();
    json_t *hist = json_array();
    for (int i = 0; i < g_history_count; i++) {
        const yt_video_t *v = &g_history[i];
        json_t *entry = json_pack(
            "{s:s,s:s,s:s,s:s,s:s,s:s}",
            "id", v->video_id, "title", v->title, "channel", v->channel,
            "views", v->views, "duration", v->duration,
            "thumb", v->thumb_url);
        /* Logging is fine here: the I/O lock is only taken further down. */
        if (!entry || json_array_append_new(hist, entry) < 0)
            vt_log("store: history entry %s dropped (invalid text)",
                   v->video_id);
    }
    json_object_set_new(root, "history", hist);
    json_t *searches = json_array();
    for (int i = 0; i < g_search_count; i++) {
        json_t *s = json_string(g_searches[i]);
        if (!s || json_array_append_new(searches, s) < 0)
            vt_log("store: search entry dropped (invalid text)");
    }
    json_object_set_new(root, "searches", searches);
    json_object_set_new(root, "channel_sort", json_integer(g_channel_sort));
    char *text = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!text) return;
    g_revision++;

    size_t len = strlen(text);
    vt_io_lock();
    sceIoMkdir(VITUBE_DATA_DIR, 0777);
    /* 0777 for the same reason as oauth.json: a file written 0600 in this
     * directory did not survive a relaunch. */
    SceUID fd = sceIoOpen(VITUBE_STATE_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int wrote = -1;
    if (fd >= 0) {
        wrote = sceIoWrite(fd, text, len);
        sceIoClose(fd);
    }
    vt_io_unlock();
    if (wrote != (int)len)
        vt_log("store save FAILED: fd=%#x wrote=%d/%d", fd, wrote, (int)len);
    free(text);
}

void vt_store_load(void) {
    g_history_count = 0;
    g_search_count = 0;
    g_channel_sort = 0;

    char *buf = malloc(STORE_READ_MAX + 1);
    if (!buf) return;
    vt_io_lock();
    SceUID fd = sceIoOpen(VITUBE_STATE_PATH, SCE_O_RDONLY, 0);
    int n = -1;
    if (fd >= 0) {
        n = sceIoRead(fd, buf, STORE_READ_MAX);
        sceIoClose(fd);
    }
    vt_io_unlock();
    if (n <= 0) {
        free(buf);
        vt_log("store: nothing stored yet");
        return;
    }
    buf[n] = '\0';
    json_error_t err;
    json_t *root = json_loads(buf, 0, &err);
    free(buf);
    if (!root) {
        vt_log("store load: json parse failed: %s", err.text);
        return;
    }

    json_t *hist = json_object_get(root, "history");
    if (json_is_array(hist)) {
        size_t i;
        json_t *item;
        json_array_foreach(hist, i, item) {
            if (g_history_count >= VT_HISTORY_MAX) break;
            if (!json_is_object(item)) continue;
            yt_video_t *v = &g_history[g_history_count];
            memset(v, 0, sizeof *v);
            copy_field(v->video_id, sizeof v->video_id, item, "id");
            copy_field(v->title, sizeof v->title, item, "title");
            copy_field(v->channel, sizeof v->channel, item, "channel");
            copy_field(v->views, sizeof v->views, item, "views");
            copy_field(v->duration, sizeof v->duration, item, "duration");
            copy_field(v->thumb_url, sizeof v->thumb_url, item, "thumb");
            /* Nothing to sort a history by but the order it was written in,
             * and a stored age would be wrong the moment it was read back. */
            v->age_min = -1;
            if (v->video_id[0] && v->title[0]) g_history_count++;
        }
    }
    json_t *searches = json_object_get(root, "searches");
    if (json_is_array(searches)) {
        size_t i;
        json_t *item;
        json_array_foreach(searches, i, item) {
            const char *s = json_string_value(item);
            if (!s || !s[0] || g_search_count >= VT_SEARCHES_MAX) continue;
            snprintf(g_searches[g_search_count], sizeof g_searches[0], "%s", s);
            g_search_count++;
        }
    }
    json_t *sort = json_object_get(root, "channel_sort");
    if (json_is_integer(sort)) g_channel_sort = (int)json_integer_value(sort);
    json_decref(root);
    vt_log("store: %d watched, %d searches, sort=%d", g_history_count,
           g_search_count, g_channel_sort);
}

unsigned vt_store_revision(void) { return g_revision; }

int vt_store_history_count(void) { return g_history_count; }

const yt_video_t *vt_store_history_at(int index) {
    if (index < 0 || index >= g_history_count) return 0;
    return &g_history[index];
}

void vt_store_history_add(const yt_video_t *video) {
    if (!video || !video->video_id[0]) return;
    /* Rewatching moves an entry to the front rather than adding a second
     * copy — a history of the same video five times is not a history. */
    int existing = -1;
    for (int i = 0; i < g_history_count; i++)
        if (strcmp(g_history[i].video_id, video->video_id) == 0) {
            existing = i;
            break;
        }
    int shift_from = existing >= 0 ? existing
                   : (g_history_count < VT_HISTORY_MAX ? g_history_count++
                                                       : VT_HISTORY_MAX - 1);
    for (int i = shift_from; i > 0; i--) g_history[i] = g_history[i - 1];
    g_history[0] = *video;
    g_history[0].age_min = -1;
    save();
}

int vt_store_search_count(void) { return g_search_count; }

const char *vt_store_search_at(int index) {
    if (index < 0 || index >= g_search_count) return 0;
    return g_searches[index];
}

void vt_store_search_add(const char *query) {
    if (!query || !query[0]) return;
    int existing = -1;
    for (int i = 0; i < g_search_count; i++)
        if (strcmp(g_searches[i], query) == 0) {
            existing = i;
            break;
        }
    int shift_from = existing >= 0 ? existing
                   : (g_search_count < VT_SEARCHES_MAX ? g_search_count++
                                                       : VT_SEARCHES_MAX - 1);
    for (int i = shift_from; i > 0; i--)
        memcpy(g_searches[i], g_searches[i - 1], sizeof g_searches[0]);
    copy_utf8(g_searches[0], sizeof g_searches[0], query);
    save();
}

void vt_store_clear_personal(void) {
    g_history_count = 0;
    g_search_count = 0;
    save();
}

int vt_store_channel_sort(void) { return g_channel_sort; }

void vt_store_set_channel_sort(int sort) {
    if (sort == g_channel_sort) return;
    g_channel_sort = sort;
    save();
}
