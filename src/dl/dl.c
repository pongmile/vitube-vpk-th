/* ViTube — offline downloads: the console bindings of the engine in dl_core.c
 * and the public API of dl.h.
 *
 * Everything that decides anything is in dl_core.c, where tests/test_dl_core.c
 * runs it against fakes. This file only supplies the real ops — sceIo behind
 * the app-wide I/O lock, vita_https for the network, the netwatch thread for
 * "is the Wi-Fi up / did the console wake" — and a worker thread that calls
 * dl_core_step() in a loop.
 *
 * Calling yt_resolve_stream from the worker thread. It is safe, and this is
 * what was checked in innertube.c / http.c / oauth.c / parse.c:
 *   - the request goes through http_post(), which only reads the global
 *     client and builds a private libcurl handle per call (the client is not
 *     persistent); the list loads, the thumbnail worker and the player's
 *     resolve already call it from different threads at the same time;
 *   - the response is parsed into the caller's own yt_stream_t (this file
 *     keeps one in the core) with a private jansson tree;
 *   - the player request is anonymous (with_auth = 0), so the OAuth token
 *     state (g_access_token, g_refresh_failed_until) is never touched;
 *   - what IS shared and unsynchronised is write-only diagnostics:
 *     g_last_error (a fixed buffer written with snprintf — never overruns,
 *     never unterminated, at worst two messages interleave or an error text
 *     is blanked by a success on the other thread) and the visitor id (only
 *     rewritten when YouTube sends a different one; read only by the
 *     VISIONOS client, so at worst one request carries a garbled
 *     X-Goog-Visitor-Id and is answered with LOGIN_REQUIRED, which the HLS
 *     path already retries). Both races exist today between list loads and
 *     the player; the engine adds one rare resolve to them and does not read
 *     yt_last_error() at all. */
#include "dl/dl.h"
#include "dl/dl_core.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita_https.h>

#include "app.h"
#include "log.h"
#include "net/http.h"
#include "net/netwatch.h"
#include "yt/innertube.h"

#define DL_USER_AGENT \
    "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"
/* TLS and curl headroom, like the other network workers. */
#define DL_WORKER_STACK (256 * 1024)
/* Below the main thread (160) and the HLS fetch workers (168): a download
 * must never get in the way of the UI, of audio or of a stream being played. */
#define DL_WORKER_PRIORITY 176
/* SCE_ERROR_ERRNO_ENOSPC: 0x80010000 + errno 28. */
#define DL_SCE_ENOSPC ((int)0x8001001C)

static DlCore *g_core;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker;
static int g_worker_started;
static volatile int g_run;
/* Used by the worker thread only. Not persistent (see hls.c: a reused handle
 * once got googlevideo to answer 404), so every request is a fresh
 * connection — which is why chunks are 4 MiB. */
static VitaHttpsClient *g_client;

/* --- ops: environment ------------------------------------------------------ */

static void ops_lock(void *u) { (void)u; pthread_mutex_lock(&g_mutex); }
static void ops_unlock(void *u) { (void)u; pthread_mutex_unlock(&g_mutex); }

static uint64_t ops_now(void *u) {
    (void)u;
    return (uint64_t)sceKernelGetProcessTimeWide() / 1000u;
}

static int ops_connected(void *u) { (void)u; return vt_netwatch_connected(); }
static unsigned ops_generation(void *u) { (void)u; return vt_netwatch_generation(); }

static void ops_log(void *u, const char *line) {
    (void)u;
    vt_log("%s", line);
}

/* --- ops: files ------------------------------------------------------------ */
/* Every sceIo call is one vt_io_lock()/vt_io_unlock() pair, never held across
 * more than one call group and never across a network wait. Nothing between
 * lock and unlock may log (vt_log takes the same lock). Files are created
 * 0777: a file written 0600 in this directory did not survive a relaunch
 * (see oauth.c). */

static int ops_mkdir(void *u, const char *path) {
    (void)u;
    vt_io_lock();
    sceIoMkdir(path, 0777); /* "already exists" is fine */
    vt_io_unlock();
    return 0;
}

static int ops_open(void *u, const char *path, int mode) {
    (void)u;
    int flags = SCE_O_RDONLY;
    if (mode & DL_FS_WRITE) {
        flags = SCE_O_WRONLY | SCE_O_CREAT;
        if (mode & DL_FS_TRUNC) flags |= SCE_O_TRUNC;
    }
    vt_io_lock();
    SceUID fd = sceIoOpen(path, flags, 0777);
    vt_io_unlock();
    return fd >= 0 ? (int)fd : DL_FS_IO;
}

static int ops_close(void *u, int fd) {
    (void)u;
    vt_io_lock();
    int ret = sceIoClose(fd);
    vt_io_unlock();
    return ret >= 0 ? 0 : DL_FS_IO;
}

static int64_t ops_pread(void *u, int fd, void *buf, size_t len, int64_t off) {
    (void)u;
    if (len > DL_WBUF_BYTES) len = DL_WBUF_BYTES; /* a short count is legal */
    vt_io_lock();
    SceOff pos = sceIoLseek(fd, off, SCE_SEEK_SET);
    int n = pos >= 0 ? sceIoRead(fd, buf, len) : (int)pos;
    vt_io_unlock();
    return n >= 0 ? n : DL_FS_IO;
}

/* The card is out of room, whatever error code the driver chose to say it
 * with: once less than a few MiB is free, a failed write is a full card. */
static int card_nearly_full(void) {
    uint64_t max_size = 0, free_size = 0;
    vt_io_lock();
    int ret = sceAppMgrGetDevInfo("ux0:", &max_size, &free_size);
    vt_io_unlock();
    return ret >= 0 && free_size < (4ULL << 20);
}

static int64_t ops_pwrite(void *u, int fd, const void *buf, size_t len,
                          int64_t off) {
    (void)u;
    if (len > DL_WBUF_BYTES) len = DL_WBUF_BYTES; /* never a long lock hold */
    vt_io_lock();
    SceOff pos = sceIoLseek(fd, off, SCE_SEEK_SET);
    int n = pos >= 0 ? sceIoWrite(fd, buf, len) : (int)pos;
    vt_io_unlock();
    if (n > 0) return n;
    if (n == 0 && len > 0) return DL_FS_NOSPC; /* wrote nothing: card full */
    if (n == DL_SCE_ENOSPC || card_nearly_full()) return DL_FS_NOSPC;
    return DL_FS_IO;
}

static int ops_sync(void *u, int fd) {
    (void)u;
    vt_io_lock();
    int ret = sceIoSyncByFd(fd, 0);
    vt_io_unlock();
    return ret >= 0 ? 0 : DL_FS_IO;
}

static int64_t ops_size(void *u, const char *path) {
    (void)u;
    vt_io_lock();
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size = -1;
    if (fd >= 0) {
        size = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoClose(fd);
    }
    vt_io_unlock();
    return size >= 0 ? (int64_t)size : -1;
}

static int ops_rename(void *u, const char *from, const char *to) {
    (void)u;
    vt_io_lock();
    int ret = sceIoRename(from, to);
    vt_io_unlock();
    return ret >= 0 ? 0 : DL_FS_IO;
}

static int ops_unlink(void *u, const char *path) {
    (void)u;
    vt_io_lock();
    sceIoRemove(path); /* missing is fine */
    vt_io_unlock();
    return 0;
}

static int ops_free(void *u, int64_t *total, int64_t *free_bytes) {
    (void)u;
    uint64_t max_size = 0, free_size = 0;
    vt_io_lock();
    int ret = sceAppMgrGetDevInfo("ux0:", &max_size, &free_size);
    vt_io_unlock();
    if (ret < 0) return -1;
    *total = (int64_t)max_size;
    *free_bytes = (int64_t)free_size;
    return 0;
}

/* --- ops: network ---------------------------------------------------------- */

static int ops_resolve(void *u, const char *video_id, volatile int *cancel,
                       yt_stream_t *out) {
    (void)u;
    unsigned failures = http_transport_failures();
    if (yt_resolve_stream(video_id, cancel, out) == 0) return DL_NET_OK;
    if (cancel && *cancel) return DL_NET_CANCELLED;
    /* The yt layer folds "no network" and "YouTube said no" into one -1;
     * the transport-failure counter (shared by every thread, so it errs
     * towards "network") and the Wi-Fi state tell them apart. */
    if (http_transport_failures() != failures || !vt_netwatch_connected())
        return DL_NET_TRANSPORT;
    return DL_NET_FAILED;
}

typedef struct {
    const DlSink *sink;
    volatile int *cancel;
    unsigned generation;  /* of the network when the request began */
    int sink_aborted;
} FetchCtx;

static size_t fetch_write(const void *data, size_t size, void *opaque) {
    FetchCtx *f = opaque;
    if (f->cancel && *f->cancel) return 0;
    if (f->sink->data(f->sink->ctx, data, size) != 0) {
        f->sink_aborted = 1;
        return 0; /* a short write ends the transfer */
    }
    return size;
}

static int fetch_response(void *opaque, long status, int64_t content_length,
                          const char *content_range) {
    FetchCtx *f = opaque;
    if (f->sink->headers(f->sink->ctx, (int)status, content_length,
                         content_range) != 0) {
        f->sink_aborted = 1;
        return 1;
    }
    return 0;
}

/* After a wake from suspend every socket is dead, but a thread blocked in
 * libcurl only finds out when its timeout runs out. Polled with the cancel
 * flag, this ends the request within about a second of the Wi-Fi coming back
 * instead of waiting out the low-speed timeout. */
static int fetch_should_abort(void *opaque) {
    FetchCtx *f = opaque;
    return vt_netwatch_generation() != f->generation;
}

static int ops_fetch(void *u, const char *url, int64_t first, int64_t last,
                     const DlSink *sink, volatile int *cancel) {
    (void)u;
    char range[64];
    const char *headers[2] = { NULL, NULL };
    if (first >= 0) {
        snprintf(range, sizeof range, "Range: bytes=%lld-%lld",
                 (long long)first, (long long)last);
        headers[0] = range;
    }
    FetchCtx ctx = { sink, cancel, vt_netwatch_generation(), 0 };
    VitaHttpsRequest request = {
        .method = NULL, /* GET */
        .url = url,
        .headers = headers,
        .write = fetch_write,
        .write_opaque = &ctx,
        .cancel_flag = cancel,
        .on_response = fetch_response,
        .on_response_opaque = &ctx,
        .should_abort = fetch_should_abort,
        .should_abort_opaque = &ctx,
    };
    VitaHttpsResponse response;
    memset(&response, 0, sizeof response);
    int ret = vita_https_perform(g_client, &request, &response);
    if (ctx.sink_aborted) return DL_NET_ABORTED;
    if (cancel && *cancel) return DL_NET_CANCELLED;
    if (ret < 0) return DL_NET_TRANSPORT;
    return DL_NET_OK;
}

/* --- the worker ------------------------------------------------------------ */

static void *worker_main(void *unused) {
    (void)unused;
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), DL_WORKER_PRIORITY);
    while (g_run) {
        int ms = dl_core_step(g_core);
        /* Polled rather than waited for on a condition variable, like the
         * other workers: a command is picked up within ~100 ms, and nothing
         * here needs a timed wait (not used anywhere else in the app). */
        if (ms > 0) sceKernelDelayThread((uint32_t)ms * 1000u);
    }
    return NULL;
}

int dl_init(void) {
    if (g_core) return 0;
    VitaHttpsClientConfig config = {
        .user_agent = DL_USER_AGENT,
        .connect_timeout_ms = 12000,
        .request_timeout_ms = 90000, /* one chunk; a slow link keeps its bytes */
        .persistent_connection = 0,
    };
    g_client = vita_https_client_create(&config);
    if (!g_client) {
        vt_log("dl: no http client, downloads unavailable");
        return -1;
    }
    DlCore *core = calloc(1, sizeof *core);
    if (!core) {
        vita_https_client_destroy(g_client);
        g_client = NULL;
        return -1;
    }
    DlOps ops = {
        .user = NULL,
        .lock = ops_lock, .unlock = ops_unlock,
        .now_ms = ops_now, .net_connected = ops_connected,
        .net_generation = ops_generation, .log = ops_log,
        .fs_mkdir = ops_mkdir, .fs_open = ops_open, .fs_close = ops_close,
        .fs_pread = ops_pread, .fs_pwrite = ops_pwrite, .fs_sync = ops_sync,
        .fs_size = ops_size, .fs_rename = ops_rename, .fs_unlink = ops_unlink,
        .fs_free = ops_free,
        .net_resolve = ops_resolve, .net_fetch = ops_fetch,
    };
    ops_mkdir(NULL, VITUBE_DATA_DIR);
    dl_core_init(core, &ops);
    dl_core_load(core);
    g_core = core;
    g_run = 1;

    pthread_attr_t attr;
    int attr_ready = pthread_attr_init(&attr) == 0;
    if (attr_ready) pthread_attr_setstacksize(&attr, DL_WORKER_STACK);
    int rc = pthread_create(&g_worker, attr_ready ? &attr : NULL, worker_main, NULL);
    if (attr_ready) pthread_attr_destroy(&attr);
    if (rc != 0) {
        vt_log("dl: worker failed to start (%d)", rc);
        g_run = 0;
        g_core = NULL;
        free(core);
        vita_https_client_destroy(g_client);
        g_client = NULL;
        return -1;
    }
    g_worker_started = 1;
    vt_log("dl: ready, %d entries", dl_core_count(core));
    return 0;
}

void dl_shutdown(void) {
    DlCore *core = g_core;
    if (!core) return;
    g_run = 0;
    dl_core_stop(core); /* ends the request in flight (cancel flag) */
    if (g_worker_started) pthread_join(g_worker, NULL);
    g_worker_started = 0;
    dl_core_flush(core);
    g_core = NULL;
    free(core);
    if (g_client) vita_https_client_destroy(g_client);
    g_client = NULL;
}

/* --- the API ----------------------------------------------------------------- */

DlEnqueue dl_enqueue(const yt_video_t *video) {
    DlCore *c = g_core;
    return c ? dl_core_enqueue(c, video) : DL_ENQ_BAD;
}

void dl_pause(const char *id) { if (g_core) dl_core_pause(g_core, id); }
void dl_resume(const char *id) { if (g_core) dl_core_resume(g_core, id); }
void dl_pause_all(void) { if (g_core) dl_core_pause_all(g_core); }
void dl_resume_all(void) { if (g_core) dl_core_resume_all(g_core); }
void dl_delete(const char *id) { if (g_core) dl_core_delete(g_core, id); }

int dl_count(void) { return g_core ? dl_core_count(g_core) : 0; }

int dl_snapshot(DlItem *out, int max) {
    return g_core ? dl_core_snapshot(g_core, out, max) : 0;
}

int dl_get(const char *id, DlItem *out) {
    return g_core ? dl_core_get(g_core, id, out) : 0;
}

int dl_active_count(void) { return g_core ? dl_core_active_count(g_core) : 0; }
unsigned dl_revision(void) { return g_core ? dl_core_revision(g_core) : 0; }

int dl_storage(DlStorage *out) {
    if (!out) return -1;
    if (!g_core) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    return dl_core_storage(g_core, out);
}

int dl_local_path(const char *id, char *path, size_t cap) {
    return g_core ? dl_core_local_path(g_core, id, path, cap) : 0;
}

int dl_thumb_path(const char *id, char *path, size_t cap) {
    return g_core ? dl_core_thumb_path(g_core, id, path, cap) : 0;
}

void dl_report_bad_file(const char *id) {
    if (g_core) dl_core_report_bad_file(g_core, id);
}

void dl_playback_begin(void) { if (g_core) dl_core_playback_begin(g_core); }
void dl_playback_end(void) { if (g_core) dl_core_playback_end(g_core); }
