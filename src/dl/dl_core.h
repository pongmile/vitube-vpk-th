/* ViTube — the download engine's logic, with no platform in it.
 *
 * Everything dl.h promises is decided here: the queue, the per-item state
 * machine, resuming, backoff, the free-space policy, the index file, the MP4
 * check. Everything that touches the world — files, sockets, the clock, the
 * Wi-Fi state, the lock — goes through DlOps, so tests/test_dl_core.c can run
 * the real code against a fake card and a fake server and replay the
 * situations a console only produces by accident: a connection cut after
 * 1,234,567 bytes, a URL that expires halfway, a card that fills up mid-write.
 * src/dl/dl.c binds the ops to sceIo, vita_https and the app's other layers
 * and runs dl_core_step() on a worker thread.
 *
 * The work is cut into UNITS: one dl_core_step() does one thing (resolve a
 * URL, fetch one range, verify a file, fetch a thumbnail, reap a deleted
 * item, write the index) and returns, so a test drives the engine without a
 * thread and the worker stays responsive to the commands in between.
 *
 * Locking. DlOps.lock/unlock protect the core's own state and are held only
 * for short bookkeeping, NEVER across a file or network call: the UI thread
 * calls dl_core_enqueue/pause/snapshot... at frame rate and must not wait for
 * a socket or a memory card. The test ops assert exactly that. */
#ifndef VITUBE_DL_DL_CORE_H
#define VITUBE_DL_DL_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "dl/dl.h"
#include "yt/model.h"

/* --- tuning --------------------------------------------------------------- */

/* Entries the list can hold (done + queued + failed). A few hundred bytes
 * each, held statically. */
#define DL_MAX_ITEMS 128

/* One Range request. googlevideo throttles open-ended ranges but serves
 * closed ones of this size at full speed (yt-dlp's http_chunk_size does the
 * same). Not smaller because every request is a fresh TCP + TLS handshake
 * (the app does not reuse connections to googlevideo, see hls.c): at 2 MiB
 * the handshake costs as much as a third of the transfer on the console's
 * Wi-Fi. Not larger because a pause or a cut loses nothing either way — the
 * bytes already received are written before a request is given up. */
#define DL_CHUNK_BYTES (4LL * 1024 * 1024)
/* Received bytes are collected and written to the card in pieces of this
 * size, so the card sees large writes and the I/O lock is taken rarely. */
#define DL_WBUF_BYTES (128 * 1024)
/* ... or sooner when this long has passed since the last write, so a slow
 * link still shows progress and loses at most a second of data to a crash. */
#define DL_FLUSH_AGE_MS 1000
/* Free space is checked again after this many written bytes. */
#define DL_SPACE_RECHECK_BYTES (8LL * 1024 * 1024)
/* googlevideo URLs live about six hours; renewing at five avoids the 403. */
#define DL_URL_MAX_AGE_MS (5ULL * 3600 * 1000)
/* Failures, with the network up and no byte of progress in between, before
 * the item gives up as DL_ERR_NETWORK. */
#define DL_NET_FAIL_LIMIT 8
#define DL_BACKOFF_FIRST_MS 1000
#define DL_BACKOFF_MAX_MS 8000
/* 403/404/410 on a freshly renewed URL this many times in a row: DL_ERR_URL. */
#define DL_URL_FAIL_LIMIT 3
/* A resolve that YouTube answers with an error (not a network failure). */
#define DL_RESOLVE_FAIL_LIMIT 4
/* Restarts from byte 0 (server ignored Range, changed length, ...) per run. */
#define DL_RESTART_LIMIT 5
/* Thumbnail attempts per item over its whole life. */
#define DL_THUMB_TRIES 3
/* What src/ui/thumbs.c is willing to load; a bigger file would be refused
 * there anyway, so it is not saved here. */
#define DL_THUMB_MAX_BYTES (256 * 1024)
/* The one receive buffer: chunks are written from it in DL_WBUF_BYTES pieces,
 * a thumbnail is collected in all of it. */
#define DL_BUF_BYTES DL_THUMB_MAX_BYTES
#define DL_INDEX_WRITE_MS 5000
#define DL_PROGRESS_BUMP_MS 250
#define DL_SPEED_WINDOW_MS 500
/* dl_core_storage asks the card at most this often, however often it is
 * called; short, because a delete or a finished download changes the answer
 * and the Downloads screen asks right after one. */
#define DL_STORAGE_CACHE_MS 250
/* Largest index.json that is read at all (a full list is ~100 KB). */
#define DL_INDEX_MAX_BYTES (512 * 1024)
/* Sleep hints dl_core_step returns when it has nothing to do right now. */
#define DL_POLL_MS 100
#define DL_IDLE_MS 250

/* --- file system ops ------------------------------------------------------ */

#define DL_FS_IO (-1)     /* any failure */
#define DL_FS_NOSPC (-2)  /* the card is full */

#define DL_FS_READ 1      /* existing file, read only */
#define DL_FS_WRITE 2     /* create if missing, never truncate */
#define DL_FS_TRUNC 4     /* with DL_FS_WRITE: start from an empty file */

/* --- network ops ---------------------------------------------------------- */

#define DL_NET_OK 0
#define DL_NET_TRANSPORT (-1)  /* DNS/connect/TLS/timeout/reset: the network failed */
#define DL_NET_ABORTED (-2)    /* a sink callback asked to stop */
#define DL_NET_CANCELLED (-3)  /* the cancel flag became non-zero */
#define DL_NET_FAILED (-4)     /* resolve only: YouTube answered, with an error */

/* Where a fetched response goes. headers() is called once per complete
 * response header block BEFORE any body byte (redirect hops included — the
 * core ignores 3xx), data() with the body in pieces of any size. Either
 * returns non-zero to stop the transfer; the ops then return DL_NET_ABORTED.
 * content_length is -1 when absent, content_range "" when absent. */
typedef struct DlSink {
    void *ctx;
    int (*headers)(void *ctx, int status, int64_t content_length,
                   const char *content_range);
    int (*data)(void *ctx, const void *buf, size_t len);
} DlSink;

typedef struct DlOps {
    void *user;

    /* Protects the core's state. NULL = single threaded. Not recursive. */
    void (*lock)(void *user);
    void (*unlock)(void *user);

    /* Cheap and I/O free; may be called with the lock held. */
    uint64_t (*now_ms)(void *user);
    int (*net_connected)(void *user);
    /* Changes whenever the network came back or the console woke up. */
    unsigned (*net_generation)(void *user);
    /* One line, no newline. Called outside the lock. NULL = silent. */
    void (*log)(void *user, const char *line);

    /* Files. Paths are full. Never called with the lock held. */
    int (*fs_mkdir)(void *user, const char *path);          /* ok if it exists */
    int (*fs_open)(void *user, const char *path, int mode);  /* fd >= 0 or <0 */
    int (*fs_close)(void *user, int fd);
    /* Bytes transferred (a short count is allowed) or DL_FS_*. A write that
     * can store nothing more returns DL_FS_NOSPC (or 0). */
    int64_t (*fs_pread)(void *user, int fd, void *buf, size_t len, int64_t off);
    int64_t (*fs_pwrite)(void *user, int fd, const void *buf, size_t len,
                         int64_t off);
    int (*fs_sync)(void *user, int fd);                     /* may be NULL */
    int64_t (*fs_size)(void *user, const char *path);       /* <0 if missing */
    /* The destination does not exist (the core removes it first). */
    int (*fs_rename)(void *user, const char *from, const char *to);
    int (*fs_unlink)(void *user, const char *path);         /* ok if missing */
    int (*fs_free)(void *user, int64_t *total, int64_t *free_bytes);

    /* The same call as yt_resolve_stream; a thin wrapper around it on the
     * console. Returns DL_NET_OK, DL_NET_TRANSPORT, DL_NET_FAILED or
     * DL_NET_CANCELLED. */
    int (*net_resolve)(void *user, const char *video_id, volatile int *cancel,
                       yt_stream_t *out);
    /* GET `url`. first >= 0: "Range: bytes=first-last" (last inclusive);
     * first < 0: a plain GET. Returns DL_NET_OK when the response ended
     * normally, else one of the DL_NET_ codes. Must also stop (as
     * DL_NET_TRANSPORT) when the network generation changes mid-request: the
     * socket is dead. */
    int (*net_fetch)(void *user, const char *url, int64_t first, int64_t last,
                     const DlSink *sink, volatile int *cancel);
} DlOps;

/* --- state ---------------------------------------------------------------- */

typedef struct {
    DlItem pub;                    /* what dl_snapshot hands out */
    char thumb_url[YT_THUMB_URL_MAX];
    int itag;                      /* of the format being saved, 0 = not chosen */
    uint32_t uid;                  /* never reused: survives delete + enqueue */
    uint32_t qseq;                 /* FIFO position among QUEUED items */
    unsigned char pending_delete;  /* hidden; the worker removes the files */
    unsigned char wipe;            /* next run starts from byte 0 */
    unsigned char thumb_ok;        /* <id>.jpg is on the card */
    unsigned char thumb_tries;
    unsigned char wait_kind;       /* DL_WAIT_* */
    unsigned char force_try;       /* the user said "try now": skip the
                                    * connectivity check once */
    int fail_count;                /* consecutive failures without progress */
    int url_fails;
    int resolve_fails;
    int restarts;
    uint64_t next_try_ms;
    unsigned wait_gen;
} DlCoreItem;

/* Why an item is DL_WAITING. */
enum { DL_WAIT_NONE, DL_WAIT_TIMER, DL_WAIT_OFFLINE, DL_WAIT_HOLD };

/* The request in flight (worker thread only; the sink callbacks fill it). */
typedef struct {
    uint32_t uid;
    int64_t first, last;      /* requested range; first < 0 = whole body */
    int64_t file_off;         /* next write position = contiguous bytes stored */
    int64_t expect_len;       /* body length the accepted response promised */
    int64_t got;              /* body bytes taken from the network */
    int fd;
    int accepted;
    int status;
    int verdict;              /* DL_XV_* */
    unsigned gen0;            /* network generation when the request began */
    size_t wlen;
    uint64_t flush_ms;
} DlXfer;

typedef struct DlCore {
    DlOps ops;
    DlCoreItem items[DL_MAX_ITEMS];
    int count;
    unsigned revision;
    uint32_t next_uid;
    uint32_t next_qseq;
    int hold;                 /* nesting of dl_playback_begin */
    int running;              /* dl_core_load done, not stopped */
    int stopping;
    uint32_t cur_uid;         /* the item that owns the one download slot */
    volatile int cancel;      /* abort the request in flight */
    int load_dirty;

    int index_dirty;          /* write at the next step */
    int progress_dirty;       /* write within DL_INDEX_WRITE_MS */
    uint64_t index_ms;

    int64_t st_total, st_free;
    uint64_t st_ms;
    int st_valid;

    /* Renewed URL of the current item. Never persisted: googlevideo URLs are
     * bound to the IP they were issued to. */
    char url[YT_URL_MAX];
    int url_valid;
    uint32_t url_uid;         /* the item the URL was issued for */
    uint64_t url_ms;
    int64_t since_check;      /* written since the last free-space check */
    uint64_t thumb_next_ms;   /* no thumbnail attempt before this */
    int thumb_streak;         /* thumbnail attempts in a row that failed */

    uint64_t spd_ms;          /* speed window */
    int64_t spd_bytes;
    uint64_t bump_ms;
    int pending_bump;

    char log_ring[16][176];
    unsigned log_head, log_tail;

    yt_stream_t stream;       /* resolve scratch */
    DlXfer x;
    unsigned char wbuf[DL_BUF_BYTES];
} DlCore;

/* --- engine --------------------------------------------------------------- */

void dl_core_init(DlCore *c, const DlOps *ops);
/* Reads index.json, reconciles it with the card (see dl_init in dl.h) and
 * arms the engine. Returns 0. */
int dl_core_load(DlCore *c);
/* One unit of work. Returns 0 when more work is ready (call again at once),
 * else how many milliseconds the caller may sleep before calling again. */
int dl_core_step(DlCore *c);
/* Asks the request in flight to stop and makes dl_core_step a no-op. */
void dl_core_stop(DlCore *c);
/* Writes index.json if anything changed since it was last written. Call
 * after the worker has stopped. */
void dl_core_flush(DlCore *c);

/* The dl.h API with the engine passed in. */
DlEnqueue dl_core_enqueue(DlCore *c, const yt_video_t *video);
void dl_core_pause(DlCore *c, const char *id);
void dl_core_resume(DlCore *c, const char *id);
void dl_core_pause_all(DlCore *c);
void dl_core_resume_all(DlCore *c);
void dl_core_delete(DlCore *c, const char *id);
int dl_core_count(DlCore *c);
int dl_core_snapshot(DlCore *c, DlItem *out, int max);
int dl_core_get(DlCore *c, const char *id, DlItem *out);
int dl_core_active_count(DlCore *c);
unsigned dl_core_revision(DlCore *c);
int dl_core_storage(DlCore *c, DlStorage *out);
int dl_core_local_path(DlCore *c, const char *id, char *path, size_t cap);
int dl_core_thumb_path(DlCore *c, const char *id, char *path, size_t cap);
void dl_core_report_bad_file(DlCore *c, const char *id);
void dl_core_playback_begin(DlCore *c);
void dl_core_playback_end(DlCore *c);

/* --- pieces with their own tests ------------------------------------------ */

/* [A-Za-z0-9_-], exactly YT_VIDEO_ID_LEN-1 characters. The only gate between
 * text from outside (a list, index.json) and a file name. */
int dl_valid_id(const char *id);

/* Copies src into dst (cap bytes) as valid UTF-8 that fits: invalid bytes
 * become '?', control characters become spaces, nothing is cut mid-sequence.
 * index.json is parsed with a strict UTF-8 parser — one bad title must not
 * make the whole file unreadable. */
void dl_clean_copy(char *dst, size_t cap, const char *src);

/* The serialised list (malloc'd, NUL-terminated; free()) — states that mean
 * "was running" are written as queued. NULL on failure. Lock held. */
char *dl_index_serialize(const DlCore *c, size_t *len);
/* Replaces the list with what `text` holds; ids that are not valid ids and
 * duplicates are skipped, every string is length-bounded, missing fields get
 * defaults. Returns the number of entries kept, or -1 when `text` is not an
 * index at all. */
int dl_index_parse(DlCore *c, const char *text, size_t len);

/* Reads `len` bytes at `off`; returns the bytes read (a short read at EOF is
 * fine) or <0. */
typedef int64_t (*DlReadFn)(void *ctx, int64_t off, void *buf, size_t len);

#define DL_MP4_OK 0
#define DL_MP4_TOO_SMALL (-1)
#define DL_MP4_NO_FTYP (-2)
#define DL_MP4_BAD_BOX (-3)
#define DL_MP4_TRUNCATED (-4)
#define DL_MP4_NO_MOOV (-5)
#define DL_MP4_NO_MDAT (-6)
#define DL_MP4_READ (-7)
#define DL_MP4_TOO_MANY (-8)
/* Walks the top-level boxes of a file of `size` bytes: the first is ftyp,
 * 32-bit sizes, 64-bit largesize (size == 1) and "to the end" (size == 0) are
 * understood, every box fits, the walk ends exactly at the end of the file,
 * and both moov and mdat were seen. */
int dl_mp4_verify(DlReadFn read, void *ctx, int64_t size);

#endif
