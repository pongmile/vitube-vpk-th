/* ViTube — the download engine's state machine; see dl_core.h.
 *
 * Reading guide. Every function whose name starts with unit_ does ONE piece
 * of work for the item that owns the download slot (c->cur_uid) and follows
 * one convention: it is entered with the core lock HELD and returns with it
 * RELEASED, because the piece of work itself (a socket, a file) must run
 * without the lock. After any unlocked stretch the item is found again by its
 * uid — never by a pointer or index kept across the gap, since the UI thread
 * may have paused it, deleted it (and enqueued the same video again) or
 * compacted the list in the meantime.
 *
 * What the worker decides lives here; what the UI thread may do while the
 * worker is mid-request is limited to flipping an item's state and raising
 * c->cancel, and the worker re-reads the state before it acts on anything. */
#include "dl/dl_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCK(c) do { if ((c)->ops.lock) (c)->ops.lock((c)->ops.user); } while (0)
#define UNLOCK(c) do { if ((c)->ops.unlock) (c)->ops.unlock((c)->ops.user); } while (0)

#define INDEX_TMP VITUBE_DL_INDEX_PATH ".tmp"
#define INDEX_BAK VITUBE_DL_INDEX_PATH ".bak"

/* How a request ended, as far as the sink could tell (DlXfer.verdict). */
enum {
    DL_XV_NONE = 0,
    DL_XV_BAD,        /* the answer does not match what was asked */
    DL_XV_HTTP,       /* an HTTP status that is a failure */
    DL_XV_URL,        /* 403/404/410: the URL is no good any more */
    DL_XV_RESTART,    /* what is on the card cannot be continued */
    DL_XV_NO_SPACE,   /* the size just became known and does not fit */
    DL_XV_DISK_FULL,  /* a write found the card full */
    DL_XV_SPACE_LOW,  /* the periodic check found the card nearly full */
    DL_XV_IO,         /* any other write failure */
    DL_XV_OVERRUN     /* the server sent more than it promised */
};

const char *dl_error_key(DlError error) {
    switch (error) {
    case DL_ERR_NONE: return "none";
    case DL_ERR_NO_SPACE: return "no_space";
    case DL_ERR_DISK_FULL: return "disk_full";
    case DL_ERR_NOT_DOWNLOADABLE: return "not_downloadable";
    case DL_ERR_VERIFY: return "verify";
    case DL_ERR_IO: return "io";
    case DL_ERR_NETWORK: return "network";
    case DL_ERR_URL: return "url";
    }
    return "io";
}

/* --- small helpers --------------------------------------------------------- */

/* Deferred logging. A log line costs a memory-card write on the console (and
 * the app-wide I/O lock), which must never happen while the UI thread may be
 * waiting on the core lock — so lines are queued here under the lock and the
 * worker hands them to ops.log from dl_core_step, outside it. */
static void core_log(DlCore *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void core_log(DlCore *c, const char *fmt, ...) {
    if (!c->ops.log) return;
    char *slot = c->log_ring[c->log_head % 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot, sizeof c->log_ring[0], fmt, ap);
    va_end(ap);
    c->log_head++;
    if (c->log_head - c->log_tail > 16) c->log_tail = c->log_head - 16;
}

static void drain_log(DlCore *c) {
    if (!c->ops.log) return;
    char line[sizeof c->log_ring[0]];
    for (;;) {
        LOCK(c);
        if (c->log_tail == c->log_head) {
            UNLOCK(c);
            return;
        }
        memcpy(line, c->log_ring[c->log_tail % 16], sizeof line);
        c->log_tail++;
        UNLOCK(c);
        c->ops.log(c->ops.user, line);
    }
}

static uint64_t now_ms(DlCore *c) { return c->ops.now_ms(c->ops.user); }

/* The ONLY place a file name is built. An id that is not a valid id yields
 * -1 and an empty path, so no caller can reach the card with text that came
 * from outside. */
static int path_of(char *out, size_t cap, const char *id, const char *ext) {
    out[0] = '\0';
    if (!dl_valid_id(id)) return -1;
    int n = snprintf(out, cap, VITUBE_DL_DIR "/%s.%s", id, ext);
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

#define PATH_CAP 96

static DlCoreItem *find_live(DlCore *c, const char *id) {
    for (int i = 0; i < c->count; i++)
        if (!c->items[i].pending_delete &&
            strcmp(c->items[i].pub.video_id, id) == 0)
            return &c->items[i];
    return NULL;
}

static DlCoreItem *find_uid(DlCore *c, uint32_t uid) {
    if (!uid) return NULL;
    for (int i = 0; i < c->count; i++)
        if (c->items[i].uid == uid) return &c->items[i];
    return NULL;
}

static void bump(DlCore *c) { c->revision++; }

/* What index.json can tell apart: every state that means "was working on it"
 * is one class, since it restarts as queued anyway. */
static int pclass(DlState s) {
    switch (s) {
    case DL_PAUSED: return 1;
    case DL_FAILED: return 2;
    case DL_DONE: return 3;
    default: return 0;
    }
}

static void set_state(DlCore *c, DlCoreItem *it, DlState s) {
    if (it->pub.state == s) return;
    if (pclass(it->pub.state) != pclass(s)) c->index_dirty = 1;
    it->pub.state = s;
    it->pub.bytes_per_sec = 0;
    it->pub.retry_in_s = 0;
    if (s != DL_WAITING) it->wait_kind = DL_WAIT_NONE;
    if (s == DL_DOWNLOADING) {
        c->spd_ms = now_ms(c);
        c->spd_bytes = 0;
    }
    bump(c);
}

static void fail_item(DlCore *c, DlCoreItem *it, DlError e) {
    it->pub.error = e;
    it->force_try = 0;
    c->st_valid = 0; /* a failure is often about space: look again */
    set_state(c, it, DL_FAILED);
    c->index_dirty = 1;
    if (c->cur_uid == it->uid) c->cur_uid = 0;
    core_log(c, "dl %s: failed (%s) at %lld of %lld", it->pub.video_id,
             dl_error_key(e), (long long)it->pub.bytes_done,
             (long long)it->pub.bytes_total);
}

static int slot_active(const DlCoreItem *it) {
    if (it->pending_delete) return 0;
    switch (it->pub.state) {
    case DL_QUEUED: case DL_RESOLVING: case DL_DOWNLOADING:
    case DL_WAITING: case DL_VERIFYING:
        return 1;
    default:
        return 0;
    }
}

static uint64_t backoff_ms(int failures) {
    uint64_t d = DL_BACKOFF_FIRST_MS;
    for (int i = 1; i < failures && d < DL_BACKOFF_MAX_MS; i++) d *= 2;
    return d > DL_BACKOFF_MAX_MS ? DL_BACKOFF_MAX_MS : d;
}

/* Puts the item to wait. kind: DL_WAIT_TIMER (retry after delay_ms),
 * DL_WAIT_OFFLINE (until the network is back) or DL_WAIT_HOLD (until the
 * online playback is over). Lock held. */
static void enter_wait(DlCore *c, DlCoreItem *it, int kind, uint64_t delay_ms) {
    set_state(c, it, DL_WAITING);
    it->wait_kind = (unsigned char)kind;
    it->wait_gen = c->ops.net_generation(c->ops.user);
    it->next_try_ms = now_ms(c) + delay_ms;
    it->pub.retry_in_s = kind == DL_WAIT_TIMER ? (int)((delay_ms + 999) / 1000)
                                               : -1;
    if (kind == DL_WAIT_OFFLINE) it->fail_count = 0;
    bump(c);
}

/* Before anything that needs the network: waiting for an online video to end
 * or for Wi-Fi is not a failure and counts for nothing. Returns 1 when the
 * item was put to wait. Lock held. */
static int gate(DlCore *c, DlCoreItem *it) {
    if (c->hold > 0) {
        enter_wait(c, it, DL_WAIT_HOLD, 0);
        return 1;
    }
    if (c->ops.net_connected(c->ops.user)) {
        it->force_try = 0; /* the flag has caught up */
        return 0;
    }
    if (it->force_try) {
        /* The user pressed retry: believe them over the connectivity flag
         * (it is sampled a few times a second) until an attempt fails. A real
         * outage fails the very next request and lands back in the wait. */
        return 0;
    }
    enter_wait(c, it, DL_WAIT_OFFLINE, 0);
    return 1;
}

/* A request or resolve failed at the transport. gen0 is the network
 * generation when it started. Lock held. */
static void note_failure(DlCore *c, DlCoreItem *it, int64_t progress,
                         unsigned gen0, const char *what) {
    it->force_try = 0;
    unsigned gen = c->ops.net_generation(c->ops.user);
    if (gen != gen0) {
        /* The Wi-Fi came back or the console woke up while this was in
         * flight: the socket was dead, nothing is wrong with the video. Try
         * again at once and do not count it. */
        it->fail_count = 0;
        core_log(c, "dl %s: %s interrupted by a network change, retrying",
                 it->pub.video_id, what);
        return;
    }
    if (!c->ops.net_connected(c->ops.user)) {
        enter_wait(c, it, DL_WAIT_OFFLINE, 0);
        core_log(c, "dl %s: %s failed, offline", it->pub.video_id, what);
        return;
    }
    if (progress > 0) it->fail_count = 0;
    it->fail_count++;
    core_log(c, "dl %s: %s failed (%d of %d)", it->pub.video_id, what,
             it->fail_count, DL_NET_FAIL_LIMIT);
    if (it->fail_count >= DL_NET_FAIL_LIMIT)
        fail_item(c, it, DL_ERR_NETWORK);
    else
        enter_wait(c, it, DL_WAIT_TIMER, backoff_ms(it->fail_count));
}

/* --- files ----------------------------------------------------------------- */

static int write_whole(DlCore *c, const char *path, const char *data,
                       size_t len) {
    int fd = c->ops.fs_open(c->ops.user, path, DL_FS_WRITE | DL_FS_TRUNC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        /* Each call holds the app-wide I/O lock for its whole duration: no
         * single write bigger than a coalesced chunk. */
        size_t piece = len - done;
        if (piece > DL_WBUF_BYTES) piece = DL_WBUF_BYTES;
        int64_t w = c->ops.fs_pwrite(c->ops.user, fd, data + done, piece,
                                     (int64_t)done);
        if (w <= 0) {
            c->ops.fs_close(c->ops.user, fd);
            c->ops.fs_unlink(c->ops.user, path);
            return -1;
        }
        done += (size_t)w;
    }
    if (c->ops.fs_sync) c->ops.fs_sync(c->ops.user, fd);
    c->ops.fs_close(c->ops.user, fd);
    return 0;
}

/* index.json is replaced without a moment in which no complete copy exists:
 * the new text goes to index.json.tmp first, the current file is kept as
 * index.json.bak, and only then does the tmp take its place. dl_core_load
 * looks at all three. (Nothing renames onto an existing file; sceIoRename's
 * behaviour for that is not something to depend on.) */
static int index_write(DlCore *c, const char *text, size_t len) {
    const DlOps *o = &c->ops;
    if (write_whole(c, INDEX_TMP, text, len) < 0) return -1;
    o->fs_unlink(o->user, INDEX_BAK);
    if (o->fs_size(o->user, VITUBE_DL_INDEX_PATH) >= 0 &&
        o->fs_rename(o->user, VITUBE_DL_INDEX_PATH, INDEX_BAK) < 0)
        o->fs_unlink(o->user, VITUBE_DL_INDEX_PATH);
    if (o->fs_rename(o->user, INDEX_TMP, VITUBE_DL_INDEX_PATH) < 0) {
        o->fs_rename(o->user, INDEX_BAK, VITUBE_DL_INDEX_PATH);
        return -1;
    }
    return 0;
}

/* Writes index.json if it is due. Not under the lock: it serialises under it
 * and writes outside it. */
static void index_maintain(DlCore *c, int force) {
    LOCK(c);
    uint64_t now = now_ms(c);
    int due = c->index_dirty ||
              (c->progress_dirty && now - c->index_ms >= DL_INDEX_WRITE_MS);
    if (!due && !(force && c->progress_dirty)) {
        UNLOCK(c);
        return;
    }
    size_t len = 0;
    char *text = dl_index_serialize(c, &len);
    c->index_dirty = 0;
    c->progress_dirty = 0;
    c->index_ms = now;
    UNLOCK(c);
    if (!text) return;
    if (index_write(c, text, len) < 0) {
        LOCK(c);
        c->progress_dirty = 1; /* try again in a few seconds */
        core_log(c, "dl: writing index.json failed");
        UNLOCK(c);
    }
    free(text);
}

static int64_t read_whole(DlCore *c, const char *path, char *buf, size_t cap) {
    int64_t size = c->ops.fs_size(c->ops.user, path);
    if (size <= 0 || (size_t)size > cap) return -1;
    int fd = c->ops.fs_open(c->ops.user, path, DL_FS_READ);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < (size_t)size) {
        size_t piece = (size_t)size - got;
        if (piece > DL_WBUF_BYTES) piece = DL_WBUF_BYTES;
        int64_t r = c->ops.fs_pread(c->ops.user, fd, buf + got, piece,
                                    (int64_t)got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    c->ops.fs_close(c->ops.user, fd);
    return got == (size_t)size ? (int64_t)got : -1;
}

/* --- engine life cycle ----------------------------------------------------- */

void dl_core_init(DlCore *c, const DlOps *ops) {
    memset(c, 0, sizeof *c);
    c->ops = *ops;
    c->revision = 1;
}

static void remove_slot(DlCore *c, int i) {
    memmove(&c->items[i], &c->items[i + 1],
            (size_t)(c->count - i - 1) * sizeof c->items[0]);
    c->count--;
}

int dl_core_load(DlCore *c) {
    const DlOps *o = &c->ops;
    o->fs_mkdir(o->user, VITUBE_DL_DIR);

    char *buf = malloc(DL_INDEX_MAX_BYTES + 1);
    int loaded = 0;
    static const char *const names[] = { VITUBE_DL_INDEX_PATH, INDEX_TMP,
                                         INDEX_BAK };
    LOCK(c);
    c->count = 0;
    if (buf) {
        for (int k = 0; k < 3 && !loaded; k++) {
            UNLOCK(c);
            int64_t n = read_whole(c, names[k], buf, DL_INDEX_MAX_BYTES);
            LOCK(c);
            if (n <= 0) continue;
            buf[n] = '\0';
            if (dl_index_parse(c, buf, (size_t)n) >= 0) {
                loaded = 1;
                /* A copy other than index.json: the next write must replace
                 * the damaged or missing main file. */
                if (k != 0) c->index_dirty = 1;
                core_log(c, "dl: index loaded from %s, %d entries", names[k],
                         c->count);
            } else {
                core_log(c, "dl: %s is not an index", names[k]);
            }
        }
        free(buf);
    }
    if (!loaded) c->count = 0;
    UNLOCK(c);

    /* Reconcile every entry with the card. The .part's size is the truth
     * about how much is stored — the index is only rewritten every few
     * seconds, so it can be behind (or, after a crash, ahead). */
    for (int i = 0;;) {
        LOCK(c);
        if (i >= c->count) {
            UNLOCK(c);
            break;
        }
        DlCoreItem copy = c->items[i];
        UNLOCK(c);

        char part[PATH_CAP], mp4[PATH_CAP], jpg[PATH_CAP];
        path_of(part, sizeof part, copy.pub.video_id, "part");
        path_of(mp4, sizeof mp4, copy.pub.video_id, "mp4");
        path_of(jpg, sizeof jpg, copy.pub.video_id, "jpg");
        int keep = 1;
        DlState state = copy.pub.state;
        int64_t done = copy.pub.bytes_done;
        if (state == DL_DONE) {
            int64_t sz = o->fs_size(o->user, mp4);
            if (sz < 0 || copy.pub.bytes_total <= 0 ||
                sz != copy.pub.bytes_total)
                keep = 0;
            else
                done = sz;
        } else {
            int64_t psize = o->fs_size(o->user, part);
            if (psize < 0) {
                int64_t msz = o->fs_size(o->user, mp4);
                if (msz > 0 && copy.pub.bytes_total > 0 &&
                    msz == copy.pub.bytes_total) {
                    /* Verified and renamed, then the app died before the
                     * index said so. */
                    state = DL_DONE;
                    done = msz;
                } else {
                    done = 0;
                }
            } else if (copy.pub.bytes_total > 0 &&
                       psize > copy.pub.bytes_total) {
                /* Larger than the file can be: it is not this file. */
                o->fs_unlink(o->user, part);
                done = 0;
            } else {
                done = psize;
            }
        }
        int thumb = keep && o->fs_size(o->user, jpg) > 0;
        if (!keep) {
            o->fs_unlink(o->user, part);
            o->fs_unlink(o->user, jpg);
        }

        LOCK(c);
        if (!keep) {
            remove_slot(c, i);
            c->index_dirty = 1;
            core_log(c, "dl %s: file is gone, entry dropped", copy.pub.video_id);
        } else {
            DlCoreItem *it = &c->items[i];
            if (it->pub.state != state) c->index_dirty = 1;
            it->pub.state = state;
            if (it->pub.bytes_done != done) c->index_dirty = 1;
            it->pub.bytes_done = done;
            it->thumb_ok = (unsigned char)thumb;
            i++;
        }
        UNLOCK(c);
    }

    LOCK(c);
    c->running = 1;
    c->stopping = 0;
    bump(c);
    UNLOCK(c);
    drain_log(c);
    return 0;
}

void dl_core_stop(DlCore *c) {
    LOCK(c);
    c->stopping = 1;
    c->cancel = 1;
    UNLOCK(c);
}

void dl_core_flush(DlCore *c) {
    LOCK(c);
    c->index_dirty = c->index_dirty || c->progress_dirty;
    UNLOCK(c);
    index_maintain(c, 1);
    drain_log(c);
}

/* --- reaping deleted items ------------------------------------------------- */

/* dl_delete only hides the item and raises the cancel flag; the files go here,
 * on the worker, once nothing can still be writing them. */
static int reap_one(DlCore *c) {
    LOCK(c);
    int found = -1;
    for (int i = 0; i < c->count; i++)
        if (c->items[i].pending_delete) { found = i; break; }
    if (found < 0) {
        UNLOCK(c);
        return 0;
    }
    uint32_t uid = c->items[found].uid;
    char id[YT_VIDEO_ID_LEN];
    memcpy(id, c->items[found].pub.video_id, sizeof id);
    if (c->cur_uid == uid) c->cur_uid = 0;
    UNLOCK(c);

    char path[PATH_CAP];
    static const char *const exts[] = { "part", "mp4", "jpg", "jpg.tmp" };
    for (size_t k = 0; k < sizeof exts / sizeof exts[0]; k++)
        if (path_of(path, sizeof path, id, exts[k]) == 0)
            c->ops.fs_unlink(c->ops.user, path);

    LOCK(c);
    for (int i = 0; i < c->count; i++)
        if (c->items[i].uid == uid) {
            remove_slot(c, i);
            break;
        }
    c->index_dirty = 1;
    c->st_valid = 0; /* the card has more room now */
    bump(c);
    core_log(c, "dl %s: deleted", id);
    UNLOCK(c);
    return 1;
}

/* --- the sink: where a response goes --------------------------------------- */

static int parse_u63(const char **pp, int64_t *out) {
    const char *p = *pp;
    if (*p < '0' || *p > '9') return 0;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v > ((int64_t)1 << 56)) return 0;
        v = v * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    *out = v;
    return 1;
}

/* "bytes 100-199/1000"; total is -1 when the server wrote a star for it. */
static int parse_content_range(const char *s, int64_t *first, int64_t *last,
                               int64_t *total) {
    if (!s || strncmp(s, "bytes ", 6) != 0) return 0;
    s += 6;
    while (*s == ' ') s++;
    if (!parse_u63(&s, first) || *s++ != '-') return 0;
    if (!parse_u63(&s, last) || *s++ != '/') return 0;
    if (*s == '*') {
        *total = -1;
        return 1;
    }
    return parse_u63(&s, total);
}

/* Writes what has been collected. Not under the lock. On a failure the
 * unwritten rest is dropped: those bytes are requested again from file_off,
 * which is exactly the number of contiguous bytes on the card. */
static int xfer_flush(DlCore *c) {
    DlXfer *x = &c->x;
    size_t n = x->wlen;
    if (n == 0) return 0;
    size_t done = 0;
    int err = 0;
    while (done < n) {
        int64_t w = c->ops.fs_pwrite(c->ops.user, x->fd, c->wbuf + done,
                                     n - done, x->file_off + (int64_t)done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        err = (w == DL_FS_NOSPC || w == 0) ? DL_XV_DISK_FULL : DL_XV_IO;
        break;
    }
    x->wlen = 0;
    x->file_off += (int64_t)done;
    c->since_check += (int64_t)done;
    uint64_t now = now_ms(c);
    x->flush_ms = now;

    int64_t total = 0;
    LOCK(c);
    DlCoreItem *it = find_uid(c, x->uid);
    if (it) {
        it->pub.bytes_done = x->file_off;
        total = it->pub.bytes_total;
        if (it->pub.state == DL_DOWNLOADING && now > c->spd_ms) {
            c->spd_bytes += (int64_t)done;
            if (now - c->spd_ms >= DL_SPEED_WINDOW_MS) {
                int64_t inst = c->spd_bytes * 1000 / (int64_t)(now - c->spd_ms);
                int64_t cur = it->pub.bytes_per_sec;
                it->pub.bytes_per_sec =
                    (int)(cur > 0 ? (cur * 3 + inst) / 4 : inst);
                c->spd_ms = now;
                c->spd_bytes = 0;
            }
        }
    }
    if (done) {
        c->progress_dirty = 1;
        if (now - c->bump_ms >= DL_PROGRESS_BUMP_MS) {
            bump(c);
            c->bump_ms = now;
            c->pending_bump = 0;
        } else {
            c->pending_bump = 1;
        }
    }
    UNLOCK(c);

    if (err) {
        x->verdict = err;
        return -1;
    }
    /* Someone else may be using the card too (a screenshot, the log): look
     * again every few megabytes, not just at the start. Half the reserve is
     * allowed to go, the rest of it is what everything else lives on. */
    if (c->since_check >= DL_SPACE_RECHECK_BYTES && total > 0) {
        c->since_check = 0;
        int64_t card_total = 0, card_free = 0;
        if (c->ops.fs_free(c->ops.user, &card_total, &card_free) == 0 &&
            card_free < (total - x->file_off) + DL_RESERVE_BYTES / 2) {
            x->verdict = DL_XV_SPACE_LOW;
            return -1;
        }
    }
    return 0;
}

static int sink_data(void *ctx, const void *buf, size_t len) {
    DlCore *c = ctx;
    DlXfer *x = &c->x;
    if (!x->accepted || x->verdict != DL_XV_NONE) return 1;
    if (c->cancel) return 1;
    int64_t room = x->expect_len - x->got;
    if ((int64_t)len > room) {
        /* More than the response promised: keep what was promised, refuse
         * the rest, and let the request end as a failure. */
        len = room > 0 ? (size_t)room : 0;
        x->verdict = DL_XV_OVERRUN;
    }
    const unsigned char *p = buf;
    while (len > 0) {
        size_t space = DL_WBUF_BYTES - x->wlen;
        size_t n = len < space ? len : space;
        memcpy(c->wbuf + x->wlen, p, n);
        x->wlen += n;
        x->got += (int64_t)n;
        p += n;
        len -= n;
        if (x->wlen == DL_WBUF_BYTES && xfer_flush(c) != 0) return 1;
    }
    if (x->wlen > 0 && now_ms(c) - x->flush_ms >= DL_FLUSH_AGE_MS &&
        xfer_flush(c) != 0)
        return 1;
    return x->verdict != DL_XV_NONE;
}

static int sink_headers(void *ctx, int status, int64_t clen,
                        const char *crange) {
    DlCore *c = ctx;
    DlXfer *x = &c->x;
    if (status < 200) return 0;
    if (status >= 300 && status < 400) return 0; /* a redirect hop */
    if (x->accepted) return 0;
    x->status = status;
    if (status == 403 || status == 404 || status == 410) {
        x->verdict = DL_XV_URL;
        return 1;
    }
    if (status == 416) {
        /* Asked for bytes the file does not have: our idea of the file and
         * the server's differ. From the start is the only fix. */
        x->verdict = x->first > 0 ? DL_XV_RESTART : DL_XV_HTTP;
        return 1;
    }
    int64_t expect = 0, total_hdr = -1;
    if (status == 206) {
        int64_t s = 0, e = 0, t = 0;
        if (!parse_content_range(crange, &s, &e, &t) || s != x->first ||
            e < s || (x->last >= 0 && e > x->last) || (t >= 0 && t <= e) ||
            (clen >= 0 && clen != e - s + 1)) {
            x->verdict = DL_XV_BAD;
            return 1;
        }
        expect = e - s + 1;
        total_hdr = t;
    } else if (status == 200) {
        if (x->first > 0) {
            /* The server ignored the Range header and is sending the whole
             * file; writing that at the resume offset would wreck the part
             * file. */
            x->verdict = DL_XV_RESTART;
            return 1;
        }
        expect = clen;
        total_hdr = clen;
    } else {
        x->verdict = DL_XV_HTTP;
        return 1;
    }

    LOCK(c);
    DlCoreItem *it = find_uid(c, x->uid);
    if (!it) {
        UNLOCK(c);
        return 1;
    }
    int64_t cur_total = it->pub.bytes_total;
    if (status == 200 && expect < 0) {
        expect = cur_total;
        total_hdr = cur_total;
    }
    int learned = 0;
    if (total_hdr > 0) {
        if (cur_total > 0 && total_hdr != cur_total) {
            if (x->first > 0) {
                /* The file changed under the URL: what is stored belongs to
                 * another file. */
                it->pub.bytes_total = total_hdr;
                c->index_dirty = 1;
                x->verdict = DL_XV_RESTART;
                core_log(c, "dl %s: length changed %lld -> %lld, starting over",
                         it->pub.video_id, (long long)cur_total,
                         (long long)total_hdr);
                UNLOCK(c);
                return 1;
            }
            /* Nothing stored yet: just take the new length. */
        }
        if (total_hdr != cur_total) {
            it->pub.bytes_total = total_hdr;
            c->index_dirty = 1;
            bump(c);
            learned = 1;
        }
    } else if (cur_total <= 0) {
        /* No way to tell where the file ends. */
        x->verdict = DL_XV_BAD;
        UNLOCK(c);
        return 1;
    }
    UNLOCK(c);

    if (expect <= 0) {
        /* An empty answer would "succeed" without ever moving the download
         * on. */
        x->verdict = DL_XV_BAD;
        return 1;
    }
    if (learned) {
        /* The size became known only now (the resolve did not carry it). */
        int64_t card_total = 0, card_free = 0;
        if (c->ops.fs_free(c->ops.user, &card_total, &card_free) == 0 &&
            (total_hdr - x->first) + DL_RESERVE_BYTES > card_free) {
            x->verdict = DL_XV_NO_SPACE;
            return 1;
        }
    }
    x->expect_len = expect;
    x->accepted = 1;
    return 0;
}

/* --- thumbnails ------------------------------------------------------------ */

static int thumb_headers(void *ctx, int status, int64_t clen,
                         const char *crange) {
    DlCore *c = ctx;
    DlXfer *x = &c->x;
    (void)crange;
    if (status < 200 || (status >= 300 && status < 400)) return 0;
    if (x->accepted) return 0;
    x->status = status;
    if (status != 200 || clen > DL_THUMB_MAX_BYTES) {
        x->verdict = DL_XV_HTTP;
        return 1;
    }
    x->expect_len = clen; /* -1: unknown, the JPEG's own end marker decides */
    x->accepted = 1;
    return 0;
}

static int thumb_data(void *ctx, const void *buf, size_t len) {
    DlCore *c = ctx;
    DlXfer *x = &c->x;
    if (!x->accepted || c->cancel) return 1;
    if (x->wlen + len > DL_THUMB_MAX_BYTES) {
        x->verdict = DL_XV_OVERRUN;
        return 1;
    }
    memcpy(c->wbuf + x->wlen, buf, len);
    x->wlen += len;
    return 0;
}

/* A complete JPEG starts with the SOI marker and ends with EOI. */
static int jpeg_complete(const unsigned char *p, int64_t n) {
    return n > 4 && p[0] == 0xFF && p[1] == 0xD8 && p[n - 2] == 0xFF &&
           p[n - 1] == 0xD9;
}

/* One best-effort thumbnail for any item that has none. Never fails an item.
 *
 * Not the list's thumb_url: those are hqdefault (4:3, letterboxed) and would
 * look squashed in a 16:9 row. mqdefault is 16:9 and small (10-20 KB).
 *
 * Entered with the lock held: returns 0 with it still held when there was
 * nothing to do, else 1 with it released. */
static int thumb_job(DlCore *c) {
    if (c->hold > 0 || !c->ops.net_connected(c->ops.user)) return 0;
    uint64_t now = now_ms(c);
    if (now < c->thumb_next_ms) return 0;
    DlCoreItem *it = NULL;
    for (int i = 0; i < c->count; i++) {
        DlCoreItem *t = &c->items[i];
        if (!t->pending_delete && !t->thumb_ok &&
            t->thumb_tries < DL_THUMB_TRIES) {
            it = t;
            break;
        }
    }
    if (!it) return 0;
    uint32_t uid = it->uid;
    char id[YT_VIDEO_ID_LEN], url[96];
    memcpy(id, it->pub.video_id, sizeof id);
    snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    it->thumb_tries++;
    c->cancel = 0;
    memset(&c->x, 0, sizeof c->x);
    c->x.uid = uid;
    c->x.first = -1;
    c->x.expect_len = -1;
    c->x.fd = -1;
    UNLOCK(c);

    DlSink sink = { c, thumb_headers, thumb_data };
    int ret = c->ops.net_fetch(c->ops.user, url, -1, -1, &sink, &c->cancel);
    int ok = 0;
    char jpg[PATH_CAP], tmp[PATH_CAP];
    if (ret == DL_NET_OK && c->x.accepted &&
        (c->x.expect_len < 0 || (int64_t)c->x.wlen == c->x.expect_len) &&
        jpeg_complete(c->wbuf, (int64_t)c->x.wlen) &&
        path_of(jpg, sizeof jpg, id, "jpg") == 0 &&
        path_of(tmp, sizeof tmp, id, "jpg.tmp") == 0) {
        /* Complete or nothing: a half-written .jpg would be offered to the
         * UI as a thumbnail. */
        if (write_whole(c, tmp, (const char *)c->wbuf, c->x.wlen) == 0) {
            c->ops.fs_unlink(c->ops.user, jpg);
            ok = c->ops.fs_rename(c->ops.user, tmp, jpg) == 0;
            if (!ok) c->ops.fs_unlink(c->ops.user, tmp);
        }
    }

    LOCK(c);
    it = find_uid(c, uid);
    if (it) {
        if (ok) {
            it->thumb_ok = 1;
            c->index_dirty = 1;
            bump(c);
        } else if (ret == DL_NET_TRANSPORT || ret == DL_NET_CANCELLED ||
                   ret == DL_NET_ABORTED) {
            /* Not the thumbnail's fault: give the attempt back. */
            if (it->thumb_tries > 0 && c->x.verdict == DL_XV_NONE)
                it->thumb_tries--;
        }
    }
    if (ok) {
        c->thumb_streak = 0;
    } else {
        /* Not on every step: 5 s, then 10, 20, 40, and 80 s at most. A
         * thumbnail is never worth a request a second while the link is
         * connected but dead. */
        if (c->thumb_streak < 5) c->thumb_streak++;
        c->thumb_next_ms = now_ms(c) + (5000u << (c->thumb_streak - 1));
    }
    core_log(c, "dl %s: thumbnail %s (ret=%d status=%d)", id,
             ok ? "saved" : "not saved", ret, c->x.status);
    UNLOCK(c);
    return 1;
}

/* --- units ------------------------------------------------------------------ */

static void begin_run(DlCore *c, DlCoreItem *it) {
    it->fail_count = 0;
    it->url_fails = 0;
    it->resolve_fails = 0;
    it->restarts = 0;
    it->pub.error = DL_ERR_NONE;
    /* A run always starts with a fresh resolve, even straight after a pause:
     * it is what proves the stream is still the same file, and the one place
     * the free-space check (size + reserve) is made before a byte is
     * fetched. */
    c->url_valid = 0;
    c->since_check = 0;
    if (it->pub.bytes_total > 0 && it->pub.bytes_done == it->pub.bytes_total &&
        !it->wipe)
        set_state(c, it, DL_VERIFYING); /* everything is here already */
    else
        set_state(c, it, DL_RESOLVING);
}

static int unit_wait(DlCore *c, DlCoreItem *it) {
    uint64_t now = now_ms(c);
    int connected = c->ops.net_connected(c->ops.user);
    unsigned gen = c->ops.net_generation(c->ops.user);
    if (c->hold > 0) {
        if (it->wait_kind != DL_WAIT_HOLD || it->pub.retry_in_s != -1) {
            it->wait_kind = DL_WAIT_HOLD;
            it->pub.retry_in_s = -1;
            bump(c);
        }
        UNLOCK(c);
        return DL_POLL_MS;
    }
    int go = 0;
    if (it->force_try) {
        go = 1;
    } else if (it->wait_kind == DL_WAIT_HOLD) {
        go = 1; /* the playback is over */
    } else if (it->wait_kind == DL_WAIT_OFFLINE) {
        go = connected || gen != it->wait_gen;
    } else {
        if (!connected) {
            it->wait_kind = DL_WAIT_OFFLINE;
            it->wait_gen = gen;
            it->fail_count = 0;
            it->pub.retry_in_s = -1;
            bump(c);
            UNLOCK(c);
            return DL_POLL_MS;
        }
        if (gen != it->wait_gen) {
            /* The network came back or the console woke: no reason to sit
             * out the rest of the backoff, and the failures before it say
             * nothing about it. */
            it->fail_count = 0;
            go = 1;
        } else if (now >= it->next_try_ms) {
            go = 1;
        } else {
            int left = (int)((it->next_try_ms - now + 999) / 1000);
            if (left != it->pub.retry_in_s) {
                it->pub.retry_in_s = left;
                bump(c);
            }
            uint64_t rest = it->next_try_ms - now;
            UNLOCK(c);
            return rest < DL_POLL_MS ? (int)rest : DL_POLL_MS;
        }
    }
    if (go) {
        set_state(c, it, c->url_valid ? DL_DOWNLOADING : DL_RESOLVING);
        UNLOCK(c);
        return 0;
    }
    UNLOCK(c);
    return DL_POLL_MS;
}

/* Which of the freshly resolved formats continues what is on the card: the
 * same itag, else the same height; a first resolve takes the best one
 * (formats[] is sorted by height, descending). *exact is 0 when neither
 * matched. */
static int choose_format(const yt_stream_t *s, int itag, int height,
                         int *exact) {
    *exact = 1;
    if (!itag && !height) return 0;
    if (itag)
        for (int i = 0; i < s->format_count; i++)
            if (s->formats[i].itag == itag) return i;
    if (height)
        for (int i = 0; i < s->format_count; i++)
            if (s->formats[i].height == height) return i;
    *exact = 0;
    return 0;
}

static int truncate_part(DlCore *c, const char *part) {
    int fd = c->ops.fs_open(c->ops.user, part, DL_FS_WRITE | DL_FS_TRUNC);
    if (fd < 0) return -1;
    c->ops.fs_close(c->ops.user, fd);
    return 0;
}

static int unit_resolve(DlCore *c, DlCoreItem *it) {
    if (gate(c, it)) {
        UNLOCK(c);
        return DL_POLL_MS;
    }
    const uint32_t uid = it->uid;
    char id[YT_VIDEO_ID_LEN];
    memcpy(id, it->pub.video_id, sizeof id);
    const int wipe = it->wipe;
    const int64_t total0 = it->pub.bytes_total;
    const unsigned gen0 = c->ops.net_generation(c->ops.user);
    c->cancel = 0;
    UNLOCK(c);

    /* What is on the card is the truth about how far the download got. */
    char part[PATH_CAP], mp4[PATH_CAP];
    int64_t psize = 0;
    if (path_of(part, sizeof part, id, "part") == 0 &&
        path_of(mp4, sizeof mp4, id, "mp4") == 0) {
        if (wipe) {
            c->ops.fs_unlink(c->ops.user, part);
            c->ops.fs_unlink(c->ops.user, mp4);
        } else {
            psize = c->ops.fs_size(c->ops.user, part);
            if (psize < 0) psize = 0;
            if (total0 > 0 && psize > total0) {
                c->ops.fs_unlink(c->ops.user, part);
                psize = 0;
            }
        }
    }

    LOCK(c);
    it = find_uid(c, uid);
    if (!it || it->pending_delete || it->pub.state != DL_RESOLVING) {
        UNLOCK(c);
        return 0;
    }
    it->wipe = 0;
    if (it->pub.bytes_done != psize) {
        it->pub.bytes_done = psize;
        bump(c);
    }
    UNLOCK(c);

    int r = c->ops.net_resolve(c->ops.user, id, &c->cancel, &c->stream);

    LOCK(c);
    it = find_uid(c, uid);
    if (!it || it->pending_delete || it->pub.state != DL_RESOLVING) {
        UNLOCK(c);
        return 0;
    }
    if (r == DL_NET_CANCELLED) {
        UNLOCK(c);
        return 0; /* the state says why (paused, held, shutting down) */
    }
    if (r == DL_NET_TRANSPORT) {
        note_failure(c, it, 0, gen0, "resolve");
        UNLOCK(c);
        return 0;
    }
    if (r != DL_NET_OK) {
        /* YouTube answered, with an error. Probably a bad moment (its
         * reputation gate flips for tens of seconds under load): a few
         * patient tries, then the user is told the URL cannot be renewed. */
        it->resolve_fails++;
        core_log(c, "dl %s: resolve refused (%d of %d)", id, it->resolve_fails,
                 DL_RESOLVE_FAIL_LIMIT);
        if (it->resolve_fails >= DL_RESOLVE_FAIL_LIMIT)
            fail_item(c, it, DL_ERR_URL);
        else
            enter_wait(c, it, DL_WAIT_TIMER, backoff_ms(it->resolve_fails));
        UNLOCK(c);
        return 0;
    }

    const yt_stream_t *s = &c->stream;
    if (s->is_live || s->format_count <= 0 ||
        strcmp(s->playability, "OK") != 0) {
        core_log(c, "dl %s: not downloadable (live=%d formats=%d status=%s)",
                 id, s->is_live, s->format_count, s->playability);
        fail_item(c, it, DL_ERR_NOT_DOWNLOADABLE);
        UNLOCK(c);
        return 0;
    }

    int exact = 1;
    int idx = choose_format(s, it->itag, it->pub.height, &exact);
    const yt_format_t *f = &s->formats[idx];
    int restart = 0;
    if (it->pub.bytes_done > 0) {
        /* Continue only if this is provably the same file. */
        if (!exact) restart = 1;
        else if (it->pub.bytes_total <= 0) restart = 1;
        else if (f->content_length > 0 &&
                 f->content_length != it->pub.bytes_total)
            restart = 1;
        if (restart)
            core_log(c, "dl %s: renewed stream differs, starting over", id);
    }
    if (restart) {
        it->pub.bytes_done = 0;
        it->pub.bytes_total = 0;
    }
    it->itag = f->itag;
    it->pub.height = f->height;
    if (f->content_length > 0) it->pub.bytes_total = f->content_length;
    snprintf(c->url, sizeof c->url, "%s", f->url);
    c->url_valid = 1;
    c->url_uid = uid;
    c->url_ms = now_ms(c);
    it->resolve_fails = 0;
    c->index_dirty = 1;
    const int64_t total = it->pub.bytes_total;
    const int64_t done = it->pub.bytes_done;
    UNLOCK(c);

    if (restart && truncate_part(c, part) < 0) {
        LOCK(c);
        it = find_uid(c, uid);
        if (it && !it->pending_delete) fail_item(c, it, DL_ERR_IO);
        UNLOCK(c);
        return 0;
    }
    int no_space = 0;
    if (total > 0) {
        /* The size is known: say now, before a byte is fetched, whether it
         * will fit. The .part (if any) stays, so freeing space and resuming
         * continues where this stopped. */
        int64_t card_total = 0, card_free = 0;
        if (c->ops.fs_free(c->ops.user, &card_total, &card_free) == 0 &&
            (total - done) + DL_RESERVE_BYTES > card_free)
            no_space = 1;
    }
    LOCK(c);
    it = find_uid(c, uid);
    if (it && !it->pending_delete && it->pub.state == DL_RESOLVING) {
        if (no_space)
            fail_item(c, it, DL_ERR_NO_SPACE);
        else
            set_state(c, it, DL_DOWNLOADING);
    }
    UNLOCK(c);
    return 0;
}

/* Called with the lock held after a request ended; returns with it released. */
static int chunk_finish(DlCore *c, int ret) {
    DlXfer *x = &c->x;
    LOCK(c);
    DlCoreItem *it = find_uid(c, x->uid);
    if (c->pending_bump) {
        bump(c);
        c->pending_bump = 0;
        c->bump_ms = now_ms(c);
    }
    if (!it || it->pending_delete || it->pub.state != DL_DOWNLOADING ||
        c->stopping) {
        UNLOCK(c);
        return 0;
    }
    const int64_t progress = x->file_off - x->first;
    const unsigned gen0 = x->gen0;
    switch (x->verdict) {
    case DL_XV_DISK_FULL:
    case DL_XV_SPACE_LOW:
        fail_item(c, it, DL_ERR_DISK_FULL);
        break;
    case DL_XV_IO:
        fail_item(c, it, DL_ERR_IO);
        break;
    case DL_XV_NO_SPACE:
        fail_item(c, it, DL_ERR_NO_SPACE);
        break;
    case DL_XV_RESTART:
        if (++it->restarts > DL_RESTART_LIMIT) {
            fail_item(c, it, DL_ERR_NETWORK);
        } else {
            /* Discard what is stored (the next resolve unit removes it) and
             * start over; the new length, if the server told, is already in
             * bytes_total. */
            core_log(c, "dl %s: starting over from byte 0 (%d)",
                     it->pub.video_id, it->restarts);
            it->wipe = 1;
            it->pub.bytes_done = 0;
            set_state(c, it, DL_RESOLVING);
        }
        break;
    case DL_XV_URL:
        c->url_valid = 0;
        it->url_fails++;
        core_log(c, "dl %s: HTTP %d, renewing the URL (%d of %d)",
                 it->pub.video_id, x->status, it->url_fails, DL_URL_FAIL_LIMIT);
        if (it->url_fails >= DL_URL_FAIL_LIMIT)
            fail_item(c, it, DL_ERR_URL);
        else
            set_state(c, it, DL_RESOLVING);
        break;
    case DL_XV_BAD:
    case DL_XV_HTTP:
    case DL_XV_OVERRUN:
        note_failure(c, it, progress, gen0,
                     x->verdict == DL_XV_BAD ? "bad response" : "bad status");
        break;
    default:
        if (ret == DL_NET_CANCELLED ||
            (ret == DL_NET_ABORTED && c->cancel)) {
            break; /* paused, deleted, held or shutting down: state decides */
        }
        if (ret == DL_NET_OK && x->accepted && x->got == x->expect_len) {
            it->fail_count = 0;
            it->url_fails = 0;
        } else {
            if (progress > 0) it->url_fails = 0;
            note_failure(c, it, progress, gen0, "transfer");
        }
        break;
    }
    UNLOCK(c);
    return 0;
}

static int unit_chunk(DlCore *c, DlCoreItem *it) {
    uint64_t now = now_ms(c);
    if (!c->url_valid || c->url_uid != it->uid ||
        now - c->url_ms >= DL_URL_MAX_AGE_MS) {
        /* Never resolved, or old enough that googlevideo may refuse it. */
        c->url_valid = 0;
        set_state(c, it, DL_RESOLVING);
        UNLOCK(c);
        return 0;
    }
    if (gate(c, it)) {
        UNLOCK(c);
        return DL_POLL_MS;
    }
    const int64_t total = it->pub.bytes_total, done = it->pub.bytes_done;
    if (total > 0 && done == total) {
        set_state(c, it, DL_VERIFYING);
        UNLOCK(c);
        return 0;
    }
    if (total > 0 && done > total) {
        it->wipe = 1;
        it->pub.bytes_done = 0;
        set_state(c, it, DL_RESOLVING);
        UNLOCK(c);
        return 0;
    }
    int64_t last = done + DL_CHUNK_BYTES - 1;
    if (total > 0 && last > total - 1) last = total - 1;
    char id[YT_VIDEO_ID_LEN];
    memcpy(id, it->pub.video_id, sizeof id);
    c->cancel = 0;
    memset(&c->x, 0, sizeof c->x);
    c->x.uid = it->uid;
    c->x.first = done;
    c->x.last = last;
    c->x.file_off = done;
    c->x.fd = -1;
    c->x.gen0 = c->ops.net_generation(c->ops.user);
    c->x.flush_ms = now;
    UNLOCK(c);

    char part[PATH_CAP];
    if (path_of(part, sizeof part, id, "part") < 0) {
        LOCK(c);
        it = find_uid(c, c->x.uid);
        if (it) fail_item(c, it, DL_ERR_IO);
        UNLOCK(c);
        return 0;
    }
    /* The bookkeeping and the card must agree on where this chunk starts;
     * a gap would be a hole of zeros in the middle of the video. */
    int64_t psize = c->ops.fs_size(c->ops.user, part);
    if (psize < 0) psize = 0;
    if (psize != done) {
        LOCK(c);
        it = find_uid(c, c->x.uid);
        if (it && !it->pending_delete && it->pub.state == DL_DOWNLOADING) {
            core_log(c, "dl %s: part file is %lld bytes, expected %lld",
                     id, (long long)psize, (long long)done);
            if (total > 0 && psize > total) {
                it->wipe = 1;
                it->pub.bytes_done = 0;
                set_state(c, it, DL_RESOLVING);
            } else {
                it->pub.bytes_done = psize;
                bump(c);
            }
        }
        UNLOCK(c);
        return 0;
    }
    int fd = c->ops.fs_open(c->ops.user, part, DL_FS_WRITE);
    if (fd < 0) {
        LOCK(c);
        it = find_uid(c, c->x.uid);
        if (it && !it->pending_delete) fail_item(c, it, DL_ERR_IO);
        UNLOCK(c);
        return 0;
    }
    c->x.fd = fd;

    DlSink sink = { c, sink_headers, sink_data };
    int ret = c->ops.net_fetch(c->ops.user, c->url, done, last, &sink,
                               &c->cancel);
    xfer_flush(c); /* whatever arrived before the end is kept */
    c->ops.fs_close(c->ops.user, fd);
    c->x.fd = -1;
    return chunk_finish(c, ret);
}

typedef struct { DlCore *c; int fd; } VerifyCtx;

static int64_t verify_read(void *ctx, int64_t off, void *buf, size_t len) {
    VerifyCtx *v = ctx;
    return v->c->ops.fs_pread(v->c->ops.user, v->fd, buf, len, off);
}

static int unit_verify(DlCore *c, DlCoreItem *it) {
    const uint32_t uid = it->uid;
    char id[YT_VIDEO_ID_LEN];
    memcpy(id, it->pub.video_id, sizeof id);
    const int64_t total = it->pub.bytes_total;
    UNLOCK(c);

    char part[PATH_CAP], mp4[PATH_CAP];
    int bad = 0, io = 0;
    int rc = DL_MP4_OK;
    if (path_of(part, sizeof part, id, "part") < 0 ||
        path_of(mp4, sizeof mp4, id, "mp4") < 0) {
        io = 1;
    } else {
        int64_t size = c->ops.fs_size(c->ops.user, part);
        if (size != total) {
            bad = 1;
            rc = DL_MP4_TRUNCATED;
        } else {
            VerifyCtx v = { c, c->ops.fs_open(c->ops.user, part, DL_FS_READ) };
            if (v.fd < 0) {
                io = 1;
            } else {
                rc = dl_mp4_verify(verify_read, &v, size);
                c->ops.fs_close(c->ops.user, v.fd);
                bad = rc != DL_MP4_OK && rc != DL_MP4_READ;
                io = rc == DL_MP4_READ;
            }
        }
        if (bad) {
            /* Nothing in it can be trusted to continue from. */
            c->ops.fs_unlink(c->ops.user, part);
        } else if (!io) {
            if (c->ops.fs_sync) {
                int fd = c->ops.fs_open(c->ops.user, part, DL_FS_WRITE);
                if (fd >= 0) {
                    c->ops.fs_sync(c->ops.user, fd);
                    c->ops.fs_close(c->ops.user, fd);
                }
            }
            /* The final name appears only now, complete and checked. */
            c->ops.fs_unlink(c->ops.user, mp4);
            if (c->ops.fs_rename(c->ops.user, part, mp4) < 0) io = 1;
        }
    }

    LOCK(c);
    it = find_uid(c, uid);
    if (it && !it->pending_delete && it->pub.state == DL_VERIFYING) {
        if (bad) {
            it->pub.bytes_done = 0;
            core_log(c, "dl %s: verification failed (%d)", id, rc);
            fail_item(c, it, DL_ERR_VERIFY);
        } else if (io) {
            fail_item(c, it, DL_ERR_IO);
        } else {
            it->pub.bytes_done = total;
            it->pub.error = DL_ERR_NONE;
            set_state(c, it, DL_DONE);
            c->index_dirty = 1;
            c->st_valid = 0;
            if (c->cur_uid == uid) c->cur_uid = 0;
            core_log(c, "dl %s: done, %lld bytes", id, (long long)total);
        }
    }
    UNLOCK(c);
    return 0;
}

static DlCoreItem *pick_next(DlCore *c) {
    DlCoreItem *best = NULL;
    for (int i = 0; i < c->count; i++) {
        DlCoreItem *it = &c->items[i];
        if (it->pending_delete || it->pub.state != DL_QUEUED) continue;
        if (!best || it->qseq < best->qseq) best = it;
    }
    return best;
}

static int step_inner(DlCore *c) {
    if (reap_one(c)) return 0;
    index_maintain(c, 0);

    LOCK(c);
    if (c->stopping || !c->running) {
        UNLOCK(c);
        return DL_IDLE_MS;
    }
    DlCoreItem *it = c->cur_uid ? find_uid(c, c->cur_uid) : NULL;
    if (it && !slot_active(it)) {
        c->cur_uid = 0; /* paused, failed, done: the slot is free */
        it = NULL;
    }
    if (!it) {
        it = pick_next(c);
        if (it) {
            c->cur_uid = it->uid;
            begin_run(c, it);
        }
    }
    if (!it) {
        if (thumb_job(c)) return 0;
        UNLOCK(c);
        return DL_IDLE_MS;
    }
    switch (it->pub.state) {
    case DL_QUEUED:
        begin_run(c, it); /* resumed while it still held the slot */
        UNLOCK(c);
        return 0;
    case DL_RESOLVING:
        return unit_resolve(c, it);
    case DL_DOWNLOADING:
        if (thumb_job(c)) return 0;
        return unit_chunk(c, it);
    case DL_WAITING:
        return unit_wait(c, it);
    case DL_VERIFYING:
        return unit_verify(c, it);
    default:
        c->cur_uid = 0;
        UNLOCK(c);
        return 0;
    }
}

int dl_core_step(DlCore *c) {
    int r = step_inner(c);
    drain_log(c);
    return r;
}

/* --- commands --------------------------------------------------------------- */

DlEnqueue dl_core_enqueue(DlCore *c, const yt_video_t *v) {
    if (!v || !dl_valid_id(v->video_id)) return DL_ENQ_BAD;
    LOCK(c);
    if (!c->running || c->stopping) {
        UNLOCK(c);
        return DL_ENQ_BAD;
    }
    if (find_live(c, v->video_id)) {
        UNLOCK(c);
        return DL_ENQ_EXISTS;
    }
    UNLOCK(c);

    /* Refuse early, with the card in front of us: starting a download that
     * is certain to fail only to say so a minute later is worse. */
    int64_t card_total = 0, card_free = 0;
    if (c->ops.fs_free(c->ops.user, &card_total, &card_free) == 0 &&
        card_free < DL_RESERVE_BYTES)
        return DL_ENQ_FULL;

    LOCK(c);
    if (find_live(c, v->video_id)) {
        UNLOCK(c);
        return DL_ENQ_EXISTS;
    }
    if (c->count >= DL_MAX_ITEMS) {
        UNLOCK(c);
        return DL_ENQ_FULL;
    }
    DlCoreItem *it = &c->items[c->count++];
    memset(it, 0, sizeof *it);
    memcpy(it->pub.video_id, v->video_id, YT_VIDEO_ID_LEN);
    dl_clean_copy(it->pub.title, sizeof it->pub.title, v->title);
    if (!it->pub.title[0])
        memcpy(it->pub.title, v->video_id, YT_VIDEO_ID_LEN);
    dl_clean_copy(it->pub.channel, sizeof it->pub.channel, v->channel);
    dl_clean_copy(it->pub.duration, sizeof it->pub.duration, v->duration);
    dl_clean_copy(it->thumb_url, sizeof it->thumb_url, v->thumb_url);
    it->pub.state = DL_QUEUED;
    it->uid = ++c->next_uid;
    it->qseq = ++c->next_qseq;
    c->index_dirty = 1;
    bump(c);
    core_log(c, "dl %s: queued", it->pub.video_id);
    UNLOCK(c);
    return DL_ENQ_OK;
}

static void pause_item(DlCore *c, DlCoreItem *it) {
    switch (it->pub.state) {
    case DL_QUEUED: case DL_RESOLVING: case DL_DOWNLOADING: case DL_WAITING:
        set_state(c, it, DL_PAUSED);
        if (c->cur_uid == it->uid) c->cancel = 1; /* stop the request now */
        core_log(c, "dl %s: paused at %lld", it->pub.video_id,
                 (long long)it->pub.bytes_done);
        break;
    default:
        break; /* verifying finishes in moments; the rest has nothing to pause */
    }
}

void dl_core_pause(DlCore *c, const char *id) {
    if (!dl_valid_id(id)) return;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    if (it) pause_item(c, it);
    UNLOCK(c);
}

static void queue_item(DlCore *c, DlCoreItem *it) {
    it->pub.error = DL_ERR_NONE;
    it->qseq = ++c->next_qseq;
    it->fail_count = 0;
    it->force_try = 0;
    set_state(c, it, DL_QUEUED);
}

static void resume_item(DlCore *c, DlCoreItem *it) {
    switch (it->pub.state) {
    case DL_PAUSED:
        queue_item(c, it);
        break;
    case DL_FAILED:
        /* A verification failure left nothing worth keeping; any other
         * failure keeps the .part and continues from it. */
        if (it->pub.error == DL_ERR_VERIFY) it->wipe = 1;
        queue_item(c, it);
        break;
    case DL_WAITING:
        /* "Try now" — but not while an online video is playing, and no use
         * while there is no network. */
        if (it->wait_kind != DL_WAIT_HOLD) {
            it->next_try_ms = now_ms(c);
            it->fail_count = 0;
            it->force_try = 1;
        }
        break;
    default:
        break;
    }
}

void dl_core_resume(DlCore *c, const char *id) {
    if (!dl_valid_id(id)) return;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    if (it) resume_item(c, it);
    UNLOCK(c);
}

void dl_core_pause_all(DlCore *c) {
    LOCK(c);
    for (int i = 0; i < c->count; i++)
        if (!c->items[i].pending_delete) pause_item(c, &c->items[i]);
    UNLOCK(c);
}

void dl_core_resume_all(DlCore *c) {
    LOCK(c);
    /* In list order, so the queue keeps the order they were added in. */
    for (int i = 0; i < c->count; i++)
        if (!c->items[i].pending_delete &&
            c->items[i].pub.state == DL_PAUSED)
            resume_item(c, &c->items[i]);
    UNLOCK(c);
}

void dl_core_delete(DlCore *c, const char *id) {
    if (!dl_valid_id(id)) return;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    if (it) {
        it->pending_delete = 1;
        if (c->cur_uid == it->uid) c->cancel = 1;
        c->index_dirty = 1;
        bump(c);
    }
    UNLOCK(c);
}

/* --- queries ---------------------------------------------------------------- */

int dl_core_count(DlCore *c) {
    LOCK(c);
    int n = 0;
    for (int i = 0; i < c->count; i++)
        if (!c->items[i].pending_delete) n++;
    UNLOCK(c);
    return n;
}

int dl_core_snapshot(DlCore *c, DlItem *out, int max) {
    if (!out || max <= 0) return 0;
    LOCK(c);
    int n = 0;
    for (int i = 0; i < c->count && n < max; i++)
        if (!c->items[i].pending_delete) out[n++] = c->items[i].pub;
    UNLOCK(c);
    return n;
}

int dl_core_get(DlCore *c, const char *id, DlItem *out) {
    if (!dl_valid_id(id)) return 0;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    if (it && out) *out = it->pub;
    UNLOCK(c);
    return it != NULL;
}

int dl_core_active_count(DlCore *c) {
    LOCK(c);
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        const DlCoreItem *it = &c->items[i];
        if (it->pending_delete) continue;
        switch (it->pub.state) {
        case DL_QUEUED: case DL_RESOLVING: case DL_DOWNLOADING:
        case DL_WAITING: case DL_VERIFYING:
            n++;
            break;
        default:
            break;
        }
    }
    UNLOCK(c);
    return n;
}

unsigned dl_core_revision(DlCore *c) {
    LOCK(c);
    unsigned r = c->revision;
    UNLOCK(c);
    return r;
}

int dl_core_storage(DlCore *c, DlStorage *out) {
    memset(out, 0, sizeof *out);
    uint64_t now = now_ms(c);
    LOCK(c);
    int fresh = c->st_valid && now - c->st_ms < DL_STORAGE_CACHE_MS;
    int64_t total = c->st_total, free_bytes = c->st_free;
    UNLOCK(c);
    if (!fresh) {
        /* A frame loop may ask every frame; the card is asked about once a
         * second. (dl_core_enqueue always asks for itself.) */
        if (c->ops.fs_free(c->ops.user, &total, &free_bytes) != 0) {
            LOCK(c);
            c->st_valid = 0;
            UNLOCK(c);
            return -1;
        }
        LOCK(c);
        c->st_valid = 1;
        c->st_total = total;
        c->st_free = free_bytes;
        c->st_ms = now;
        UNLOCK(c);
    }
    LOCK(c);
    int64_t used = 0;
    for (int i = 0; i < c->count; i++) {
        const DlCoreItem *it = &c->items[i];
        if (!it->pending_delete) used += it->pub.bytes_done;
    }
    UNLOCK(c);
    out->total_bytes = total;
    out->free_bytes = free_bytes;
    out->downloads_bytes = used;
    out->low = free_bytes < DL_RESERVE_BYTES;
    return 0;
}

int dl_core_local_path(DlCore *c, const char *id, char *path, size_t cap) {
    if (!dl_valid_id(id) || !path || cap == 0) return 0;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    int64_t total = (it && it->pub.state == DL_DONE) ? it->pub.bytes_total : -1;
    UNLOCK(c);
    if (total < 0) return 0;
    char mp4[PATH_CAP];
    if (path_of(mp4, sizeof mp4, id, "mp4") < 0 || strlen(mp4) >= cap) return 0;
    int64_t size = c->ops.fs_size(c->ops.user, mp4);
    if (size == total) {
        memcpy(path, mp4, strlen(mp4) + 1);
        return 1;
    }
    /* Deleted from outside, or replaced: the entry no longer describes a
     * file that exists. dl_resume() fetches it again. */
    LOCK(c);
    it = find_live(c, id);
    if (it && it->pub.state == DL_DONE) {
        it->pub.bytes_done = 0;
        it->wipe = 1;
        fail_item(c, it, DL_ERR_IO);
    }
    UNLOCK(c);
    return 0;
}

int dl_core_thumb_path(DlCore *c, const char *id, char *path, size_t cap) {
    if (!dl_valid_id(id) || !path || cap == 0) return 0;
    LOCK(c);
    int known = find_live(c, id) != NULL;
    UNLOCK(c);
    if (!known) return 0;
    char jpg[PATH_CAP];
    if (path_of(jpg, sizeof jpg, id, "jpg") < 0 || strlen(jpg) >= cap) return 0;
    if (c->ops.fs_size(c->ops.user, jpg) <= 0) return 0;
    memcpy(path, jpg, strlen(jpg) + 1);
    return 1;
}

void dl_core_report_bad_file(DlCore *c, const char *id) {
    if (!dl_valid_id(id)) return;
    LOCK(c);
    DlCoreItem *it = find_live(c, id);
    if (it && it->pub.state == DL_DONE) {
        it->pub.bytes_done = 0;
        it->wipe = 1;
        fail_item(c, it, DL_ERR_VERIFY);
    }
    UNLOCK(c);
}

void dl_core_playback_begin(DlCore *c) {
    LOCK(c);
    c->hold++;
    /* Stop what is in flight now; the next unit sees the hold and waits. */
    if (c->cur_uid) c->cancel = 1;
    UNLOCK(c);
}

void dl_core_playback_end(DlCore *c) {
    LOCK(c);
    if (c->hold > 0) c->hold--;
    UNLOCK(c);
}
