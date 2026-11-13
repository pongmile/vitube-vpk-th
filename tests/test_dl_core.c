/* Host-side tests for the download engine (src/dl/dl_core.c, dl_index.c,
 * dl_mp4.c) against a fake memory card and a fake YouTube.
 *
 * The engine is the real code; everything it touches is a fake that can be
 * told to misbehave at an exact byte: the connection drops after 1,234,567
 * bytes, the URL expires between two chunks, the card is full after 5,000,000
 * bytes, the server answers 200 to a Range request. Time is a variable and
 * the "worker" is the test calling dl_core_step(), so every case is
 * deterministic and runs in milliseconds.
 *
 * The fake also checks what the engine promises about itself: no file or
 * network call is made with the core lock held (the UI thread must never wait
 * behind a socket), the lock is never taken recursively, every path stays
 * inside the downloads directory with a plain file name, nothing is written
 * past the end of a file (a hole of zeros in the middle of a video), no file
 * handle is left open, and rename never relies on replacing a file.
 *
 *   gcc -Isrc -Iexternal/jansson -Wall -Wextra -Werror -o /tmp/tdl \
 *       tests/test_dl_core.c src/dl/dl_core.c src/dl/dl_index.c \
 *       src/dl/dl_mp4.c <jansson objects> && /tmp/tdl
 *
 * (CI builds jansson's sources without -Werror first, see build.yml.) */
#include "dl/dl_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

#define MIB (1024LL * 1024)
#define SRC_LEN (9 * MIB + 12345) /* three chunks: 4 MiB, 4 MiB, the rest */
#define ID_A "aaaaaaaaaaa"
#define ID_B "bbbbbbbbbbb"
#define ID_C "ccccccccccc"
#define DIR "ux0:data/ViTube/downloads/"

/* --- the fake world -------------------------------------------------------- */

typedef struct {
    char path[128];
    unsigned char *data;
    int64_t size, cap;
} FFile;

struct World;
typedef void (*Hook)(struct World *w);

typedef struct World {
    DlCore *core;

    /* card */
    FFile files[64];
    int nfiles;
    struct { int file, mode; } fds[16];
    int64_t card_total;
    int64_t extra_used;       /* what other apps hold: reported, not stored */
    int64_t write_budget;     /* bytes the card still accepts, -1 = no limit */
    int mutations;            /* count of mutating fs ops */
    int crash_after;          /* >= 0: mutating ops after this many do nothing */
    int fail_rename_to_json;
    int part_wipes;           /* unlink or truncate of a .part */
    int fs_syncs;

    /* invariants */
    int lock_depth, lock_violations, lock_reentry, lock_imbalance;
    long lock_count;
    int bad_path, hole_writes, rename_over_existing, leaked_fds;

    /* clock and network state */
    uint64_t now;
    int connected;
    unsigned gen;
    char logbuf[4096];
    int verbose;

    /* the server */
    unsigned char *src;
    int64_t src_len;
    int token;                /* URLs carrying another token answer 403 */
    int fmt_count;
    int fmt_itag[4], fmt_height[4];
    int64_t fmt_len[4];
    int is_live;
    char playability[16];
    int resolve_result;       /* 0 or a DL_NET_ code for every resolve */
    int resolve_calls;
    int fetch_all_fail;       /* every media fetch: DL_NET_TRANSPORT */
    int64_t piece;            /* body delivered in pieces of this size */
    int64_t rate;             /* bytes per second, advances the clock */
    int64_t drop_after;       /* one shot: cut the next response here */
    int64_t hook_at;          /* one shot: call hook after this many body bytes */
    Hook hook;
    Hook on_piece;            /* called after every piece of body */
    int ignore_range;
    int kill_now;             /* one shot: the response dies at the next piece */
    int reject;               /* every media request answers 403 */
    int bad_crange;           /* one shot: Content-Range starts one byte late */
    int force_status;         /* one shot: answer this status */
    int64_t overrun;          /* one shot: send this much past the range */
    int swap_src_at_total;
    int hide_clen;            /* no Content-Length header */
    int nreq;
    int64_t req_first[256], req_last[256];
    int req_status[256];
    char req_id[256][12];
    int thumb_fetches, thumb_status;
    unsigned char *thumb;
    int64_t thumb_len;
    int64_t thumb_cut;        /* >= 0: deliver only this much of the thumbnail */
    int hide_thumb_clen;
    char thumb_req[160];
    char live_id[12];         /* this video resolves as a live stream */
    int flag_stale;           /* connected flag says no, the network works */
    Hook resolve_hook;        /* one shot, inside the next resolve */
    Hook open_read_hook;      /* one shot, when a .part is opened for reading */
    int64_t max_write, max_read;
    int64_t max_part;         /* biggest a .part ever was */
    int64_t max_src;          /* longest video ever served */
    int thumb_fail_transport;
    char watch[12];           /* count .mp4 mutations of this video only */
    int mp4_mutations;        /* creations, renames and removals of its .mp4 */
    long fs_ops, net_ops;
} World;

static void world_check_path(World *w, const char *path) {
    static const char pre[] = "ux0:data/ViTube/downloads";
    size_t n = sizeof pre - 1;
    int ok = strncmp(path, pre, n) == 0 && (path[n] == '\0' || path[n] == '/');
    if (ok && path[n] == '/') {
        const char *name = path + n + 1;
        ok = *name != '\0' && strchr(name, '/') == NULL && name[0] != '.';
    }
    for (const char *p = path; ok && *p; p++) {
        char ch = *p;
        ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
             (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
             ch == '/' || ch == ':';
        if (ok && ch == '.' && p[1] == '.') ok = 0;
    }
    if (!ok) {
        w->bad_path++;
        fprintf(stderr, "BAD PATH '%s'\n", path);
    }
}

static FFile *ffind(World *w, const char *path) {
    for (int i = 0; i < w->nfiles; i++)
        if (strcmp(w->files[i].path, path) == 0) return &w->files[i];
    return NULL;
}

static void fremove(World *w, FFile *f) {
    free(f->data);
    *f = w->files[--w->nfiles];
}

static FFile *fcreate(World *w, const char *path) {
    FFile *f = &w->files[w->nfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    return f;
}

static int64_t used_by_files(World *w) {
    int64_t sum = 0;
    for (int i = 0; i < w->nfiles; i++) sum += w->files[i].size;
    return sum;
}

#define FS_ENTER(w, path) do { \
    (w)->fs_ops++; \
    if ((w)->lock_depth) (w)->lock_violations++; \
    if (path) world_check_path((w), (path)); \
} while (0)

static void note_mp4(World *w, const char *path) {
    if (strstr(path, ".mp4") && (!w->watch[0] || strstr(path, w->watch)))
        w->mp4_mutations++;
}

/* A crash: from now on nothing the engine does reaches the card. */
static int crashed(World *w) {
    if (w->crash_after < 0) return 0;
    if (w->mutations >= w->crash_after) return 1;
    w->mutations++;
    return 0;
}

static int f_mkdir(void *u, const char *path) {
    World *w = u;
    FS_ENTER(w, path);
    return 0;
}

static int f_open(void *u, const char *path, int mode) {
    World *w = u;
    FS_ENTER(w, path);
    FFile *f = ffind(w, path);
    if (!(mode & DL_FS_WRITE) && f && strstr(path, ".part") && w->open_read_hook) {
        Hook h = w->open_read_hook;
        w->open_read_hook = NULL;
        h(w);
        f = ffind(w, path); /* it may have been deleted from under us */
    }
    if (mode & DL_FS_WRITE) {
        if (crashed(w)) return DL_FS_IO;
        note_mp4(w, path);
        if (!f) f = fcreate(w, path);
        if (mode & DL_FS_TRUNC) {
            f->size = 0;
            if (strstr(path, ".part")) w->part_wipes++;
        }
    } else if (!f) {
        return DL_FS_IO;
    }
    for (int i = 0; i < 16; i++)
        if (w->fds[i].file == 0) {
            w->fds[i].file = 1;
            w->fds[i].mode = (int)(f - w->files);
            return i;
        }
    return DL_FS_IO;
}

static int f_close(void *u, int fd) {
    World *w = u;
    FS_ENTER(w, NULL);
    if (fd < 0 || fd >= 16 || !w->fds[fd].file) return DL_FS_IO;
    w->fds[fd].file = 0;
    return 0;
}

static int64_t f_pread(void *u, int fd, void *buf, size_t len, int64_t off) {
    World *w = u;
    FS_ENTER(w, NULL);
    if (fd < 0 || fd >= 16 || !w->fds[fd].file) return DL_FS_IO;
    FFile *f = &w->files[w->fds[fd].mode];
    if ((int64_t)len > w->max_read) w->max_read = (int64_t)len;
    if (off >= f->size) return 0;
    int64_t n = f->size - off;
    if (n > (int64_t)len) n = (int64_t)len;
    memcpy(buf, f->data + off, (size_t)n);
    return n;
}

static int64_t f_pwrite(void *u, int fd, const void *buf, size_t len,
                        int64_t off) {
    World *w = u;
    FS_ENTER(w, NULL);
    if (fd < 0 || fd >= 16 || !w->fds[fd].file) return DL_FS_IO;
    if (crashed(w)) return DL_FS_IO;
    FFile *f = &w->files[w->fds[fd].mode];
    if ((int64_t)len > w->max_write) w->max_write = (int64_t)len;
    if (off > f->size) w->hole_writes++;
    int64_t n = (int64_t)len;
    int budgeted = w->write_budget >= 0 && strstr(f->path, ".part") != NULL;
    if (budgeted) {
        int64_t grow = off + n > f->size ? off + n - f->size : 0;
        if (grow > w->write_budget) n -= grow - w->write_budget;
        if (n <= 0) return DL_FS_NOSPC;
    }
    if (off + n + 1 > f->cap) {
        int64_t cap = (off + n) * 2 + 4096;
        f->data = realloc(f->data, (size_t)cap);
        memset(f->data + f->cap, 0, (size_t)(cap - f->cap));
        f->cap = cap;
    }
    if (off > f->size) memset(f->data + f->size, 0, (size_t)(off - f->size));
    memcpy(f->data + off, buf, (size_t)n);
    if (off + n > f->size) {
        if (budgeted) w->write_budget -= off + n - f->size;
        f->size = off + n;
        if (strstr(f->path, ".part") && f->size > w->max_part) w->max_part = f->size;
    }
    return n;
}

static int f_sync(void *u, int fd) {
    World *w = u;
    FS_ENTER(w, NULL);
    (void)fd;
    w->fs_syncs++;
    return 0;
}

static int64_t f_size(void *u, const char *path) {
    World *w = u;
    FS_ENTER(w, path);
    FFile *f = ffind(w, path);
    return f ? f->size : -1;
}

static int f_rename(void *u, const char *from, const char *to) {
    World *w = u;
    FS_ENTER(w, from);
    world_check_path(w, to);
    if (crashed(w)) return DL_FS_IO;
    FFile *f = ffind(w, from);
    if (!f) return DL_FS_IO;
    note_mp4(w, from);
    note_mp4(w, to);
    if (ffind(w, to)) {
        w->rename_over_existing++;
        return DL_FS_IO;
    }
    if (w->fail_rename_to_json && strcmp(to, DIR "index.json") == 0) {
        w->fail_rename_to_json = 0;
        return DL_FS_IO;
    }
    snprintf(f->path, sizeof f->path, "%s", to);
    return 0;
}

static int f_unlink(void *u, const char *path) {
    World *w = u;
    FS_ENTER(w, path);
    if (crashed(w)) return DL_FS_IO;
    FFile *f = ffind(w, path);
    if (f) {
        if (strstr(path, ".part")) w->part_wipes++;
        note_mp4(w, path);
        fremove(w, f);
    }
    return 0;
}

static int f_free(void *u, int64_t *total, int64_t *free_bytes) {
    World *w = u;
    FS_ENTER(w, NULL);
    *total = w->card_total;
    *free_bytes = w->card_total - w->extra_used - used_by_files(w);
    return 0;
}

static void f_lock(void *u) {
    World *w = u;
    if (w->lock_depth) w->lock_reentry++;
    w->lock_depth++;
    w->lock_count++;
}

static void f_unlock(void *u) {
    World *w = u;
    if (w->lock_depth <= 0) w->lock_imbalance++;
    w->lock_depth--;
}

static uint64_t f_now(void *u) { return ((World *)u)->now; }
static int f_connected(void *u) { return ((World *)u)->connected; }

/* A minimal JPEG: SOI, filler, EOI. */
static void set_thumb(World *w, int64_t len) {
    free(w->thumb);
    w->thumb = malloc((size_t)len);
    memset(w->thumb, 0x55, (size_t)len);
    w->thumb[0] = 0xFF;
    w->thumb[1] = 0xD8;
    w->thumb[len - 2] = 0xFF;
    w->thumb[len - 1] = 0xD9;
    w->thumb_len = len;
}
static unsigned f_gen(void *u) { return ((World *)u)->gen; }

static void f_log(void *u, const char *line) {
    World *w = u;
    if (w->lock_depth) w->lock_violations++; /* logging is deferred, never locked */
    if (w->verbose) fprintf(stderr, "    [%6llu] %s\n", (unsigned long long)w->now, line);
}

static int f_resolve(void *u, const char *id, volatile int *cancel,
                     yt_stream_t *out) {
    World *w = u;
    if (w->lock_depth) w->lock_violations++;
    w->resolve_calls++;
    w->net_ops++;
    if (*cancel) return DL_NET_CANCELLED;
    if (!w->connected && !w->flag_stale) return DL_NET_TRANSPORT;
    if (w->resolve_hook) {
        Hook h = w->resolve_hook;
        w->resolve_hook = NULL;
        h(w);
    }
    if (w->resolve_result) return w->resolve_result;
    memset(out, 0, sizeof *out);
    snprintf(out->video_id, sizeof out->video_id, "%s", id);
    snprintf(out->playability, sizeof out->playability, "%s", w->playability);
    out->is_live = w->is_live || strcmp(id, w->live_id) == 0;
    out->format_count = w->fmt_count;
    for (int i = 0; i < w->fmt_count; i++) {
        out->formats[i].itag = w->fmt_itag[i];
        out->formats[i].height = w->fmt_height[i];
        out->formats[i].content_length = w->fmt_len[i];
        snprintf(out->formats[i].url, sizeof out->formats[i].url,
                 "https://gv.example/videoplayback?v=%s&itag=%d&t=%d", id,
                 w->fmt_itag[i], w->token);
    }
    return DL_NET_OK;
}

static int f_fetch(void *u, const char *url, int64_t first, int64_t last,
                   const DlSink *sink, volatile int *cancel) {
    World *w = u;
    if (w->lock_depth) w->lock_violations++;
    w->net_ops++;
    if (!w->connected && !w->flag_stale) return DL_NET_TRANSPORT;

    if (strstr(url, "/vi/")) {
        w->thumb_fetches++;
        if (w->thumb_fail_transport) return DL_NET_TRANSPORT;
        snprintf(w->thumb_req, sizeof w->thumb_req, "%s", url);
        if (w->thumb_status != 200) {
            if (sink->headers(sink->ctx, w->thumb_status, 0, "")) return DL_NET_ABORTED;
            return DL_NET_OK;
        }
        if (sink->headers(sink->ctx, 200, w->hide_thumb_clen ? -1 : w->thumb_len, ""))
            return DL_NET_ABORTED;
        int64_t send = w->thumb_len;
        if (w->thumb_cut >= 0 && w->thumb_cut < send) send = w->thumb_cut;
        for (int64_t at = 0; at < send;) {
            int64_t k = send - at > 8192 ? 8192 : send - at;
            if (sink->data(sink->ctx, w->thumb + at, (size_t)k)) return DL_NET_ABORTED;
            at += k;
        }
        return DL_NET_OK;
    }

    int n = w->nreq < 256 ? w->nreq++ : 255;
    w->req_first[n] = first;
    w->req_last[n] = last;
    const char *v = strstr(url, "v=");
    memcpy(w->req_id[n], v ? v + 2 : "?", 11);
    w->req_id[n][11] = '\0';
    if (w->fetch_all_fail) return DL_NET_TRANSPORT;

    const char *t = strstr(url, "t=");
    int token = t ? atoi(t + 2) : -1;
    int status;
    int64_t body_first = 0, body_len = 0;
    char crange[96] = "";
    int64_t src_len = w->src_len;
    if (w->force_status) {
        status = w->force_status;
        w->force_status = 0;
    } else if (token != w->token || w->reject) {
        status = 403;
    } else if (w->ignore_range || first < 0) {
        status = 200;
        body_len = src_len;
    } else if (first >= src_len) {
        status = 416;
    } else {
        int64_t end = last < src_len - 1 ? last : src_len - 1;
        status = 206;
        body_first = first;
        body_len = end - first + 1;
        snprintf(crange, sizeof crange, "bytes %lld-%lld/%lld",
                 (long long)(first + (w->bad_crange ? 1 : 0)), (long long)end,
                 (long long)src_len);
        w->bad_crange = 0;
    }
    w->req_status[n] = status;
    int64_t clen = w->hide_clen ? -1 : body_len;
    if (sink->headers(sink->ctx, status, clen, crange)) return DL_NET_ABORTED;
    if (status >= 400) return DL_NET_OK; /* never reached: the core refuses */

    int64_t pos = 0, drop = w->drop_after, hook_at = w->hook_at;
    int64_t total_to_send = body_len + w->overrun;
    w->drop_after = -1;
    w->overrun = 0;
    while (pos < total_to_send) {
        if (*cancel) return DL_NET_CANCELLED;
        if (w->kill_now) {
            w->kill_now = 0;
            return DL_NET_TRANSPORT;
        }
        int64_t k = total_to_send - pos;
        if (k > w->piece) k = w->piece;
        if (drop >= 0 && pos + k > drop) k = drop - pos;
        if (k <= 0) break;
        const unsigned char *from = w->src + body_first + pos;
        unsigned char junk[64] = {0};
        if (pos >= body_len) from = junk, k = k > 64 ? 64 : k;
        if (sink->data(sink->ctx, from, (size_t)k)) return DL_NET_ABORTED;
        pos += k;
        w->now += (uint64_t)(k * 1000 / w->rate);
        if (w->on_piece) w->on_piece(w);
        if (hook_at >= 0 && pos >= hook_at && w->hook) {
            Hook h = w->hook;
            w->hook = NULL;
            w->hook_at = -1;
            hook_at = -1;
            h(w);
        }
        if (drop >= 0 && pos >= drop) return DL_NET_TRANSPORT;
    }
    return DL_NET_OK;
}

static void make_source(World *w, int64_t len, unsigned seed, int with_moov) {
    free(w->src);
    w->src = malloc((size_t)len);
    w->src_len = len;
    if (len > w->max_src) w->max_src = len;
    unsigned x = seed * 2654435761u + 1;
    for (int64_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        w->src[i] = (unsigned char)(x >> 24);
    }
    /* ftyp (24) + moov (2000) + mdat (rest). Payload bytes keep the high bit
     * so a box walk that strays into one finds no printable type. */
    unsigned char *p = w->src;
    memcpy(p, "\0\0\0\x18" "ftypisom\0\0\2\0isomiso2", 24);
    p += 24;
    int64_t moov = with_moov ? 2000 : 0;
    if (with_moov) {
        p[0] = 0; p[1] = 0; p[2] = 0x07; p[3] = 0xD0;
        memcpy(p + 4, "moov", 4);
        for (int i = 8; i < 2000; i++) p[i] |= 0x80;
        p += moov;
    }
    int64_t mdat = len - 24 - moov;
    p[0] = (unsigned char)(mdat >> 24); p[1] = (unsigned char)(mdat >> 16);
    p[2] = (unsigned char)(mdat >> 8); p[3] = (unsigned char)mdat;
    memcpy(p + 4, "mdat", 4);
}

static World *world_new(void) {
    World *w = calloc(1, sizeof *w);
    w->card_total = 4096 * MIB;
    w->write_budget = -1;
    w->crash_after = -1;
    w->now = 1000000;
    w->connected = 1;
    w->gen = 1;
    w->token = 1;
    w->fmt_count = 1;
    w->fmt_itag[0] = 18;
    w->fmt_height[0] = 360;
    snprintf(w->playability, sizeof w->playability, "OK");
    w->piece = 16 * 1024;
    w->rate = 2 * MIB;
    w->drop_after = -1;
    w->hook_at = -1;
    w->thumb_status = 200;
    w->thumb_cut = -1;
    set_thumb(w, 40);
    make_source(w, SRC_LEN, 1, 1);
    w->fmt_len[0] = SRC_LEN;
    return w;
}

static DlCore *core_new(World *w) {
    DlOps ops = {
        .user = w,
        .lock = f_lock, .unlock = f_unlock,
        .now_ms = f_now, .net_connected = f_connected, .net_generation = f_gen,
        .log = f_log,
        .fs_mkdir = f_mkdir, .fs_open = f_open, .fs_close = f_close,
        .fs_pread = f_pread, .fs_pwrite = f_pwrite, .fs_sync = f_sync,
        .fs_size = f_size, .fs_rename = f_rename, .fs_unlink = f_unlink,
        .fs_free = f_free,
        .net_resolve = f_resolve, .net_fetch = f_fetch,
    };
    DlCore *c = calloc(1, sizeof *c);
    dl_core_init(c, &ops);
    dl_core_load(c);
    w->core = c;
    return c;
}

static void world_free(World *w) {
    for (int i = 0; i < 16; i++)
        if (w->fds[i].file) w->leaked_fds++;
    CHECK(w->leaked_fds == 0, "%d file handles left open", w->leaked_fds);
    CHECK(w->lock_violations == 0, "%d fs/net calls with the core lock held",
          w->lock_violations);
    CHECK(w->lock_reentry == 0, "the core lock was taken recursively");
    CHECK(w->lock_imbalance == 0 && w->lock_depth == 0, "lock unbalanced (%d)",
          w->lock_depth);
    CHECK(w->bad_path == 0, "%d paths outside the downloads directory", w->bad_path);
    CHECK(w->hole_writes == 0, "%d writes past the end of a file", w->hole_writes);
    CHECK(w->rename_over_existing == 0, "rename relied on replacing a file");
    CHECK(w->max_part <= w->max_src, "a .part grew to %lld bytes, the video is at most %lld",
          (long long)w->max_part, (long long)w->max_src);
    CHECK(w->max_write <= DL_WBUF_BYTES, "a write of %lld bytes under the I/O lock",
          (long long)w->max_write);
    CHECK(w->max_read <= DL_WBUF_BYTES, "a read of %lld bytes under the I/O lock",
          (long long)w->max_read);
    for (int i = 0; i < w->nfiles; i++) free(w->files[i].data);
    free(w->thumb);
    free(w->src);
    free(w->core);
    free(w);
}

/* --- drivers and small helpers --------------------------------------------- */

static yt_video_t video(const char *id, const char *title) {
    yt_video_t v;
    memset(&v, 0, sizeof v);
    snprintf(v.video_id, sizeof v.video_id, "%s", id);
    snprintf(v.title, sizeof v.title, "%s", title);
    snprintf(v.channel, sizeof v.channel, "Channel");
    snprintf(v.duration, sizeof v.duration, "12:34");
    return v;
}

static DlEnqueue enq(World *w, const char *id, const char *title) {
    yt_video_t v = video(id, title);
    return dl_core_enqueue(w->core, &v);
}

static DlItem item(World *w, const char *id) {
    DlItem it;
    memset(&it, 0, sizeof it);
    dl_core_get(w->core, id, &it);
    return it;
}

static int has_item(World *w, const char *id) {
    return dl_core_get(w->core, id, NULL);
}

static int active(World *w) { return dl_core_active_count(w->core); }

/* Steps until the engine says "nothing to do until time passes or someone
 * acts". Does not move the clock. Returns the sleep it asked for. */
static int pump(World *w) {
    for (int i = 0; i < 100000; i++) {
        int r = dl_core_step(w->core);
        if (r > 0) return r;
    }
    CHECK(0, "engine never went to sleep");
    return -1;
}

/* Lets the engine run, and lets time pass whenever it sleeps for a timer,
 * until nothing is active except items waiting for an EVENT (no network,
 * online playback) — or max_ms of fake time has gone. */
static void run(World *w, int64_t max_ms) {
    uint64_t end = w->now + (uint64_t)max_ms;
    while (w->now < end) {
        int r = pump(w);
        DlItem snap[DL_MAX_ITEMS];
        int n = dl_core_snapshot(w->core, snap, DL_MAX_ITEMS);
        int busy = 0;
        for (int i = 0; i < n; i++) {
            DlState s = snap[i].state;
            if (s == DL_QUEUED || s == DL_RESOLVING || s == DL_DOWNLOADING ||
                s == DL_VERIFYING || (s == DL_WAITING && snap[i].retry_in_s >= 0))
                busy = 1;
        }
        if (!busy) return;
        w->now += (uint64_t)r;
    }
}

/* Steps (moving the clock when it sleeps) until the item reaches `state`. */
static int run_until(World *w, const char *id, DlState state, int64_t max_ms) {
    uint64_t end = w->now + (uint64_t)max_ms;
    for (;;) {
        DlItem it;
        if (!dl_core_get(w->core, id, &it)) return 0;
        if (it.state == state) return 1;
        if (w->now >= end) return 0;
        int r = dl_core_step(w->core);
        if (r > 0) w->now += (uint64_t)r;
    }
}

/* Steps until bytes_done reaches `bytes` (state unchanged otherwise). */
static int run_until_bytes(World *w, const char *id, int64_t bytes) {
    for (int i = 0; i < 100000; i++) {
        DlItem it = item(w, id);
        if (it.bytes_done >= bytes) return 1;
        int r = dl_core_step(w->core);
        if (r > 0) w->now += (uint64_t)r;
    }
    return 0;
}

static FFile *file_of(World *w, const char *id, const char *ext) {
    char path[160];
    snprintf(path, sizeof path, DIR "%s.%s", id, ext);
    return ffind(w, path);
}

static int file_equals_src(World *w, const char *id, const char *ext) {
    FFile *f = file_of(w, id, ext);
    return f && f->size == w->src_len && memcmp(f->data, w->src, (size_t)w->src_len) == 0;
}

static void put_file(World *w, const char *path, const void *data, int64_t len) {
    FFile *f = ffind(w, path);
    if (!f) f = fcreate(w, path);
    free(f->data);
    f->data = calloc((size_t)len + 1, 1);
    memcpy(f->data, data, (size_t)len);
    f->size = len;
    f->cap = len + 1;
}

static void put_text(World *w, const char *path, const char *text) {
    put_file(w, path, text, (int64_t)strlen(text));
}

static void put_part(World *w, const char *id, int64_t bytes) {
    char path[160];
    snprintf(path, sizeof path, DIR "%s.part", id);
    put_file(w, path, w->src, bytes);
}

/* Full download of one item from a fresh world. */
static void download_all(World *w, const char *id) {
    CHECK(enq(w, id, "Video") == DL_ENQ_OK, "enqueue");
    run(w, 600000);
}

/* A fresh engine over the same card, as after an app restart. */
static void restart_engine(World *w) {
    dl_core_flush(w->core);
    free(w->core);
    core_new(w);
}

/* --- the tests ------------------------------------------------------------- */

static void test_error_keys(void) {
    CHECK(strcmp(dl_error_key(DL_ERR_NONE), "none") == 0, "none");
    CHECK(strcmp(dl_error_key(DL_ERR_NO_SPACE), "no_space") == 0, "no_space");
    CHECK(strcmp(dl_error_key(DL_ERR_DISK_FULL), "disk_full") == 0, "disk_full");
    CHECK(strcmp(dl_error_key(DL_ERR_NOT_DOWNLOADABLE), "not_downloadable") == 0, "nd");
    CHECK(strcmp(dl_error_key(DL_ERR_VERIFY), "verify") == 0, "verify");
    CHECK(strcmp(dl_error_key(DL_ERR_IO), "io") == 0, "io");
    CHECK(strcmp(dl_error_key(DL_ERR_NETWORK), "network") == 0, "network");
    CHECK(strcmp(dl_error_key(DL_ERR_URL), "url") == 0, "url");
}

static void test_valid_id(void) {
    CHECK(dl_valid_id("dQw4w9WgXcQ"), "real id");
    CHECK(dl_valid_id("-_aZ09-_aZ0"), "dash underscore");
    CHECK(!dl_valid_id(NULL), "NULL");
    CHECK(!dl_valid_id(""), "empty");
    CHECK(!dl_valid_id("dQw4w9WgXc"), "10 chars");
    CHECK(!dl_valid_id("dQw4w9WgXcQQ"), "12 chars");
    CHECK(!dl_valid_id("../x"), "dotdot");
    CHECK(!dl_valid_id("..//..//.."), "slashes");
    CHECK(!dl_valid_id("dQw4w9WgXc/"), "slash");
    CHECK(!dl_valid_id("dQw4w9WgXc."), "dot");
    CHECK(!dl_valid_id("dQw4w9 gXcQ"), "space");
    CHECK(!dl_valid_id("dQw4w9WgXc\n"), "newline");
    CHECK(!dl_valid_id("dQw4w9WgX\xC3\xA9"), "utf-8");
    CHECK(!dl_valid_id("dQw4w9WgXc%"), "percent");
}

static void test_fresh_download(void) {
    World *w = world_new();
    core_new(w);
    yt_video_t v = video(ID_A, "A fine video");
    snprintf(v.thumb_url, sizeof v.thumb_url, "https://i.example/thumb/a.jpg");
    unsigned rev0 = dl_core_revision(w->core);
    CHECK(dl_core_enqueue(w->core, &v) == DL_ENQ_OK, "enqueue");
    CHECK(dl_core_count(w->core) == 1, "one item");
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_QUEUED, "queued first (%d)", it.state);
    CHECK(strcmp(it.title, "A fine video") == 0, "title kept");
    CHECK(dl_core_revision(w->core) != rev0, "revision bumped by enqueue");
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "state %d error %d", it.state, it.error);
    CHECK(it.bytes_done == SRC_LEN && it.bytes_total == SRC_LEN, "bytes %lld/%lld",
          (long long)it.bytes_done, (long long)it.bytes_total);
    CHECK(it.height == 360, "height recorded");
    CHECK(it.bytes_per_sec == 0 && it.retry_in_s == 0, "idle fields cleared");
    CHECK(file_equals_src(w, ID_A, "mp4"), "mp4 equals the source");
    CHECK(file_of(w, ID_A, "part") == NULL, "no .part left");
    CHECK(file_of(w, ID_A, "jpg") != NULL, "thumbnail saved");
    CHECK(file_of(w, ID_A, "jpg.tmp") == NULL, "no thumbnail tmp left");
    CHECK(w->thumb_fetches == 1, "thumbnail fetched once (%d)", w->thumb_fetches);
    CHECK(strcmp(w->thumb_req, "https://i.ytimg.com/vi/" ID_A "/mqdefault.jpg") == 0,
          "the 16:9 thumbnail, not the list's 4:3 one: %s", w->thumb_req);
    CHECK(w->nreq == 3, "three ranged requests (%d)", w->nreq);
    CHECK(w->req_first[0] == 0 && w->req_last[0] == 4 * MIB - 1, "first range");
    CHECK(w->req_first[1] == 4 * MIB && w->req_last[1] == 8 * MIB - 1, "second range");
    CHECK(w->req_first[2] == 8 * MIB, "third range starts where the second ended");
    CHECK(w->resolve_calls == 1, "resolved once");
    CHECK(active(w) == 0, "nothing active");
    char path[128], thumb[128];
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 1, "local path");
    CHECK(strcmp(path, DIR ID_A ".mp4") == 0, "local path value %s", path);
    CHECK(dl_core_thumb_path(w->core, ID_A, thumb, sizeof thumb) == 1, "thumb path");
    CHECK(w->fs_syncs >= 1, "synced before the rename");
    /* The index followed. */
    FFile *idx = ffind(w, DIR "index.json");
    CHECK(idx && strstr((char *)idx->data, "\"state\":\"done\"") != NULL, "index says done");
    world_free(w);
}

/* hooks, run from inside a response at a chosen byte */
static void hook_pause_a(World *w) { dl_core_pause(w->core, ID_A); }
static void hook_delete_a(World *w) { dl_core_delete(w->core, ID_A); }
static void hook_hold(World *w) { dl_core_playback_begin(w->core); }
static void hook_stop(World *w) { dl_core_stop(w->core); }
static void hook_offline(World *w) { w->connected = 0; w->kill_now = 1; }
static void hook_network_change(World *w) { w->gen++; w->kill_now = 1; }
static void hook_card_filled(World *w) {
    w->extra_used = w->card_total - 12 * MIB;
}

static int run_until_not(World *w, const char *id, DlState state, int64_t max_ms) {
    uint64_t end = w->now + (uint64_t)max_ms;
    for (;;) {
        DlItem it;
        if (!dl_core_get(w->core, id, &it)) return 0;
        if (it.state != state) return 1;
        if (w->now >= end) return 0;
        int r = dl_core_step(w->core);
        if (r > 0) w->now += (uint64_t)r;
    }
}

static void test_pause_resume_exact_byte(void) {
    World *w = world_new();
    core_new(w);
    w->piece = 10007; /* nothing lines up with a power of two */
    w->hook_at = 1000000;
    w->hook = hook_pause_a;
    enq(w, ID_A, "A");
    CHECK(run_until(w, ID_A, DL_PAUSED, 60000), "paused");
    pump(w);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_PAUSED, "state %d", it.state);
    int64_t got = it.bytes_done;
    CHECK(got >= 1000000 && got < 1000000 + 10007, "stopped at %lld", (long long)got);
    FFile *p = file_of(w, ID_A, "part");
    CHECK(p && p->size == got, "the part file holds exactly bytes_done");
    CHECK(p && memcmp(p->data, w->src, (size_t)got) == 0, "and they are the right bytes");
    int reqs = w->nreq;
    for (int i = 0; i < 50; i++) { pump(w); w->now += 1000; }
    CHECK(w->nreq == reqs, "a paused item never starts by itself");
    CHECK(active(w) == 0, "paused is not active");
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "done after resume (%d)", it.state);
    CHECK(w->req_first[reqs] == got, "resumed at the exact byte: %lld vs %lld",
          (long long)w->req_first[reqs], (long long)got);
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical to the source");
    CHECK(w->resolve_calls == 2, "a new run resolves again (%d)", w->resolve_calls);
    world_free(w);
}

static void test_drop_mid_chunk(void) {
    World *w = world_new();
    core_new(w);
    w->drop_after = 1234567;
    enq(w, ID_A, "A");
    CHECK(run_until(w, ID_A, DL_WAITING, 60000), "waiting after the cut");
    DlItem it = item(w, ID_A);
    CHECK(it.bytes_done == 1234567, "kept every byte that arrived: %lld",
          (long long)it.bytes_done);
    CHECK(it.retry_in_s == 1, "first retry in 1 s (%d)", it.retry_in_s);
    FFile *p = file_of(w, ID_A, "part");
    CHECK(p && p->size == 1234567, "on the card too");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "finished");
    CHECK(w->req_first[1] == 1234567, "next request at the exact byte (%lld)",
          (long long)w->req_first[1]);
    CHECK(w->req_last[1] == 1234567 + 4 * MIB - 1, "a full chunk from there");
    CHECK(file_equals_src(w, ID_A, "mp4"), "no gap, no duplicate");
    world_free(w);
}

static void test_url_expiry(void) {
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    CHECK(run_until_bytes(w, ID_A, 4 * MIB), "first chunk");
    w->token++; /* googlevideo no longer knows this URL */
    run(w, 600000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "done (%d/%d)", it.state, it.error);
    CHECK(w->resolve_calls == 2, "resolved again (%d)", w->resolve_calls);
    CHECK(w->req_status[1] == 403 && w->req_first[1] == 4 * MIB, "the 403");
    CHECK(w->req_first[2] == 4 * MIB && w->req_status[2] == 206,
          "retried at the same offset with the new URL");
    CHECK(w->part_wipes == 0, "same length: continued, did not start over");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical to the source");
    world_free(w);

    /* Same height under another itag is the same file. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    w->token++;
    w->fmt_itag[0] = 134;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->part_wipes == 0,
          "renamed itag, same height and length: continued");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* Neither itag nor height: another rendition, start over. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    w->token++;
    w->fmt_itag[0] = 22;
    w->fmt_height[0] = 720;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->part_wipes >= 1,
          "another rendition: restarted");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    CHECK(item(w, ID_A).height == 720, "records the new height");
    world_free(w);

    /* Three 403s in a row on fresh URLs: the user is told. */
    w = world_new();
    core_new(w);
    w->reject = 1;
    enq(w, ID_A, "A");
    run(w, 600000);
    DlItem f = item(w, ID_A);
    CHECK(f.state == DL_FAILED && f.error == DL_ERR_URL, "failed: url (%d/%d)",
          f.state, f.error);
    CHECK(w->resolve_calls == DL_URL_FAIL_LIMIT, "resolves %d", w->resolve_calls);
    world_free(w);
}

static void test_url_age(void) {
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    w->now += DL_URL_MAX_AGE_MS + 1; /* nothing is wrong with the URL yet */
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(w->resolve_calls == 2, "renewed before it could fail (%d)", w->resolve_calls);
    CHECK(w->req_status[1] == 206 && w->nreq == 3, "no 403 ever happened");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);
}

static void test_new_length_restarts(void) {
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    make_source(w, 6 * MIB + 77, 2, 1);
    w->fmt_len[0] = w->src_len;
    w->token++;
    run(w, 600000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "done (%d/%d)", it.state, it.error);
    CHECK(it.bytes_total == 6 * MIB + 77, "new length recorded");
    CHECK(w->part_wipes >= 1, "started over");
    CHECK(file_equals_src(w, ID_A, "mp4"), "the NEW file, whole");
    world_free(w);

    /* The file changes under the same URL and the resolve cannot tell: the
     * Content-Range total gives it away. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    make_source(w, 6 * MIB + 77, 3, 1); /* fmt_len still says SRC_LEN */
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "done (%d/%d)", it.state, it.error);
    CHECK(file_equals_src(w, ID_A, "mp4"), "the new file, whole");
    world_free(w);
}

static void test_range_ignored_or_wrong(void) {
    /* 200 to a request that starts mid-file. */
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    w->ignore_range = 1;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(w->req_status[1] == 200 && w->req_first[1] == 4 * MIB, "the refused 200");
    CHECK(w->req_first[2] == 0 && w->req_status[2] == 200 && w->nreq == 3,
          "then from 0, taking the 200 as the whole file");
    CHECK(w->part_wipes >= 1, "stored part discarded");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* 200 to the very first request is simply the whole file. */
    w = world_new();
    core_new(w);
    w->ignore_range = 1;
    enq(w, ID_A, "A");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->nreq == 1, "one request");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* 416 in the middle of the file. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    run_until_bytes(w, ID_A, 4 * MIB);
    w->force_status = 416;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->part_wipes >= 1, "restarted");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* A Content-Range that is not the range asked for: nothing is written. */
    w = world_new();
    core_new(w);
    w->bad_crange = 1;
    enq(w, ID_A, "A");
    CHECK(run_until(w, ID_A, DL_WAITING, 60000), "refused");
    DlItem it = item(w, ID_A);
    CHECK(it.bytes_done == 0, "no byte accepted");
    FFile *p = file_of(w, ID_A, "part");
    CHECK(!p || p->size == 0, "none on the card");
    CHECK(it.retry_in_s == 1, "retried after a second");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->nreq == 4, "then fine (%d requests)", w->nreq);
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* An answer longer than it promised: only the promise is kept. */
    w = world_new();
    core_new(w);
    w->overrun = 100;
    enq(w, ID_A, "A");
    CHECK(run_until(w, ID_A, DL_WAITING, 60000), "refused the extra bytes");
    CHECK(item(w, ID_A).bytes_done == 4 * MIB, "exactly the first chunk");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* The resolve carried no length: the first Content-Range does. */
    w = world_new();
    core_new(w);
    w->fmt_len[0] = 0;
    enq(w, ID_A, "A");
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_DONE && it.bytes_total == SRC_LEN, "length learned");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* No Content-Length header on the 206 is fine. */
    w = world_new();
    core_new(w);
    w->hide_clen = 1;
    enq(w, ID_A, "A");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done without Content-Length");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);
}

static void test_disk_full(void) {
    World *w = world_new();
    core_new(w);
    w->write_budget = 5000000;
    enq(w, ID_A, "A");
    run(w, 600000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_DISK_FULL, "disk_full (%d/%d)",
          it.state, it.error);
    CHECK(it.bytes_done == 5000000, "every byte the card took is counted: %lld",
          (long long)it.bytes_done);
    FFile *p = file_of(w, ID_A, "part");
    CHECK(p && p->size == 5000000, "and kept");
    CHECK(active(w) == 0, "failed is not active");
    int reqs = w->nreq;
    for (int i = 0; i < 20; i++) { pump(w); w->now += 5000; }
    CHECK(w->nreq == reqs, "does not retry by itself");
    w->write_budget = -1; /* the user freed some space */
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_DONE, "done (%d/%d)", it.state, it.error);
    CHECK(w->req_first[reqs] == 5000000, "continued at the exact byte");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* Someone else filled the card while this wrote: noticed within 8 MiB. */
    w = world_new();
    core_new(w);
    make_source(w, 12 * MIB + 5, 4, 1);
    w->fmt_len[0] = w->src_len;
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_card_filled;
    enq(w, ID_A, "A");
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_DISK_FULL, "disk_full (%d/%d)",
          it.state, it.error);
    CHECK(it.bytes_done >= 8 * MIB && it.bytes_done < w->src_len, "stopped at %lld",
          (long long)it.bytes_done);
    w->extra_used = 0;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done after space returned");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);
}

static void test_free_space_policy(void) {
    /* At enqueue: below the reserve, refuse at once. */
    World *w = world_new();
    core_new(w);
    w->extra_used = w->card_total - 100 * MIB;
    CHECK(enq(w, ID_A, "A") == DL_ENQ_FULL, "FULL below the reserve");
    CHECK(dl_core_count(w->core) == 0, "nothing queued");
    DlStorage st;
    CHECK(dl_core_storage(w->core, &st) == 0 && st.low, "storage says low");
    w->extra_used = 0;
    w->now += 5000;
    CHECK(enq(w, ID_A, "A") == DL_ENQ_OK, "fine with room");
    world_free(w);

    /* After the resolve: the size is known, one byte short fails. */
    w = world_new();
    core_new(w);
    /* 4 KiB short (the index file is on the card too). */
    w->extra_used = w->card_total - (DL_RESERVE_BYTES + SRC_LEN - 4096);
    CHECK(enq(w, ID_A, "A") == DL_ENQ_OK, "free >= reserve: accepted");
    run(w, 60000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_NO_SPACE, "no_space (%d/%d)",
          it.state, it.error);
    CHECK(it.bytes_total == SRC_LEN, "size kept so the UI can say how much");
    CHECK(w->nreq == 0, "nothing fetched");
    w->extra_used -= 16384; /* now it fits */
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done once it fits (%d)", item(w, ID_A).error);
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* With a part file on the card: it is kept. */
    w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_pause_a;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_PAUSED, 60000);
    pump(w);
    int64_t got = item(w, ID_A).bytes_done;
    w->extra_used = w->card_total - (DL_RESERVE_BYTES + SRC_LEN - got - 4096 +
                                     used_by_files(w));
    dl_core_resume(w->core, ID_A);
    run(w, 60000);
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_NO_SPACE, "no_space (%d/%d)",
          it.state, it.error);
    CHECK(it.bytes_done == got && file_of(w, ID_A, "part")->size == got,
          "the part file stays");
    w->extra_used -= 16384;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "continued");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* The length unknown until the first Content-Range. */
    w = world_new();
    core_new(w);
    w->fmt_len[0] = 0;
    w->extra_used = w->card_total - (DL_RESERVE_BYTES + SRC_LEN - 4096);
    enq(w, ID_A, "A");
    run(w, 60000);
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_NO_SPACE, "no_space (%d/%d)",
          it.state, it.error);
    CHECK(it.bytes_total == SRC_LEN && it.bytes_done == 0, "size kept, nothing stored");
    CHECK(w->nreq == 1, "one request, refused on its headers");
    world_free(w);
}

/* --- MP4 verification ------------------------------------------------------ */

typedef struct { unsigned char d[8192]; int n; } Mem;

static void m32(Mem *m, uint32_t v) {
    m->d[m->n++] = (unsigned char)(v >> 24);
    m->d[m->n++] = (unsigned char)(v >> 16);
    m->d[m->n++] = (unsigned char)(v >> 8);
    m->d[m->n++] = (unsigned char)v;
}

/* header with an explicit size field, then `payload` bytes of 0x90 */
static void mbox(Mem *m, const char *type, uint32_t size_field, int payload) {
    m32(m, size_field);
    memcpy(m->d + m->n, type, 4);
    m->n += 4;
    memset(m->d + m->n, 0x90, (size_t)payload);
    m->n += payload;
}

static int64_t mem_read(void *ctx, int64_t off, void *buf, size_t len) {
    Mem *m = ctx;
    if (off >= m->n) return 0;
    int64_t n = m->n - off;
    if (n > (int64_t)len) n = (int64_t)len;
    memcpy(buf, m->d + off, (size_t)n);
    return n;
}

static int verify_mem(Mem *m) { return dl_mp4_verify(mem_read, m, m->n); }

static void good_file(Mem *m) {
    m->n = 0;
    mbox(m, "ftyp", 24, 16);
    mbox(m, "moov", 108, 100);
    mbox(m, "mdat", 208, 200);
}

static void test_verify(void) {
    Mem m;
    good_file(&m);
    CHECK(verify_mem(&m) == DL_MP4_OK, "good file: %d", verify_mem(&m));
    CHECK(dl_mp4_verify(mem_read, &m, m.n - 10) == DL_MP4_TRUNCATED, "truncated");
    CHECK(dl_mp4_verify(mem_read, &m, 0) == DL_MP4_TOO_SMALL, "empty");
    CHECK(dl_mp4_verify(mem_read, &m, 15) == DL_MP4_TOO_SMALL, "tiny");

    good_file(&m);
    m.d[24 + 3] = 0xFF; /* moov claims 255 bytes... */
    m.d[24 + 2] = 0x10; /* ... no, 4223: far past the end */
    CHECK(verify_mem(&m) == DL_MP4_TRUNCATED, "box larger than the file");
    good_file(&m);
    m.d[24 + 3] = 12; /* moov 12 bytes: the walk lands inside its payload */
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "box too small: %d", verify_mem(&m));
    good_file(&m);
    m.d[24 + 3] = 4; /* smaller than its own header */
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "box smaller than a header");

    m.n = 0;
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "mdat", 208, 200);
    CHECK(verify_mem(&m) == DL_MP4_NO_MOOV, "no moov");
    m.n = 0;
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "moov", 108, 100);
    CHECK(verify_mem(&m) == DL_MP4_NO_MDAT, "no mdat");
    m.n = 0;
    mbox(&m, "moov", 108, 100);
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "mdat", 208, 200);
    CHECK(verify_mem(&m) == DL_MP4_NO_FTYP, "ftyp not first");
    m.n = 0;
    mbox(&m, "ftyp", 8, 0);
    mbox(&m, "moov", 108, 100);
    mbox(&m, "mdat", 208, 200);
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "ftyp with no brand");

    good_file(&m);
    memset(m.d + m.n, 0x41, 4);
    m.n += 4;
    CHECK(verify_mem(&m) == DL_MP4_TRUNCATED, "4 bytes of trailing garbage");
    good_file(&m);
    memset(m.d + m.n, 0xEE, 16);
    m.n += 16;
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "16 bytes of trailing garbage");
    good_file(&m);
    memset(m.d + m.n, 0x00, 64);
    m.n += 64;
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "a zero-filled tail is not a box");
    good_file(&m);
    mbox(&m, "free", 8, 0);
    CHECK(verify_mem(&m) == DL_MP4_OK, "an extra free box is fine");

    /* 64-bit largesize */
    m.n = 0;
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "moov", 108, 100);
    m32(&m, 1);
    memcpy(m.d + m.n, "mdat", 4);
    m.n += 4;
    m32(&m, 0);
    m32(&m, 16 + 200);
    memset(m.d + m.n, 0x90, 200);
    m.n += 200;
    CHECK(verify_mem(&m) == DL_MP4_OK, "largesize box: %d", verify_mem(&m));
    m.d[m.n - 200 - 1] = 8; /* largesize 8 < its own 16-byte header */
    CHECK(verify_mem(&m) == DL_MP4_BAD_BOX, "largesize too small");
    m.d[m.n - 200 - 1] = 0xFF; /* 255 + 16... runs off the end */
    m.d[m.n - 200 - 2] = 0x01;
    CHECK(verify_mem(&m) == DL_MP4_TRUNCATED, "largesize past the end");
    m.d[m.n - 200 - 2] = 0;
    m.d[m.n - 200 - 1] = 16 + 200 - 4; /* and one that ends short of the end */
    CHECK(verify_mem(&m) != DL_MP4_OK, "largesize ends before the end of file");

    /* size 0 = to the end of the file */
    m.n = 0;
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "moov", 108, 100);
    mbox(&m, "mdat", 0, 300);
    CHECK(verify_mem(&m) == DL_MP4_OK, "mdat to EOF");
    m.n = 0;
    mbox(&m, "ftyp", 24, 16);
    mbox(&m, "mdat", 0, 300);
    mbox(&m, "moov", 108, 100); /* swallowed by the mdat above */
    CHECK(verify_mem(&m) == DL_MP4_NO_MOOV, "size 0 box swallows what follows");
}

static void test_verify_in_core(void) {
    World *w = world_new();
    core_new(w);
    make_source(w, SRC_LEN, 5, 0); /* no moov */
    enq(w, ID_A, "A");
    run(w, 600000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_VERIFY, "verify (%d/%d)",
          it.state, it.error);
    CHECK(file_of(w, ID_A, "part") == NULL && file_of(w, ID_A, "mp4") == NULL,
          "no file left");
    CHECK(it.bytes_done == 0, "progress reset");
    char path[128];
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0, "not playable");
    make_source(w, SRC_LEN, 5, 1);
    int reqs = w->nreq;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done the second time");
    CHECK(w->req_first[reqs] == 0, "restarted from byte 0");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

}

static void test_not_downloadable(void) {
    for (int variant = 0; variant < 3; variant++) {
        World *w = world_new();
        core_new(w);
        if (variant == 0) w->is_live = 1;
        if (variant == 1) w->fmt_count = 0;
        if (variant == 2) snprintf(w->playability, sizeof w->playability, "LOGIN_REQUIRED");
        enq(w, ID_A, "Kept title");
        run(w, 60000);
        DlItem it = item(w, ID_A);
        CHECK(it.state == DL_FAILED && it.error == DL_ERR_NOT_DOWNLOADABLE,
              "variant %d: %d/%d", variant, it.state, it.error);
        CHECK(strcmp(it.title, "Kept title") == 0, "title retained");
        CHECK(w->nreq == 0, "nothing fetched");
        /* It stays failed until the user asks, and then it looks again. */
        int calls = w->resolve_calls;
        for (int i = 0; i < 10; i++) { pump(w); w->now += 10000; }
        CHECK(w->resolve_calls == calls, "no automatic retry");
        w->is_live = 0;
        w->fmt_count = 1;
        snprintf(w->playability, sizeof w->playability, "OK");
        dl_core_resume(w->core, ID_A);
        run(w, 600000);
        CHECK(item(w, ID_A).state == DL_DONE, "downloadable after all");
        world_free(w);
    }
}

static void test_backoff_and_cap(void) {
    World *w = world_new();
    core_new(w);
    w->fetch_all_fail = 1;
    enq(w, ID_A, "A");
    static const int expect[] = { 1, 2, 4, 8, 8, 8, 8 };
    uint64_t t0 = 0;
    for (int i = 0; i < 7; i++) {
        CHECK(run_until(w, ID_A, DL_WAITING, 120000), "failure %d", i + 1);
        if (i == 0) t0 = w->now;
        DlItem it = item(w, ID_A);
        CHECK(it.retry_in_s == expect[i], "failure %d: retry in %d s, want %d", i + 1,
              it.retry_in_s, expect[i]);
        CHECK(run_until_not(w, ID_A, DL_WAITING, 120000), "attempt %d", i + 2);
    }
    CHECK(run_until(w, ID_A, DL_FAILED, 120000), "gives up");
    DlItem it = item(w, ID_A);
    CHECK(it.error == DL_ERR_NETWORK, "network (%d)", it.error);
    CHECK(w->nreq == DL_NET_FAIL_LIMIT, "eight attempts (%d)", w->nreq);
    uint64_t spent = w->now - t0;
    CHECK(spent >= 39000 && spent < 39500, "1+2+4+8+8+8+8 s of waiting (%llu ms)",
          (unsigned long long)spent);
    w->fetch_all_fail = 0;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "a retry works");
    world_free(w);

    /* Progress between failures starts the count again. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    for (int i = 0; i < 12; i++) {
        w->drop_after = 100000;
        w->piece = 7001;
        run_until(w, ID_A, DL_WAITING, 120000);
        DlItem p = item(w, ID_A);
        if (p.state != DL_WAITING) break;
        CHECK(p.retry_in_s == 1, "always the first step again (%d)", p.retry_in_s);
        run_until_not(w, ID_A, DL_WAITING, 120000);
    }
    CHECK(item(w, ID_A).state != DL_FAILED, "twelve cuts with progress did not fail it");
    w->drop_after = -1;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* YouTube refusing the resolve: patient, then DL_ERR_URL. */
    w = world_new();
    core_new(w);
    w->resolve_result = DL_NET_FAILED;
    enq(w, ID_A, "A");
    run(w, 600000);
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_URL, "url (%d/%d)", it.state, it.error);
    CHECK(w->resolve_calls == DL_RESOLVE_FAIL_LIMIT, "resolves: %d", w->resolve_calls);
    world_free(w);
}

static void test_offline(void) {
    World *w = world_new();
    core_new(w);
    w->connected = 0;
    enq(w, ID_A, "A");
    run(w, 5000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "waits for an event (%d, %d)",
          it.state, it.retry_in_s);
    CHECK(w->nreq == 0 && w->resolve_calls == 0, "no attempt while offline");
    for (int i = 0; i < 360; i++) { pump(w); w->now += 100000; } /* ten hours */
    it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "still waiting, never failed");
    w->connected = 1;
    w->gen++;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done once the network is back");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* Offline in the middle of a chunk. */
    w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_offline;
    enq(w, ID_A, "A");
    run(w, 5000);
    it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "offline wait (%d, %d)",
          it.state, it.retry_in_s);
    int64_t got = it.bytes_done;
    CHECK(got >= 1000000 && file_of(w, ID_A, "part")->size == got, "bytes kept");
    int reqs = w->nreq;
    for (int i = 0; i < 100; i++) { pump(w); w->now += 60000; }
    CHECK(w->nreq == reqs && item(w, ID_A).state == DL_WAITING, "still waiting");
    w->connected = 1;
    w->gen++;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(w->req_first[reqs] == got, "continued at the exact byte");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* The Wi-Fi came back while a request was in flight: not a failure, and
     * no waiting. */
    w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_network_change;
    enq(w, ID_A, "A");
    CHECK(run_until(w, ID_A, DL_DOWNLOADING, 1000), "downloading");
    for (int i = 0; i < 400 && w->hook; i++) dl_core_step(w->core);
    dl_core_step(w->core);
    it = item(w, ID_A);
    CHECK(it.state == DL_DOWNLOADING, "retries at once, no wait (%d)", it.state);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* A wake while waiting out a backoff ends the backoff. */
    w = world_new();
    core_new(w);
    w->fetch_all_fail = 1;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_WAITING, 60000);
    run_until_not(w, ID_A, DL_WAITING, 60000);
    run_until(w, ID_A, DL_WAITING, 60000); /* second failure: 2 s */
    CHECK(item(w, ID_A).retry_in_s == 2, "2 s backoff");
    w->fetch_all_fail = 0;
    w->gen++;
    uint64_t t = w->now;
    dl_core_step(w->core);
    CHECK(item(w, ID_A).state != DL_WAITING && w->now == t, "retried at once");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    world_free(w);

    /* dl_resume on a waiting item: try now. */
    w = world_new();
    core_new(w);
    w->drop_after = 1234567;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_WAITING, 60000);
    dl_core_resume(w->core, ID_A);
    dl_core_step(w->core);
    CHECK(item(w, ID_A).state != DL_WAITING, "immediate attempt");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    world_free(w);
}

static void test_playback_hold(void) {
    World *w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_hold; /* an online video starts to play */
    enq(w, ID_A, "A");
    run(w, 5000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "held (%d, %d)", it.state,
          it.retry_in_s);
    int64_t got = it.bytes_done;
    CHECK(file_of(w, ID_A, "part")->size == got, "bytes kept");
    int reqs = w->nreq;
    dl_core_playback_begin(w->core); /* nesting: two playbacks now */
    for (int i = 0; i < 50; i++) { pump(w); w->now += 10000; }
    CHECK(w->nreq == reqs, "no traffic while playing");
    dl_core_playback_end(w->core);
    for (int i = 0; i < 50; i++) { pump(w); w->now += 10000; }
    CHECK(w->nreq == reqs && item(w, ID_A).state == DL_WAITING, "one playback still on");
    dl_core_playback_end(w->core);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "continues by itself afterwards");
    CHECK(w->req_first[reqs] == got, "at the exact byte");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* A queued item shows the hold too, and does not start. */
    w = world_new();
    core_new(w);
    dl_core_playback_begin(w->core);
    enq(w, ID_A, "A");
    run(w, 5000);
    it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "waiting for playback");
    CHECK(w->nreq == 0 && w->resolve_calls == 0, "no traffic");
    dl_core_playback_end(w->core);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done afterwards");
    dl_core_playback_end(w->core); /* unbalanced end: harmless */
    world_free(w);
}

static void test_queue_order(void) {
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    enq(w, ID_C, "C");
    dl_core_pause(w->core, ID_B);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "A done");
    CHECK(item(w, ID_C).state == DL_DONE, "C started by itself when the slot freed");
    CHECK(item(w, ID_B).state == DL_PAUSED, "B never starts by itself");
    for (int i = 0; i < w->nreq; i++)
        CHECK(strcmp(w->req_id[i], ID_B) != 0, "no request for B");
    CHECK(strcmp(w->req_id[0], ID_A) == 0 && strcmp(w->req_id[w->nreq - 1], ID_C) == 0,
          "A before C");
    dl_core_resume(w->core, ID_B);
    run(w, 600000);
    CHECK(item(w, ID_B).state == DL_DONE, "B after resume");
    CHECK(dl_core_count(w->core) == 3, "three items");
    DlItem all[8];
    CHECK(dl_core_snapshot(w->core, all, 8) == 3, "snapshot");
    CHECK(strcmp(all[0].video_id, ID_A) == 0 && strcmp(all[1].video_id, ID_B) == 0 &&
              strcmp(all[2].video_id, ID_C) == 0, "oldest first");
    CHECK(dl_core_snapshot(w->core, all, 2) == 2, "snapshot respects max");
    world_free(w);

    /* FIFO among the queued, and a resumed item goes to the back. */
    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    enq(w, ID_C, "C");
    dl_core_pause_all(w->core);
    for (int i = 0; i < 20; i++) { pump(w); w->now += 1000; }
    CHECK(w->nreq == 0 && active(w) == 0, "everything paused");
    dl_core_resume(w->core, ID_C);
    dl_core_resume(w->core, ID_A);
    dl_core_resume(w->core, ID_B);
    run(w, 600000);
    int first_of[3] = { -1, -1, -1 };
    for (int i = 0; i < w->nreq; i++) {
        int k = w->req_id[i][0] - 'a';
        if (k >= 0 && k < 3 && first_of[k] < 0) first_of[k] = i;
    }
    CHECK(first_of[2] < first_of[0] && first_of[0] < first_of[1], "C, A, B in resume order");
    DlItem order[4];
    CHECK(dl_core_snapshot(w->core, order, 4) == 3 && strcmp(order[0].video_id, ID_A) == 0 &&
              strcmp(order[1].video_id, ID_B) == 0 && strcmp(order[2].video_id, ID_C) == 0,
          "the list stays oldest first whatever order the queue runs in");
    world_free(w);

    w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    run_until_bytes(w, ID_A, 1 * MIB);
    dl_core_pause_all(w->core);
    pump(w);
    int64_t a_done = item(w, ID_A).bytes_done;
    CHECK(item(w, ID_A).state == DL_PAUSED && item(w, ID_B).state == DL_PAUSED, "paused");
    int reqs = w->nreq;
    dl_core_resume_all(w->core);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && item(w, ID_B).state == DL_DONE, "both done");
    CHECK(w->req_first[reqs] == a_done && strcmp(w->req_id[reqs], ID_A) == 0,
          "A continues where it stopped");
    world_free(w);
}

static void test_delete(void) {
    World *w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_delete_a;
    yt_video_t v = video(ID_A, "A");
    snprintf(v.thumb_url, sizeof v.thumb_url, "https://i.example/thumb/a.jpg");
    dl_core_enqueue(w->core, &v);
    while (w->hook) dl_core_step(w->core); /* until the hook has run */
    CHECK(dl_core_count(w->core) == 0, "gone from the list at once");
    CHECK(!has_item(w, ID_A), "dl_get agrees");
    pump(w);
    pump(w);
    int left = 0;
    for (int i = 0; i < w->nfiles; i++)
        if (strstr(w->files[i].path, ID_A) != NULL) left++;
    CHECK(left == 0, "all files of the video removed (%d left)", left);
    CHECK(file_of(w, ID_A, "jpg") == NULL && file_of(w, ID_A, "part") == NULL, "jpg, part");
    world_free(w);

    /* Delete, then download the same video again straight away. */
    w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_delete_a;
    enq(w, ID_A, "A");
    while (w->hook) dl_core_step(w->core);
    CHECK(enq(w, ID_A, "A again") == DL_ENQ_OK, "enqueue right after delete");
    run(w, 600000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_DONE && strcmp(it.title, "A again") == 0, "the new entry downloads");
    CHECK(dl_core_count(w->core) == 1, "one entry");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* Delete a finished one, then restart: it stays gone. */
    w = world_new();
    core_new(w);
    yt_video_t v2 = video(ID_A, "A");
    snprintf(v2.thumb_url, sizeof v2.thumb_url, "https://i.example/thumb/a.jpg");
    dl_core_enqueue(w->core, &v2);
    run(w, 600000);
    CHECK(file_of(w, ID_A, "mp4") && file_of(w, ID_A, "jpg"), "have mp4 and jpg");
    dl_core_delete(w->core, ID_A);
    char path[128];
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0, "no path any more");
    run(w, 5000);
    CHECK(file_of(w, ID_A, "mp4") == NULL && file_of(w, ID_A, "jpg") == NULL, "files gone");
    restart_engine(w);
    CHECK(dl_core_count(w->core) == 0, "not in the index either");
    world_free(w);

    /* Delete while waiting, paused, failed and queued. */
    w = world_new();
    core_new(w);
    w->connected = 0;
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    run(w, 1000);
    dl_core_delete(w->core, ID_A); /* the waiting one */
    dl_core_delete(w->core, ID_B); /* the queued one */
    run(w, 1000);
    CHECK(dl_core_count(w->core) == 0, "both removed");
    world_free(w);
}

static void test_duplicates_and_bad_ids(void) {
    World *w = world_new();
    core_new(w);
    CHECK(enq(w, ID_A, "A") == DL_ENQ_OK, "first");
    CHECK(enq(w, ID_A, "A") == DL_ENQ_EXISTS, "queued: exists");
    run(w, 600000);
    CHECK(enq(w, ID_A, "A") == DL_ENQ_EXISTS, "done: exists");
    CHECK(dl_core_count(w->core) == 1, "still one");
    w->fmt_count = 0;
    enq(w, ID_B, "B");
    run(w, 60000);
    CHECK(item(w, ID_B).state == DL_FAILED, "failed");
    w->fmt_count = 1;
    int calls = w->resolve_calls;
    CHECK(enq(w, ID_B, "B") == DL_ENQ_EXISTS, "failed: exists");
    run(w, 60000);
    CHECK(item(w, ID_B).state == DL_FAILED && w->resolve_calls == calls,
          "and left alone for dl_resume");
    world_free(w);

    w = world_new();
    core_new(w);
    static const char *const bad[] = {
        "../x", "..//..//..", "aaaaaaaaaa", "aaaaaaaaaa/",
        "aaaaaaaaa.a", "aaaaa aaaaa", "", "ux0:data/ViTube", "aaaaaaaaaa\n",
        "aaaaaaaaa\xC3\xA9", "a/../../etc", "%2e%2e%2fxxxx",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(enq(w, bad[i], "x") == DL_ENQ_BAD, "'%s' accepted", bad[i]);
    CHECK(dl_core_enqueue(w->core, NULL) == DL_ENQ_BAD, "NULL");
    CHECK(dl_core_count(w->core) == 0, "nothing queued");
    dl_core_pause(w->core, "../x");
    dl_core_resume(w->core, "../x");
    dl_core_delete(w->core, "../x");
    dl_core_report_bad_file(w->core, "../x");
    char path[128];
    CHECK(!dl_core_get(w->core, "../x", NULL), "get");
    CHECK(!dl_core_local_path(w->core, "../x", path, sizeof path), "local path");
    CHECK(!dl_core_thumb_path(w->core, "../x", path, sizeof path), "thumb path");
    run(w, 1000);
    world_free(w);

    /* Before dl_core_load the engine is not running. */
    DlOps ops = { .user = NULL };
    DlCore *c = calloc(1, sizeof *c);
    dl_core_init(c, &ops);
    yt_video_t v = video(ID_A, "A");
    CHECK(dl_core_enqueue(c, &v) == DL_ENQ_BAD, "not running");
    free(c);
}

static void test_index_roundtrip(void) {
    World *w = world_new();
    core_new(w);
    yt_video_t v;
    const char *titles[] = {
        "Plain title",
        "Thai \xE0\xB8\xA0\xE0\xB8\xB2\xE0\xB8\xA9\xE0\xB8\xB2 and emoji \xF0\x9F\x8E\xAC",
        "quote \" and back\\slash and </script>",
        "bad \xFF\xFE bytes \xC3 end",
        "tab\there\nnewline",
    };
    const char *ids[] = { ID_A, ID_B, ID_C, "ddddddddddd", "eeeeeeeeeee" };
    for (int i = 0; i < 5; i++) {
        v = video(ids[i], titles[i]);
        snprintf(v.channel, sizeof v.channel, "Chan %d \xE2\x80\x94", i);
        snprintf(v.duration, sizeof v.duration, "%d:%02d", i, 7);
        snprintf(v.thumb_url, sizeof v.thumb_url, "https://i.example/thumb/%d.jpg?x=1&y=2", i);
        dl_core_enqueue(w->core, &v);
    }
    dl_core_pause(w->core, ID_B);
    dl_core_pause(w->core, ID_C);
    run(w, 1200000);
    DlItem before[8], after[8];
    int nb = dl_core_snapshot(w->core, before, 8);
    CHECK(nb == 5, "five entries");
    CHECK(strcmp(before[3].title, "bad ?? bytes ? end") == 0, "invalid UTF-8 repaired: '%s'",
          before[3].title);
    CHECK(strcmp(before[4].title, "tab here newline") == 0, "controls: '%s'", before[4].title);
    restart_engine(w);
    int na = dl_core_snapshot(w->core, after, 8);
    CHECK(na == nb, "same count");
    for (int i = 0; i < na && i < nb; i++) {
        CHECK(strcmp(before[i].video_id, after[i].video_id) == 0, "id %d", i);
        CHECK(strcmp(before[i].title, after[i].title) == 0, "title %d: '%s' vs '%s'", i,
              before[i].title, after[i].title);
        CHECK(strcmp(before[i].channel, after[i].channel) == 0, "channel %d", i);
        CHECK(strcmp(before[i].duration, after[i].duration) == 0, "duration %d", i);
        CHECK(before[i].state == after[i].state, "state %d: %d vs %d", i, before[i].state,
              after[i].state);
        CHECK(before[i].error == after[i].error, "error %d", i);
        CHECK(before[i].height == after[i].height, "height %d", i);
        CHECK(before[i].bytes_done == after[i].bytes_done, "done %d", i);
        CHECK(before[i].bytes_total == after[i].bytes_total, "total %d", i);
    }
    /* The thumbnail URL made the trip too: nothing is fetched again for the
     * ones that have a jpg, and the paused ones get theirs. */
    int fetched = w->thumb_fetches;
    run(w, 60000);
    CHECK(w->thumb_fetches >= fetched, "no crash on thumbs");
    world_free(w);
}

static const char *const ids8[] = { "aaaaaaaaaaa", "bbbbbbbbbbb", "ccccccccccc",
    "ddddddddddd", "eeeeeeeeeee", "fffffffffff", "ggggggggggg", "hhhhhhhhhhh" };

static void test_crash_recovery(void) {
    World *w = world_new();
    char text[4096];
    snprintf(text, sizeof text,
        "{\"v\":1,\"items\":["
        "{\"id\":\"%s\",\"title\":\"A\",\"state\":\"queued\",\"done\":100,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"B\",\"state\":\"downloading\",\"done\":777,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"C\",\"state\":\"done\",\"done\":%lld,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"D\",\"state\":\"done\",\"done\":%lld,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"E\",\"state\":\"queued\",\"done\":5,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"F\",\"state\":\"paused\",\"done\":0,\"total\":%lld},"
        "{\"id\":\"%s\",\"title\":\"G\",\"state\":\"queued\",\"done\":0,\"total\":1000},"
        "{\"id\":\"%s\",\"title\":\"H\",\"state\":\"done\",\"done\":%lld,\"total\":%lld}"
        "]}",
        ids8[0], (long long)SRC_LEN, ids8[1], (long long)SRC_LEN,
        ids8[2], (long long)SRC_LEN, (long long)SRC_LEN,
        ids8[3], (long long)SRC_LEN, (long long)SRC_LEN,
        ids8[4], (long long)SRC_LEN, ids8[5], (long long)SRC_LEN, ids8[6],
        ids8[7], (long long)SRC_LEN, (long long)SRC_LEN);
    put_text(w, DIR "index.json", text);
    put_part(w, ids8[0], 5000);                        /* A: index says 100 */
    /* B: no part file at all */
    /* C: finished, but the mp4 is gone */
    put_file(w, DIR "ddddddddddd.mp4", w->src, 1234);   /* D: wrong size */
    put_file(w, DIR "eeeeeeeeeee.mp4", w->src, SRC_LEN); /* E: renamed, then crashed */
    put_part(w, ids8[5], 321);                          /* F: paused */
    put_part(w, ids8[6], 5000);                         /* G: larger than total */
    put_file(w, DIR "hhhhhhhhhhh.mp4", w->src, SRC_LEN); /* H: fine */
    core_new(w);

    DlItem it = item(w, ids8[0]);
    CHECK(it.state == DL_QUEUED && it.bytes_done == 5000, "A: the part file is the truth (%lld)",
          (long long)it.bytes_done);
    it = item(w, ids8[1]);
    CHECK(it.state == DL_QUEUED && it.bytes_done == 0, "B: was downloading, queued again");
    CHECK(!has_item(w, ids8[2]), "C: mp4 missing, entry dropped");
    CHECK(!has_item(w, ids8[3]), "D: wrong size, entry dropped");
    it = item(w, ids8[4]);
    CHECK(it.state == DL_DONE && it.bytes_done == SRC_LEN, "E: completed before the crash");
    it = item(w, ids8[5]);
    CHECK(it.state == DL_PAUSED && it.bytes_done == 321, "F: stays paused");
    it = item(w, ids8[6]);
    CHECK(it.state == DL_QUEUED && it.bytes_done == 0, "G: oversized part discarded");
    CHECK(file_of(w, ids8[6], "part") == NULL, "G: and removed");
    it = item(w, ids8[7]);
    CHECK(it.state == DL_DONE, "H: done");
    char path[128];
    CHECK(dl_core_local_path(w->core, ids8[7], path, sizeof path) == 1, "H plays");
    CHECK(dl_core_count(w->core) == 6, "six entries (%d)", dl_core_count(w->core));

    run(w, 1200000);
    CHECK(item(w, ids8[0]).state == DL_DONE && file_equals_src(w, ids8[0], "mp4"),
          "A continues from byte 5000 and is identical");
    CHECK(item(w, ids8[1]).state == DL_DONE, "B downloads");
    CHECK(item(w, ids8[5]).state == DL_PAUSED, "F is still paused");
    int64_t a_first = -1;
    for (int i = 0; i < w->nreq; i++)
        if (strcmp(w->req_id[i], ids8[0]) == 0) { a_first = w->req_first[i]; break; }
    CHECK(a_first == 5000, "first request for A at byte 5000 (%lld)", (long long)a_first);
    restart_engine(w);
    CHECK(dl_core_count(w->core) == 6, "the cleaned list was written back");
    world_free(w);
}

static void test_hostile_index(void) {
    World *w = world_new();
    char big[1200];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    char text[4096];
    snprintf(text, sizeof text,
        "{\"v\":1,\"items\":["
        "{\"id\":\"../../etc/pa\",\"title\":\"x\",\"state\":\"done\",\"done\":1,\"total\":1},"
        "{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaa\",\"title\":\"long\"},"
        "{\"id\":\"short\",\"title\":\"s\"},"
        "{\"id\":\"a/aaaaaaaaaa\",\"title\":\"slash\"},"
        "{\"id\":\"aaaaaaaa.aa\",\"title\":\"dot\"},"
        "{\"id\":\"..\\\\..\\\\xxxxx\",\"title\":\"backslash\"},"
        "{\"id\":12345678901,\"title\":\"num\"},"
        "{\"id\":null},\"string item\",7,[],{},"
        "{\"id\":\"bbbbbbbbbbb\",\"title\":\"good \\\"quoted\\\" \\\\ title\",\"channel\":\"%s\","
        "\"state\":\"paused\",\"done\":-5,\"total\":9223372036854775807,\"height\":\"360\"},"
        "{\"id\":\"bbbbbbbbbbb\",\"title\":\"dup\"},"
        "{\"id\":\"ccccccccccc\",\"state\":\"bogus\",\"error\":\"nonsense\",\"done\":1.5}"
        "]}", big);
    put_text(w, DIR "index.json", text);
    core_new(w);
    CHECK(dl_core_count(w->core) == 2, "only the two sound entries (%d)", dl_core_count(w->core));
    DlItem it = item(w, ID_B);
    CHECK(it.state == DL_PAUSED, "B paused");
    CHECK(strcmp(it.title, "good \"quoted\" \\ title") == 0, "title unescaped: '%s'", it.title);
    CHECK(strlen(it.channel) < sizeof it.channel && strlen(it.channel) > 70,
          "oversized channel clipped to fit (%zu)", strlen(it.channel));
    CHECK(it.bytes_done == 0, "negative size clamped");
    CHECK(it.bytes_total <= ((int64_t)1 << 42) && it.bytes_total > 0, "huge size clamped");
    it = item(w, ID_C);
    CHECK(it.state == DL_QUEUED && it.error == DL_ERR_NONE, "unknown state: queued");
    CHECK(strcmp(it.title, ID_C) == 0, "no title: the id");
    run(w, 1000);
    world_free(w);
}

static void index_text_of_two(World *w, char *out, size_t cap) {
    core_new(w);
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    dl_core_pause_all(w->core);
    dl_core_flush(w->core);
    FFile *f = ffind(w, DIR "index.json");
    snprintf(out, cap, "%s", f ? (char *)f->data : "");
    free(w->core);
    w->core = NULL;
}

static void test_corrupt_index(void) {
    World *src = world_new();
    char good[8192];
    index_text_of_two(src, good, sizeof good);
    CHECK(strlen(good) > 100, "have a good index text");
    world_free(src);

    static const char *const junk[] = {
        "not json at all", "[]", "\"x\"", "null", "42", "{\"items\":42}",
        "{\"items\":[1,2,3]}", "\xFF\xFE\xFD",
    };
    for (size_t i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        World *w = world_new();
        put_text(w, DIR "index.json", junk[i]);
        core_new(w);
        CHECK(dl_core_count(w->core) == 0, "junk %zu: empty list, no crash", i);
        CHECK(enq(w, ID_A, "A") == DL_ENQ_OK, "and usable afterwards");
        world_free(w);
    }

    World *w = world_new();
    char cut[8192];
    snprintf(cut, sizeof cut, "%.*s", (int)(strlen(good) / 2), good);
    put_text(w, DIR "index.json", cut);
    core_new(w);
    CHECK(dl_core_count(w->core) == 0, "truncated file: empty");
    world_free(w);

    w = world_new();
    put_text(w, DIR "index.json", cut);
    put_text(w, DIR "index.json.bak", good);
    core_new(w);
    CHECK(dl_core_count(w->core) == 2, "corrupt main, good .bak: restored");
    run(w, 1000);
    FFile *f = ffind(w, DIR "index.json");
    CHECK(f && strstr((char *)f->data, ID_A) != NULL, "and the main file rewritten");
    world_free(w);

    w = world_new();
    put_text(w, DIR "index.json.tmp", good);
    core_new(w);
    CHECK(dl_core_count(w->core) == 2, "no main file, complete .tmp: used");
    world_free(w);

    w = world_new();
    put_text(w, DIR "index.json", cut);
    put_text(w, DIR "index.json.tmp", cut);
    put_text(w, DIR "index.json.bak", good);
    core_new(w);
    CHECK(dl_core_count(w->core) == 2, "main and .tmp both cut, .bak good");
    world_free(w);

    /* Absurdly deep nesting, and an oversized file. */
    w = world_new();
    {
        size_t depth = 200000;
        char *deep = malloc(depth + 2);
        memset(deep, '[', depth);
        deep[depth] = '\0';
        put_text(w, DIR "index.json", deep);
        free(deep);
        core_new(w);
        CHECK(dl_core_count(w->core) == 0, "deep nesting: empty, no crash");
    }
    world_free(w);
    w = world_new();
    {
        size_t n = DL_INDEX_MAX_BYTES + 100;
        char *huge = malloc(n + strlen(good) + 2);
        memset(huge, ' ', n);
        strcpy(huge + n, good);
        put_text(w, DIR "index.json", huge);
        free(huge);
        core_new(w);
        CHECK(dl_core_count(w->core) == 0, "oversized file is not read");
    }
    world_free(w);
}

static void test_atomic_index(void) {
    for (int cut = 0; cut <= 8; cut++) {
        World *w = world_new();
        core_new(w);
        enq(w, ID_A, "A");
        enq(w, ID_B, "B");
        dl_core_pause_all(w->core);
        dl_core_flush(w->core);
        enq(w, ID_C, "C");
        dl_core_pause(w->core, ID_C);
        w->crash_after = w->mutations + cut; /* the power goes mid-write */
        dl_core_flush(w->core);
        w->crash_after = -1;
        free(w->core);
        core_new(w);
        int n = dl_core_count(w->core);
        CHECK(n == 2 || n == 3, "cut after %d fs ops: %d entries survive", cut, n);
        world_free(w);
    }

    /* A rename that fails: the old file is put back, and it is tried again. */
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    dl_core_pause_all(w->core);
    dl_core_flush(w->core);
    enq(w, ID_B, "B");
    dl_core_pause(w->core, ID_B);
    w->fail_rename_to_json = 1;
    dl_core_flush(w->core);
    FFile *f = ffind(w, DIR "index.json");
    CHECK(f && strstr((char *)f->data, ID_A) != NULL, "old index restored");
    w->now += 6000;
    pump(w);
    pump(w);
    f = ffind(w, DIR "index.json");
    CHECK(f && strstr((char *)f->data, ID_B) != NULL, "retried a few seconds later");
    CHECK(ffind(w, DIR "index.json.bak") != NULL, "keeps the previous version as .bak");
    world_free(w);
}

static void test_thumbnails(void) {
    char path[128];
    yt_video_t v = video(ID_A, "A");
    snprintf(v.thumb_url, sizeof v.thumb_url, "https://i.ytimg.com/vi/aaaaaaaaaaa/hqdefault.jpg?sqp=x");

    /* A failing thumbnail never fails the download, and is not hammered. */
    World *w = world_new();
    core_new(w);
    w->thumb_status = 404;
    dl_core_enqueue(w->core, &v);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "download unaffected");
    CHECK(file_of(w, ID_A, "jpg") == NULL, "no thumbnail");
    for (int i = 0; i < 100; i++) { pump(w); w->now += 3000; }
    CHECK(w->thumb_fetches == DL_THUMB_TRIES, "tried %d times in all", w->thumb_fetches);
    CHECK(dl_core_thumb_path(w->core, ID_A, path, sizeof path) == 0, "thumb path: none");
    world_free(w);

    /* Each way a body can be wrong is refused, and leaves nothing behind. */
    for (int variant = 0; variant < 6; variant++) {
        w = world_new();
        core_new(w);
        const char *what = "";
        switch (variant) {
        case 0: w->thumb[0] = 0x00; what = "not a JPEG"; break;
        case 1: w->thumb[w->thumb_len - 1] = 0x00; what = "no end marker"; break;
        case 2: w->thumb[28] = 0xFF; w->thumb[29] = 0xD9; w->thumb_cut = 30;
                what = "complete-looking JPEG shorter than Content-Length"; break;
        case 3: w->thumb_cut = 30; w->hide_thumb_clen = 1; what = "cut, no length"; break;
        case 4: set_thumb(w, DL_THUMB_MAX_BYTES + 1); what = "over 256 KiB, advertised"; break;
        case 5: set_thumb(w, DL_THUMB_MAX_BYTES + 1); w->hide_thumb_clen = 1;
                what = "over 256 KiB, not advertised"; break;
        }
        dl_core_enqueue(w->core, &v);
        run(w, 600000);
        CHECK(item(w, ID_A).state == DL_DONE, "%s: the download is unaffected", what);
        CHECK(file_of(w, ID_A, "jpg") == NULL, "%s: nothing saved", what);
        CHECK(file_of(w, ID_A, "jpg.tmp") == NULL, "%s: no tmp left", what);
        CHECK(dl_core_thumb_path(w->core, ID_A, path, sizeof path) == 0, "%s: no path", what);
        world_free(w);
    }

    /* A complete JPEG of exactly 256 KiB is fine (and written in pieces). */
    w = world_new();
    core_new(w);
    set_thumb(w, DL_THUMB_MAX_BYTES);
    dl_core_enqueue(w->core, &v);
    run(w, 600000);
    FFile *jf = file_of(w, ID_A, "jpg");
    CHECK(jf && jf->size == DL_THUMB_MAX_BYTES && memcmp(jf->data, w->thumb, DL_THUMB_MAX_BYTES) == 0,
          "256 KiB thumbnail saved whole");
    CHECK(strstr(w->thumb_req, "mqdefault.jpg") && !strstr(w->thumb_req, "hqdefault"),
          "never the list's URL");
    world_free(w);

    /* A transport failure gives the attempt back: a minute of failures does
     * not use up the three tries. */
    w = world_new();
    core_new(w);
    w->thumb_fail_transport = 1;
    dl_core_enqueue(w->core, &v);
    dl_core_pause(w->core, ID_A);
    for (int i = 0; i < 60; i++) { pump(w); w->now += 2000; }
    CHECK(w->thumb_fetches > DL_THUMB_TRIES, "keeps trying, throttled (%d)", w->thumb_fetches);
    CHECK(w->thumb_fetches < 8, "backs off, not a request every few seconds (%d)",
          w->thumb_fetches);
    w->thumb_fail_transport = 0;
    for (int i = 0; i < 60; i++) { pump(w); w->now += 2000; }
    CHECK(file_of(w, ID_A, "jpg") != NULL, "a paused item gets its thumbnail too");
    world_free(w);

    /* Once saved, never fetched again, also across a restart; every item
     * gets one, paused or not. */
    w = world_new();
    core_new(w);
    dl_core_enqueue(w->core, &v);
    yt_video_t v2 = video(ID_B, "B");
    dl_core_enqueue(w->core, &v2);
    dl_core_pause(w->core, ID_B);
    run(w, 600000);
    CHECK(file_of(w, ID_A, "jpg") && file_of(w, ID_B, "jpg"), "both have a thumbnail");
    CHECK(w->thumb_fetches == 2, "two fetches (%d)", w->thumb_fetches);
    restart_engine(w);
    run(w, 60000);
    CHECK(w->thumb_fetches == 2, "none after a restart (%d)", w->thumb_fetches);
    CHECK(dl_core_thumb_path(w->core, ID_B, path, sizeof path) == 1, "thumb path");
    CHECK(strcmp(path, DIR ID_B ".jpg") == 0, "thumb path value");
    world_free(w);

    /* No thumbnail traffic while an online video plays. */
    w = world_new();
    core_new(w);
    dl_core_playback_begin(w->core);
    dl_core_enqueue(w->core, &v);
    run(w, 5000);
    CHECK(w->thumb_fetches == 0, "held");
    dl_core_playback_end(w->core);
    run(w, 600000);
    CHECK(w->thumb_fetches == 1, "then fetched");
    world_free(w);
}

static unsigned g_rev_last;
static int g_rev_changes;
static int64_t g_speed_sample;
static int g_speed_sampled;

static void sample_piece(World *w) {
    unsigned r = dl_core_revision(w->core);
    if (r != g_rev_last) { g_rev_changes++; g_rev_last = r; }
    DlItem it = item(w, ID_A);
    if (!g_speed_sampled && it.bytes_done > 6 * MIB) {
        g_speed_sample = it.bytes_per_sec;
        g_speed_sampled = 1;
    }
}

static void test_speed_and_revision(void) {
    World *w = world_new();
    core_new(w);
    w->rate = 1000000;
    w->on_piece = sample_piece;
    g_rev_last = dl_core_revision(w->core);
    g_rev_changes = 0;
    g_speed_sampled = 0;
    enq(w, ID_A, "A");
    uint64_t t0 = w->now;
    run(w, 600000);
    uint64_t secs = (w->now - t0) / 1000 + 1;
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    CHECK(g_speed_sample > 800000 && g_speed_sample < 1200000,
          "smoothed speed %lld B/s for 1,000,000", (long long)g_speed_sample);
    CHECK(g_rev_changes <= (int)(secs * 4 + 15), "%d revision bumps in %llu s",
          g_rev_changes, (unsigned long long)secs);
    CHECK(g_rev_changes >= 15, "but it does move (%d)", g_rev_changes);
    world_free(w);
}

static void test_report_bad_file_and_local_path(void) {
    World *w = world_new();
    core_new(w);
    download_all(w, ID_A);
    char path[128];
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 1, "playable");
    CHECK(dl_core_local_path(w->core, ID_A, path, 10) == 0, "path too small for the buffer");
    dl_core_report_bad_file(w->core, ID_A);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_VERIFY, "failed: verify");
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0, "no longer offered");
    int reqs = w->nreq;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "downloaded again");
    CHECK(w->req_first[reqs] == 0, "from byte 0");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* The file changed behind the app's back. */
    w = world_new();
    core_new(w);
    download_all(w, ID_A);
    FFile *f = file_of(w, ID_A, "mp4");
    f->size -= 100;
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0, "size differs");
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.bytes_done == 0, "entry no longer claims it");
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && file_equals_src(w, ID_A, "mp4"), "fetched again");
    world_free(w);

    /* The player's report outlives a restart. The unplayable .mp4 stays on
     * the card until the retry, and its size still matches the entry, which
     * is exactly what makes a crashed-after-rename entry count as finished:
     * the reconcile at start-up must not mistake the one for the other, or
     * the broken file is offered as a finished download after every
     * restart. */
    w = world_new();
    core_new(w);
    download_all(w, ID_A);
    dl_core_report_bad_file(w->core, ID_A);
    restart_engine(w);
    it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_VERIFY,
          "still failed after a restart (%d, %d)", it.state, it.error);
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0,
          "not offered for playback again");
    CHECK(file_of(w, ID_A, "mp4") != NULL, "the file itself is left alone");
    reqs = w->nreq;
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "retry downloads it");
    CHECK(w->nreq > reqs && w->req_first[reqs] == 0, "from byte 0, not from the old file");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);

    /* A queued entry whose .part is gone and whose .mp4 has the wrong size
     * is not a finished download either. */
    w = world_new();
    char text[512];
    snprintf(text, sizeof text,
        "{\"v\":1,\"items\":[{\"id\":\"%s\",\"title\":\"A\",\"state\":\"queued\","
        "\"done\":0,\"total\":%lld}]}", ID_A, (long long)SRC_LEN);
    put_text(w, DIR "index.json", text);
    put_file(w, DIR ID_A ".mp4", w->src, 1234);
    core_new(w);
    it = item(w, ID_A);
    CHECK(it.state == DL_QUEUED && it.bytes_done == 0, "wrong-size .mp4 is not 'done' (%d)",
          it.state);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && file_equals_src(w, ID_A, "mp4"),
          "downloaded in full");
    world_free(w);
}

static void test_shutdown_and_restart(void) {
    World *w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_stop;
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    dl_core_pause(w->core, ID_B);
    while (w->hook) dl_core_step(w->core);
    pump(w);
    int64_t got = file_of(w, ID_A, "part")->size;
    CHECK(got >= 1000000, "bytes of the interrupted chunk were written");
    restart_engine(w);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_QUEUED && it.bytes_done == got, "queued again at the exact byte");
    CHECK(item(w, ID_B).state == DL_PAUSED, "pause survives");
    int reqs = w->nreq;
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "continues");
    CHECK(w->req_first[reqs] == got, "first request after the restart at the exact byte");
    CHECK(item(w, ID_B).state == DL_PAUSED, "B still paused");
    CHECK(file_equals_src(w, ID_A, "mp4"), "identical");
    world_free(w);
}

static void test_storage(void) {
    World *w = world_new();
    core_new(w);
    DlStorage st;
    CHECK(dl_core_storage(w->core, &st) == 0, "ok");
    CHECK(st.total_bytes == 4096 * MIB && st.free_bytes == 4096 * MIB, "numbers");
    CHECK(!st.low && st.downloads_bytes == 0, "plenty");
    w->extra_used = 1000 * MIB;
    dl_core_storage(w->core, &st);
    CHECK(st.free_bytes == 4096 * MIB, "a frame loop's repeated calls are answered from memory");
    w->now += 1500;
    dl_core_storage(w->core, &st);
    CHECK(st.free_bytes == 3096 * MIB, "then fresh");
    download_all(w, ID_A);
    w->now += 1500;
    dl_core_storage(w->core, &st);
    CHECK(st.downloads_bytes == SRC_LEN, "downloads occupy the file size (%lld)",
          (long long)st.downloads_bytes);
    w->extra_used = w->card_total - 50 * MIB;
    w->now += 1500;
    dl_core_storage(w->core, &st);
    CHECK(st.low, "low");
    world_free(w);
}

/* --- behaviours the Downloads screen and the player rely on ---------------- */

static unsigned g_pr_rev;
static uint64_t g_pr_time;
static int64_t g_pr_bytes;
static int g_pr_bad;

/* Called after every piece of a response: while bytes keep arriving, the
 * revision must keep moving. */
static void progress_watch(World *w) {
    unsigned r = dl_core_revision(w->core);
    int64_t b = item(w, ID_A).bytes_done;
    if (r != g_pr_rev) {
        g_pr_rev = r;
        g_pr_time = w->now;
        g_pr_bytes = b;
    } else if (b > g_pr_bytes && w->now - g_pr_time > 450) {
        g_pr_bad++;
    }
}

static void test_revision_and_commands(void) {
    World *w = world_new();
    core_new(w);
    unsigned r = dl_core_revision(w->core);
    long ops = w->fs_ops + w->net_ops;
    CHECK(enq(w, ID_A, "A") == DL_ENQ_OK, "enqueue");
    CHECK(dl_core_revision(w->core) != r, "enqueue bumps the revision");
    CHECK(dl_core_count(w->core) == 1 && item(w, ID_A).state == DL_QUEUED,
          "and is visible at once, before the worker did anything");
    CHECK(w->fs_ops + w->net_ops - ops == 1,
          "enqueue costs one free-space query and nothing else (%ld)",
          w->fs_ops + w->net_ops - ops);

    ops = w->fs_ops + w->net_ops;
    r = dl_core_revision(w->core);
    dl_core_pause(w->core, ID_A);
    CHECK(dl_core_revision(w->core) != r && item(w, ID_A).state == DL_PAUSED, "pause");
    r = dl_core_revision(w->core);
    dl_core_resume(w->core, ID_A);
    CHECK(dl_core_revision(w->core) != r && item(w, ID_A).state == DL_QUEUED, "resume");
    r = dl_core_revision(w->core);
    dl_core_pause_all(w->core);
    CHECK(dl_core_revision(w->core) != r && item(w, ID_A).state == DL_PAUSED, "pause_all");
    r = dl_core_revision(w->core);
    dl_core_resume_all(w->core);
    CHECK(dl_core_revision(w->core) != r && item(w, ID_A).state == DL_QUEUED, "resume_all");
    DlItem tmp[4];
    dl_core_snapshot(w->core, tmp, 4);
    dl_core_get(w->core, ID_A, tmp);
    dl_core_count(w->core);
    dl_core_active_count(w->core);
    r = dl_core_revision(w->core);
    dl_core_delete(w->core, ID_A);
    CHECK(dl_core_revision(w->core) != r, "delete bumps the revision");
    CHECK(dl_core_count(w->core) == 0 && !has_item(w, ID_A), "and the row is gone at once");
    CHECK(w->fs_ops + w->net_ops - ops == 0,
          "commands and queries touch neither card nor network (%ld)",
          w->fs_ops + w->net_ops - ops);
    world_free(w);

    /* Every state change, and the progress in between, shows in the revision. */
    w = world_new();
    core_new(w);
    w->on_piece = progress_watch;
    g_pr_bad = 0;
    enq(w, ID_A, "A");
    g_pr_rev = dl_core_revision(w->core);
    g_pr_time = w->now;
    g_pr_bytes = 0;
    DlState seen = DL_QUEUED;
    int changes = 0;
    for (int i = 0; i < 100000; i++) {
        unsigned before = dl_core_revision(w->core);
        int rr = dl_core_step(w->core);
        if (rr > 0) w->now += (uint64_t)rr;
        DlItem it = item(w, ID_A);
        if (it.state != seen) {
            changes++;
            CHECK(dl_core_revision(w->core) != before, "state %d -> %d without a bump", seen,
                  it.state);
            seen = it.state;
        }
        if (it.state == DL_DONE) break;
    }
    CHECK(seen == DL_DONE && changes >= 3, "went through its states (%d changes)", changes);
    CHECK(g_pr_bad == 0, "progress was invisible for more than 450 ms %d times", g_pr_bad);
    world_free(w);

    /* A retry countdown shows in the revision as it ticks. */
    w = world_new();
    core_new(w);
    w->fetch_all_fail = 1;
    enq(w, ID_A, "A");
    for (int i = 0; i < 4; i++) {
        run_until(w, ID_A, DL_WAITING, 120000);
        if (i < 3) run_until_not(w, ID_A, DL_WAITING, 120000);
    }
    CHECK(item(w, ID_A).retry_in_s == 8, "8 s countdown");
    for (int k = 0; k < 5; k++) {
        int before_s = item(w, ID_A).retry_in_s;
        unsigned before = dl_core_revision(w->core);
        w->now += 1000;
        pump(w);
        int after_s = item(w, ID_A).retry_in_s;
        if (after_s != before_s && item(w, ID_A).state == DL_WAITING)
            CHECK(dl_core_revision(w->core) != before, "countdown %d -> %d without a bump",
                  before_s, after_s);
    }
    world_free(w);
}

static void test_storage_is_cheap(void) {
    World *w = world_new();
    core_new(w);
    DlStorage st;
    long o = w->fs_ops;
    dl_core_storage(w->core, &st);
    CHECK(w->fs_ops - o <= 1, "one query (%ld)", w->fs_ops - o);
    o = w->fs_ops;
    for (int i = 0; i < 100; i++) dl_core_storage(w->core, &st);
    CHECK(w->fs_ops - o == 0, "a hundred calls in the same instant cost nothing");
    for (int i = 0; i < 20; i++) {
        w->now += 2000; /* the screen's own 2 s refresh */
        o = w->fs_ops;
        long n = w->net_ops;
        dl_core_storage(w->core, &st);
        CHECK(w->fs_ops - o <= 1 && w->net_ops == n, "at most one syscall per call");
    }
    /* The answer follows a delete at once: the screen asks right after it. */
    enq(w, ID_A, "A");
    run(w, 600000);
    dl_core_storage(w->core, &st);
    int64_t used_free = st.free_bytes;
    dl_core_delete(w->core, ID_A);
    run(w, 5000);
    dl_core_storage(w->core, &st);
    CHECK(st.free_bytes > used_free + SRC_LEN / 2,
          "free space grew after the delete without waiting for the cache (%lld -> %lld)",
          (long long)used_free, (long long)st.free_bytes);
    world_free(w);
}

static int files_of(World *w, const char *id) {
    int n = 0;
    for (int i = 0; i < w->nfiles; i++)
        if (strstr(w->files[i].path, id) != NULL) n++;
    return n;
}

static void expect_gone(World *w, const char *id, const char *what) {
    run(w, 5000);
    pump(w);
    pump(w);
    CHECK(!has_item(w, id), "%s: still listed", what);
    CHECK(files_of(w, id) == 0, "%s: %d files left behind", what, files_of(w, id));
    restart_engine(w);
    CHECK(dl_core_count(w->core) == 0, "%s: still in the index", what);
}

static void test_delete_every_state(void) {
    /* queued */
    World *w = world_new();
    core_new(w);
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    dl_core_delete(w->core, ID_B);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "the other one is unaffected");
    dl_core_delete(w->core, ID_A);
    expect_gone(w, ID_B, "queued");
    expect_gone(w, ID_A, "done");
    world_free(w);

    /* resolving */
    w = world_new();
    core_new(w);
    w->resolve_hook = hook_delete_a;
    enq(w, ID_A, "A");
    expect_gone(w, ID_A, "resolving");
    CHECK(w->nreq == 0, "nothing fetched for it");
    world_free(w);

    /* downloading is test_delete; paused */
    w = world_new();
    core_new(w);
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_pause_a;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_PAUSED, 60000);
    pump(w);
    CHECK(file_of(w, ID_A, "part") != NULL, "have a part file");
    dl_core_delete(w->core, ID_A);
    expect_gone(w, ID_A, "paused");
    world_free(w);

    /* waiting */
    w = world_new();
    core_new(w);
    w->drop_after = 1234567;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_WAITING, 60000);
    dl_core_delete(w->core, ID_A);
    expect_gone(w, ID_A, "waiting");
    world_free(w);

    /* failed, with its part file */
    w = world_new();
    core_new(w);
    w->write_budget = 5000000;
    enq(w, ID_A, "A");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_FAILED && file_of(w, ID_A, "part"), "failed with a part");
    dl_core_delete(w->core, ID_A);
    expect_gone(w, ID_A, "failed");
    world_free(w);

    /* verifying: the file is renamed while the delete is pending */
    w = world_new();
    core_new(w);
    w->open_read_hook = hook_delete_a;
    enq(w, ID_A, "A");
    expect_gone(w, ID_A, "verifying");
    world_free(w);

    /* delete, and enqueue the same video again, with files already there */
    w = world_new();
    core_new(w);
    download_all(w, ID_A);
    dl_core_delete(w->core, ID_A);
    CHECK(enq(w, ID_A, "again") == DL_ENQ_OK, "enqueue straight after");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && file_equals_src(w, ID_A, "mp4"), "fresh copy");
    world_free(w);
}

static void hook_pause_all(World *w) { dl_core_pause_all(w->core); }

static void test_pause_all_and_try_now(void) {
    /* pause_all: queued, downloading; resume_all: paused only */
    World *w = world_new();
    core_new(w);
    snprintf(w->live_id, sizeof w->live_id, "ddddddddddd");
    enq(w, "eeeeeeeeeee", "E");
    run(w, 600000);
    enq(w, "ddddddddddd", "D");
    run(w, 60000);
    CHECK(item(w, "eeeeeeeeeee").state == DL_DONE, "E done");
    CHECK(item(w, "ddddddddddd").state == DL_FAILED, "D failed");
    w->piece = 10007;
    w->hook_at = 1000000;
    w->hook = hook_pause_all;
    enq(w, ID_A, "A");
    enq(w, ID_B, "B");
    enq(w, ID_C, "C");
    run(w, 60000);
    CHECK(item(w, ID_A).state == DL_PAUSED && item(w, ID_B).state == DL_PAUSED &&
              item(w, ID_C).state == DL_PAUSED, "downloading and queued ones paused");
    CHECK(item(w, "ddddddddddd").state == DL_FAILED && item(w, "eeeeeeeeeee").state == DL_DONE,
          "failed and done ones left alone");
    int reqs = w->nreq;
    for (int i = 0; i < 20; i++) { pump(w); w->now += 5000; }
    CHECK(w->nreq == reqs, "nothing runs");
    dl_core_resume_all(w->core);
    CHECK(item(w, "ddddddddddd").state == DL_FAILED, "resume_all does not retry failures");
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && item(w, ID_B).state == DL_DONE &&
              item(w, ID_C).state == DL_DONE, "paused ones continue");
    CHECK(item(w, "ddddddddddd").state == DL_FAILED, "D still failed");
    w->live_id[0] = '\0';
    dl_core_resume(w->core, "ddddddddddd"); /* resume on FAILED is retry */
    run(w, 600000);
    CHECK(item(w, "ddddddddddd").state == DL_DONE, "retried and downloaded");
    world_free(w);

    /* RESOLVING */
    w = world_new();
    core_new(w);
    w->resolve_hook = hook_pause_all;
    enq(w, ID_A, "A");
    run(w, 60000);
    CHECK(item(w, ID_A).state == DL_PAUSED && w->nreq == 0, "paused while resolving");
    world_free(w);

    /* WAITING */
    w = world_new();
    core_new(w);
    w->drop_after = 1234567;
    enq(w, ID_A, "A");
    run_until(w, ID_A, DL_WAITING, 60000);
    dl_core_pause_all(w->core);
    CHECK(item(w, ID_A).state == DL_PAUSED, "paused while waiting");
    for (int i = 0; i < 20; i++) { pump(w); w->now += 5000; }
    dl_core_resume_all(w->core);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && file_equals_src(w, ID_A, "mp4"), "continued");
    world_free(w);

    /* "Try now" when the connectivity flag is stale: the network works. */
    w = world_new();
    core_new(w);
    w->connected = 0;
    w->flag_stale = 1;
    enq(w, ID_A, "A");
    run(w, 5000);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "flag says offline: waits");
    CHECK(w->resolve_calls == 0, "and made no attempt by itself");
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && file_equals_src(w, ID_A, "mp4"),
          "resume on a waiting item tries at once");
    world_free(w);

    /* ... and when it really is down: one attempt, back to waiting. */
    w = world_new();
    core_new(w);
    w->connected = 0;
    enq(w, ID_A, "A");
    run(w, 5000);
    dl_core_resume(w->core, ID_A);
    run(w, 5000);
    it = item(w, ID_A);
    CHECK(w->resolve_calls == 1, "one attempt (%d)", w->resolve_calls);
    CHECK(it.state == DL_WAITING && it.retry_in_s == -1, "and it waits again");
    world_free(w);

    /* An online playback is not overridden by it. */
    w = world_new();
    core_new(w);
    dl_core_playback_begin(w->core);
    enq(w, ID_A, "A");
    run(w, 5000);
    dl_core_resume(w->core, ID_A);
    run(w, 5000);
    CHECK(w->resolve_calls == 0 && item(w, ID_A).state == DL_WAITING &&
              item(w, ID_A).retry_in_s == -1, "still held");
    dl_core_playback_end(w->core);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE, "done afterwards");
    world_free(w);
}

static void test_done_file_is_untouched(void) {
    World *w = world_new();
    core_new(w);
    download_all(w, ID_A);
    CHECK(item(w, ID_A).state == DL_DONE, "done");
    snprintf(w->watch, sizeof w->watch, "%s", ID_A);
    w->mp4_mutations = 0;
    char path[128];

    /* Everything the UI and the player may do to a finished item except
     * delete it. */
    for (int round = 0; round < 3; round++) {
        CHECK(enq(w, ID_A, "A") == DL_ENQ_EXISTS, "exists");
        dl_core_resume(w->core, ID_A);
        dl_core_pause(w->core, ID_A);
        dl_core_pause_all(w->core);
        dl_core_resume_all(w->core);
        dl_core_playback_begin(w->core);
        dl_core_local_path(w->core, ID_A, path, sizeof path);
        dl_core_thumb_path(w->core, ID_A, path, sizeof path);
        run(w, 5000);
        dl_core_playback_end(w->core);
        w->connected = 0;
        w->gen++;
        run(w, 5000);
        w->connected = 1;
        w->gen++;
        enq(w, ID_B, "B"); /* another download comes and goes */
        run(w, 600000);
        dl_core_delete(w->core, ID_B);
        run(w, 5000);
        restart_engine(w);
        run(w, 5000);
    }
    CHECK(item(w, ID_A).state == DL_DONE, "still done");
    CHECK(w->mp4_mutations == 0, "the finished file was renamed, rewritten or removed (%d)",
          w->mp4_mutations);
    CHECK(file_equals_src(w, ID_A, "mp4"), "and is intact");
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 1, "playable");

    /* A report from the player turns it into a failure at once, and offers
     * no path afterwards; the file itself stays until the user retries. */
    dl_core_report_bad_file(w->core, ID_A);
    DlItem it = item(w, ID_A);
    CHECK(it.state == DL_FAILED && it.error == DL_ERR_VERIFY, "failed: verify");
    CHECK(dl_core_local_path(w->core, ID_A, path, sizeof path) == 0, "no path any more");
    run(w, 5000);
    CHECK(w->mp4_mutations == 0, "nothing deleted behind the player's back");
    dl_core_resume(w->core, ID_A);
    run(w, 600000);
    CHECK(item(w, ID_A).state == DL_DONE && w->mp4_mutations > 0, "fetched again on retry");

    /* delete is the one way it goes */
    w->mp4_mutations = 0;
    dl_core_delete(w->core, ID_A);
    run(w, 5000);
    CHECK(w->mp4_mutations == 1 && file_of(w, ID_A, "mp4") == NULL, "removed by delete");
    world_free(w);
}

int main(void) {
    test_error_keys();
    test_valid_id();
    test_fresh_download();
    test_pause_resume_exact_byte();
    test_drop_mid_chunk();
    test_url_expiry();
    test_url_age();
    test_new_length_restarts();
    test_range_ignored_or_wrong();
    test_disk_full();
    test_free_space_policy();
    test_verify();
    test_verify_in_core();
    test_not_downloadable();
    test_backoff_and_cap();
    test_offline();
    test_playback_hold();
    test_queue_order();
    test_delete();
    test_duplicates_and_bad_ids();
    test_index_roundtrip();
    test_crash_recovery();
    test_hostile_index();
    test_corrupt_index();
    test_atomic_index();
    test_thumbnails();
    test_speed_and_revision();
    test_report_bad_file_and_local_path();
    test_shutdown_and_restart();
    test_storage();
    test_revision_and_commands();
    test_storage_is_cheap();
    test_delete_every_state();
    test_pause_all_and_try_now();
    test_done_file_is_untouched();
    if (failures) {
        fprintf(stderr, "%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("ALL DL CORE TESTS PASSED\n");
    return 0;
}
