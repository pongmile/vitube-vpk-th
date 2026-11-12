/* Host-side tests for the offline-playback file stream (src/media/local_stream.c).
 *
 * The stream sits between the hardware decoder's demuxer and a file on the
 * memory card, and what goes wrong there is invisible until someone plays a
 * download on a console: a read that straddles the read-ahead window and comes
 * back with the wrong bytes, a seek relative to the end that is off by the
 * window size, a position that wraps at 2 GiB, a failed read that leaves the
 * descriptor somewhere the bookkeeping does not know about. The module takes
 * its file operations as a table, so here the "card" is a virtual file whose
 * bytes are a function of the offset (a 3 GB file costs nothing) with faults
 * that can be switched on, and the table also watches the lock rule the real
 * one has to obey: every file call under the I/O lock, the lock held for that
 * one call and for no longer.
 *
 *   gcc -Isrc -Wall -Wextra -Werror -o /tmp/tls tests/test_local_stream.c \
 *       src/media/local_stream.c && /tmp/tls
 */
#include "media/local_stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

#define WINDOW VT_LOCAL_STREAM_READAHEAD
#define LL(x) ((long long)(x))

/* --- the virtual card ------------------------------------------------------- */

/* Pseudo-random but a pure function of the offset, so any slice of any file
 * can be checked without keeping the file. */
static unsigned char pat(int64_t offset) {
    uint64_t x = (uint64_t)offset * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    return (unsigned char)(x >> 56);
}

#define MAX_FDS 8

typedef struct {
    int64_t size;          /* of the virtual file */
    int max_chunk;         /* >0: a read returns at most this many bytes */
    int fail_open;         /* <0: open returns this */
    int fail_seek_end;     /* <0: the next END seek returns this */
    int fail_seek_set_in;  /* N>0: the Nth SET seek from now fails */
    int fail_read_in;      /* N>0: the Nth read from now fails with fail_read_code */
    int fail_read_code;
    int short_file;        /* >0: reads hit end of file at this offset although
                            * the length said more (the file shrank) */

    int opens, closes, reads, seeks, seeks_set, seeks_end;
    int max_read_request;
    int64_t fd_pos[MAX_FDS];
    int fd_open[MAX_FDS];

    /* The lock rule. */
    int lock_depth, max_lock_depth, lock_calls, unlock_calls;
    int calls_in_hold, max_calls_in_hold, ops_outside_lock;
} Card;

static Card card;

static void enter_op(void) {
    if (card.lock_depth != 1) card.ops_outside_lock++;
    card.calls_in_hold++;
    if (card.calls_in_hold > card.max_calls_in_hold)
        card.max_calls_in_hold = card.calls_in_hold;
}

static void m_lock(void) {
    card.lock_calls++;
    card.lock_depth++;
    if (card.lock_depth > card.max_lock_depth)
        card.max_lock_depth = card.lock_depth;
    card.calls_in_hold = 0;
}

static void m_unlock(void) {
    card.unlock_calls++;
    card.lock_depth--;
}

static int m_open(void *ctx, const char *path) {
    (void)ctx; (void)path;
    enter_op();
    if (card.fail_open < 0) return card.fail_open;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (card.fd_open[fd]) continue;
        card.fd_open[fd] = 1;
        card.fd_pos[fd] = 0;
        card.opens++;
        return fd;
    }
    return -24; /* out of descriptors */
}

static int m_read(void *ctx, int fd, void *buffer, size_t size) {
    (void)ctx;
    enter_op();
    card.reads++;
    if ((int)size > card.max_read_request) card.max_read_request = (int)size;
    if (fd < 0 || fd >= MAX_FDS || !card.fd_open[fd]) return -9;
    if (card.fail_read_in > 0 && --card.fail_read_in == 0)
        return card.fail_read_code;
    int64_t end = card.short_file > 0 ? card.short_file : card.size;
    int64_t at = card.fd_pos[fd];
    if (at >= end) return 0;
    int64_t take = (int64_t)size;
    if (card.max_chunk > 0 && take > card.max_chunk) take = card.max_chunk;
    if (take > end - at) take = end - at;
    unsigned char *out = buffer;
    for (int64_t i = 0; i < take; i++) out[i] = pat(at + i);
    card.fd_pos[fd] = at + take;
    return (int)take;
}

static int64_t m_seek(void *ctx, int fd, int64_t offset, int whence) {
    (void)ctx;
    enter_op();
    card.seeks++;
    if (fd < 0 || fd >= MAX_FDS || !card.fd_open[fd]) return -9;
    int64_t target;
    if (whence == VT_LOCAL_SEEK_SET) {
        card.seeks_set++;
        if (card.fail_seek_set_in > 0 && --card.fail_seek_set_in == 0)
            return -5;
        target = offset;
    } else if (whence == VT_LOCAL_SEEK_END) {
        card.seeks_end++;
        if (card.fail_seek_end < 0) return card.fail_seek_end;
        target = card.size + offset;
    } else {
        return -22; /* the stream must never ask for anything else */
    }
    if (target < 0) return -22;
    card.fd_pos[fd] = target;
    return target;
}

static void m_close(void *ctx, int fd) {
    (void)ctx;
    enter_op();
    if (fd < 0 || fd >= MAX_FDS || !card.fd_open[fd]) {
        failures++;
        fprintf(stderr, "FAIL close of a descriptor that is not open: %d\n", fd);
        return;
    }
    card.fd_open[fd] = 0;
    card.closes++;
}

static const VtLocalIo io_locked = {
    .ctx = &card, .lock = m_lock, .unlock = m_unlock,
    .open = m_open, .read = m_read, .seek = m_seek, .close = m_close,
};

static const VtLocalIo io_unlocked = {
    .ctx = &card, .lock = NULL, .unlock = NULL,
    .open = m_open, .read = m_read, .seek = m_seek, .close = m_close,
};

static VtLocalStreamFactory factory;

static void begin(int64_t size) {
    memset(&card, 0, sizeof card);
    card.size = size;
    CHECK(vt_local_stream_factory_init_io(&factory, "ux0:data/ViTube/downloads/x.mp4",
                                          &io_locked) == 0, "factory init");
}

static int open_stream(VtDecoderStreamHandle *h) {
    memset(h, 0x5a, sizeof *h); /* a factory must define every field itself */
    return factory.factory.open(factory.factory.opaque, h);
}

/* After a test: nothing left open, and the lock rule held throughout. */
static void finish(const char *name) {
    CHECK(card.opens == card.closes, "%s: %d opened, %d closed", name,
          card.opens, card.closes);
    CHECK(card.lock_depth == 0, "%s: lock still held (depth %d)", name,
          card.lock_depth);
    CHECK(card.lock_calls == card.unlock_calls, "%s: %d locks, %d unlocks",
          name, card.lock_calls, card.unlock_calls);
    CHECK(card.max_lock_depth <= 1, "%s: lock taken recursively (%d)", name,
          card.max_lock_depth);
    CHECK(card.ops_outside_lock == 0, "%s: %d file calls outside the lock",
          name, card.ops_outside_lock);
    CHECK(card.max_calls_in_hold <= 1,
          "%s: the lock was held across %d file calls", name,
          card.max_calls_in_hold);
    CHECK(card.max_read_request <= WINDOW, "%s: asked the card for %d bytes",
          name, card.max_read_request);
}

/* One read through the handle, checked against the pattern at `*at`, which it
 * advances. Returns what read() returned. */
static int rd(VtDecoderStreamHandle *h, int64_t *at, size_t want) {
    static unsigned char buf[4 * WINDOW];
    if (want > sizeof buf) want = sizeof buf;
    int n = h->read(h->opaque, buf, want);
    if (n > 0) {
        CHECK((size_t)n <= want, "read returned %d for a request of %zu", n, want);
        int bad = 0;
        for (int i = 0; i < n && !bad; i++)
            if (buf[i] != pat(*at + i)) {
                bad = 1;
                CHECK(0, "wrong byte at offset %lld (read of %zu at %lld)",
                      LL(*at + i), want, LL(*at));
            }
        *at += n;
    }
    return n;
}

/* --- tests ------------------------------------------------------------------ */

static void test_open_and_size(void) {
    begin(1000000);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    CHECK(h.size == 1000000, "size %lld", LL(h.size));
    CHECK(h.read && h.seek && h.close && h.opaque, "handle not complete");
    CHECK(h.fetch == NULL && h.url == NULL,
          "a media file is not a playlist: fetch/url must be NULL");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_SIZE) == 1000000,
          "AVSEEK_SIZE query");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_SIZE | VT_LOCAL_SEEK_FORCE) == 1000000,
          "AVSEEK_SIZE with the force bit");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == 0,
          "the size query must not move the position");
    CHECK(card.reads == 0, "open reads nothing: %d", card.reads);
    int64_t at = 0;
    CHECK(rd(&h, &at, 100) == 100, "first read");
    CHECK(card.reads == 1, "one read for the first window: %d", card.reads);
    CHECK(card.seeks == 2, "only open's two seeks (length, rewind): %d", card.seeks);
    CHECK(card.max_read_request == WINDOW, "window asked of the card: %d",
          card.max_read_request);
    h.close(h.opaque);
    finish("open_and_size");
}

static void test_sequential(void) {
    int64_t size = 3 * (int64_t)WINDOW + 1000;
    begin(size);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    static const size_t asks[] = { 65536, 1000, 1, 4096, 77777, 3, 100000 };
    int64_t at = 0;
    for (size_t i = 0; at < size; i++) {
        size_t ask = asks[i % (sizeof asks / sizeof *asks)];
        int n = rd(&h, &at, ask);
        CHECK(n > 0, "unexpected %d at %lld of %lld", n, LL(at), LL(size));
        if (n <= 0) break;
    }
    CHECK(at == size, "read %lld of %lld", LL(at), LL(size));
    CHECK(card.reads == 4, "ceil(size / window) = 4 card reads, got %d", card.reads);
    CHECK(card.seeks == 2, "sequential reading never repositions: %d seeks", card.seeks);
    int before = card.reads;
    CHECK(rd(&h, &at, 10) == 0, "EOF");
    CHECK(rd(&h, &at, 10) == 0, "EOF again");
    CHECK(card.reads == before, "end of file is answered without the card");
    CHECK(h.read(h.opaque, (char[1]){0}, 0) == 0, "zero-length read");
    h.close(h.opaque);
    finish("sequential");
}

static void test_window_boundary(void) {
    begin(4 * (int64_t)WINDOW);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    CHECK(rd(&h, &at, WINDOW - 5) == WINDOW - 5, "up to 5 bytes short of the end of the window");
    CHECK(card.reads == 1, "one card read so far");
    /* A request that straddles the window is answered with what the window
     * holds; the demuxer asks again for the rest. */
    CHECK(rd(&h, &at, 20) == 5, "straddling read is short");
    CHECK(card.reads == 1, "...and still one card read");
    CHECK(rd(&h, &at, 20) == 20, "the rest comes from the next window");
    CHECK(card.reads == 2, "second window");
    CHECK(card.seeks == 2, "the next window needs no repositioning: %d", card.seeks);
    CHECK(at == WINDOW + 20, "position %lld", LL(at));
    /* A request as big as the window, and bigger, never gets more than a window. */
    CHECK(rd(&h, &at, WINDOW) == WINDOW - 20, "rest of the second window");
    CHECK(rd(&h, &at, 3 * WINDOW) == WINDOW, "a huge request returns one window");
    h.close(h.opaque);
    finish("window_boundary");
}

static void test_seek_whence(void) {
    int64_t size = 1000000;
    begin(size);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at;

    CHECK(h.seek(h.opaque, 500, VT_LOCAL_SEEK_SET) == 500, "SET 500");
    at = 500;
    CHECK(rd(&h, &at, 10) == 10, "read after SET");
    CHECK(h.seek(h.opaque, 10, VT_LOCAL_SEEK_CUR) == 520, "CUR +10 from 510");
    at = 520;
    CHECK(rd(&h, &at, 4) == 4, "read after CUR");
    CHECK(h.seek(h.opaque, -24, VT_LOCAL_SEEK_CUR) == 500, "CUR -24 from 524");
    at = 500;
    CHECK(rd(&h, &at, 4) == 4, "read after negative CUR");

    CHECK(h.seek(h.opaque, -100, VT_LOCAL_SEEK_END) == size - 100, "END -100");
    at = size - 100;
    CHECK(rd(&h, &at, 200) == 100, "read to the end is short by exactly the tail");
    CHECK(rd(&h, &at, 200) == 0, "then EOF");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_END) == size, "END 0");
    CHECK(rd(&h, &at, 1) == 0, "nothing at the end");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_SET) == 0, "SET 0");
    at = 0;
    CHECK(rd(&h, &at, 1) == 1, "first byte");

    /* The force bit is stripped, as the decoder does before it calls. */
    CHECK(h.seek(h.opaque, 77, VT_LOCAL_SEEK_SET | VT_LOCAL_SEEK_FORCE) == 77,
          "SET with the force bit");

    /* Refusals leave the position where it was. */
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == 77, "position before refusals");
    CHECK(h.seek(h.opaque, -1, VT_LOCAL_SEEK_SET) < 0, "SET -1");
    CHECK(h.seek(h.opaque, -78, VT_LOCAL_SEEK_CUR) < 0, "CUR before the start");
    CHECK(h.seek(h.opaque, -size - 1, VT_LOCAL_SEEK_END) < 0, "END before the start");
    CHECK(h.seek(h.opaque, 0, 7) < 0, "unknown whence");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == 77,
          "a refused seek must not move the position");
    /* The very first byte before the start is exactly the refused edge. */
    CHECK(h.seek(h.opaque, -77, VT_LOCAL_SEEK_CUR) == 0, "CUR back to exactly 0");
    h.close(h.opaque);
    finish("seek_whence");
}

static void test_seek_costs(void) {
    begin(10 * (int64_t)WINDOW);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    CHECK(rd(&h, &at, 10) == 10, "fill the first window");
    int reads = card.reads, seeks = card.seeks;

    /* Inside the window, in both directions: the card is not touched. */
    CHECK(h.seek(h.opaque, 100000, VT_LOCAL_SEEK_SET) == 100000, "into the window");
    at = 100000;
    CHECK(rd(&h, &at, 50) == 50, "read inside the window");
    CHECK(h.seek(h.opaque, 3, VT_LOCAL_SEEK_SET) == 3, "back inside the window");
    at = 3;
    CHECK(rd(&h, &at, 50) == 50, "read inside the window, behind");
    CHECK(card.reads == reads && card.seeks == seeks,
          "seeks within the window cost no card access (%d reads, %d seeks)",
          card.reads - reads, card.seeks - seeks);

    /* Several seeks and then one read: only the last position is acted on. */
    h.seek(h.opaque, 5 * (int64_t)WINDOW, VT_LOCAL_SEEK_SET);
    h.seek(h.opaque, 7 * (int64_t)WINDOW + 12, VT_LOCAL_SEEK_SET);
    h.seek(h.opaque, 2 * (int64_t)WINDOW + 5, VT_LOCAL_SEEK_SET);
    CHECK(card.reads == reads && card.seeks == seeks, "seeking alone is free");
    at = 2 * (int64_t)WINDOW + 5;
    CHECK(rd(&h, &at, 100) == 100, "read after a far seek");
    CHECK(card.seeks == seeks + 1 && card.reads == reads + 1,
          "one reposition and one read: %d/%d", card.seeks - seeks, card.reads - reads);

    /* Backwards, out of the window: repositions again. */
    h.seek(h.opaque, 1000, VT_LOCAL_SEEK_SET);
    at = 1000;
    CHECK(rd(&h, &at, 100) == 100, "read behind the window");
    CHECK(card.seeks == seeks + 2 && card.reads == reads + 2,
          "backwards out of the window repositions");

    /* Exactly at the end of the window: a refill, but the descriptor is
     * already there, so no reposition. */
    int64_t window_end = 1000 + WINDOW;
    seeks = card.seeks;
    reads = card.reads;
    h.seek(h.opaque, window_end, VT_LOCAL_SEEK_SET);
    at = window_end;
    CHECK(rd(&h, &at, 100) == 100, "read at the end of the window");
    CHECK(card.seeks == seeks && card.reads == reads + 1,
          "adjacent window: refill without reposition (%d seeks, %d reads)",
          card.seeks - seeks, card.reads - reads);
    /* The last byte of the window is still inside it. */
    h.seek(h.opaque, window_end - 1, VT_LOCAL_SEEK_SET);
    at = window_end - 1;
    reads = card.reads;
    CHECK(rd(&h, &at, 1) == 1, "last byte of the previous window");
    CHECK(card.reads == reads + 1, "it is the previous window, now gone");
    h.close(h.opaque);
    finish("seek_costs");
}

static void test_past_the_end(void) {
    int64_t size = 50000;
    begin(size);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    CHECK(h.seek(h.opaque, size + 1000, VT_LOCAL_SEEK_SET) == size + 1000,
          "seeking past the end is allowed, as for any file");
    CHECK(rd(&h, &at, 10) == 0, "and reads EOF");
    CHECK(h.seek(h.opaque, 5LL << 30, VT_LOCAL_SEEK_CUR) == size + 1000 + (5LL << 30),
          "five gigabytes past the end");
    CHECK(rd(&h, &at, 10) == 0, "EOF there too");
    CHECK(card.reads == 0 && card.seeks == 2,
          "none of it reached the card (%d reads, %d seeks)", card.reads, card.seeks);
    CHECK(h.seek(h.opaque, size, VT_LOCAL_SEEK_SET) == size, "exactly the end");
    CHECK(rd(&h, &at, 10) == 0, "EOF at exactly the size");
    CHECK(h.seek(h.opaque, -10, VT_LOCAL_SEEK_END) == size - 10, "back into the file");
    at = size - 10;
    CHECK(rd(&h, &at, 10) == 10, "the last ten bytes");
    CHECK(rd(&h, &at, 10) == 0, "EOF");
    h.close(h.opaque);
    finish("past_the_end");
}

static void test_short_reads(void) {
    int64_t size = 100000;
    begin(size);
    card.max_chunk = 7;
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    int calls = 0;
    while (at < size) {
        int n = rd(&h, &at, 65536);
        calls++;
        CHECK(n > 0 && n <= 7, "short read returned %d", n);
        if (n <= 0) break;
    }
    CHECK(at == size, "got %lld of %lld", LL(at), LL(size));
    CHECK(calls == (size + 6) / 7, "one call per seven bytes: %d", calls);
    CHECK(card.seeks == 2, "short reads never reposition: %d seeks", card.seeks);
    /* Smaller asks than the window holds, then a refill, from a seek. */
    h.seek(h.opaque, 1000, VT_LOCAL_SEEK_SET);
    at = 1000;
    CHECK(rd(&h, &at, 3) == 3, "3 of 7");
    CHECK(rd(&h, &at, 3) == 3, "3 of 7 again");
    CHECK(rd(&h, &at, 3) == 1, "the last byte of the 7");
    CHECK(rd(&h, &at, 3) == 3, "next window");
    h.close(h.opaque);
    finish("short_reads");
}

static void test_read_errors(void) {
    int64_t size = 4 * (int64_t)WINDOW;
    begin(size);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    CHECK(rd(&h, &at, WINDOW) == WINDOW, "first window");

    /* The refill of the second window fails. The demuxer gets the error, the
     * position does not move, and the next read starts over. */
    card.fail_read_in = 1;
    card.fail_read_code = -0x7ffefffe;
    unsigned char buf[16];
    int n = h.read(h.opaque, buf, sizeof buf);
    CHECK(n < 0, "the card's error must reach the decoder, got %d", n);
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == WINDOW,
          "a failed read must not advance the position");
    int seeks = card.seeks;
    CHECK(rd(&h, &at, 16) == 16, "the retry works");
    CHECK(card.seeks == seeks + 1,
          "after an error the descriptor is repositioned, not assumed (%d)",
          card.seeks - seeks);

    /* A failing reposition. */
    h.seek(h.opaque, 3 * (int64_t)WINDOW, VT_LOCAL_SEEK_SET);
    card.fail_seek_set_in = 1;
    CHECK(h.read(h.opaque, buf, sizeof buf) < 0, "failing seek surfaces as an error");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == 3 * (int64_t)WINDOW,
          "position kept after a failed reposition");
    at = 3 * (int64_t)WINDOW;
    CHECK(rd(&h, &at, 16) == 16, "and the retry works");

    /* Error on the very first read of a cursor. */
    VtDecoderStreamHandle h2;
    CHECK(open_stream(&h2) == 0, "second cursor");
    card.fail_read_in = 1;
    card.fail_read_code = -5;
    CHECK(h2.read(h2.opaque, buf, sizeof buf) == -5, "first read fails with the card's code");
    int64_t at2 = 0;
    CHECK(rd(&h2, &at2, 16) == 16, "first read, retried");

    /* A file that turns out shorter than its length said: EOF, not a hang. */
    VtDecoderStreamHandle h3;
    CHECK(open_stream(&h3) == 0, "third cursor");
    card.short_file = 1000;
    h3.seek(h3.opaque, 2000, VT_LOCAL_SEEK_SET);
    CHECK(h3.read(h3.opaque, buf, sizeof buf) == 0, "end of data before the length: EOF");

    h.close(h.opaque);
    h2.close(h2.opaque);
    h3.close(h3.opaque);
    finish("read_errors");
}

static void test_open_errors(void) {
    VtDecoderStreamHandle h;

    begin(1000);
    /* A real sceIo code: SCE_ERROR_ERRNO_ENOENT, as a negative int. */
    const int enoent = (int)0x80010002u;
    card.fail_open = enoent;
    CHECK(open_stream(&h) == enoent, "the open error is passed on");
    CHECK(factory.last_error == enoent, "and remembered: %d", factory.last_error);
    CHECK(card.opens == 0, "nothing opened");
    finish("open_fails");

    begin(1000);
    card.fail_seek_end = -5;
    CHECK(open_stream(&h) < 0, "length unknown");
    CHECK(card.opens == 1 && card.closes == 1, "the descriptor is closed again (%d/%d)",
          card.opens, card.closes);
    CHECK(factory.last_error == -5, "last_error %d", factory.last_error);
    finish("size_unknown");

    begin(1000);
    card.fail_seek_set_in = 1; /* the rewind after measuring */
    CHECK(open_stream(&h) < 0, "rewind failed");
    CHECK(card.opens == 1 && card.closes == 1, "closed again");
    finish("rewind_fails");

    begin(0);
    CHECK(open_stream(&h) < 0, "a file of no length is not a finished download");
    CHECK(card.opens == 1 && card.closes == 1, "closed again");
    CHECK(factory.last_error == -EINVAL, "last_error %d", factory.last_error);
    finish("empty_file");

    /* Reopening after a failure works: the decoder does it on every seek. */
    begin(1000);
    card.fail_open = -1;
    CHECK(open_stream(&h) < 0, "fails");
    card.fail_open = 0;
    CHECK(open_stream(&h) == 0, "works once the card does");
    h.close(h.opaque);
    finish("reopen");
}

static void test_factory_init(void) {
    VtLocalStreamFactory f;
    char longpath[VT_LOCAL_STREAM_PATH_MAX + 10];
    memset(longpath, 'a', sizeof longpath);
    longpath[sizeof longpath - 1] = '\0';
    CHECK(vt_local_stream_factory_init_io(&f, "a", &io_locked) == 0, "plain");
    CHECK(f.factory.open != NULL && f.factory.opaque == &f, "factory defined");
    CHECK(vt_local_stream_factory_init_io(&f, "", &io_locked) < 0, "empty path");
    CHECK(vt_local_stream_factory_init_io(&f, NULL, &io_locked) < 0, "no path");
    CHECK(vt_local_stream_factory_init_io(&f, longpath, &io_locked) < 0, "path too long");
    CHECK(vt_local_stream_factory_init_io(NULL, "a", &io_locked) < 0, "no factory");
    CHECK(vt_local_stream_factory_init_io(&f, "a", NULL) < 0, "no table");
    VtLocalIo broken = io_locked;
    broken.read = NULL;
    CHECK(vt_local_stream_factory_init_io(&f, "a", &broken) < 0, "table without read");
    /* The longest path that fits is accepted and kept whole. */
    longpath[VT_LOCAL_STREAM_PATH_MAX - 1] = '\0';
    CHECK(vt_local_stream_factory_init_io(&f, longpath, &io_locked) == 0, "255 characters");
    CHECK(strcmp(f.path, longpath) == 0, "path kept");
    vt_local_stream_factory_term(&f);
    CHECK(f.factory.open == NULL, "term disarms the factory");
}

static void test_big_offsets(void) {
    /* A 3 GB download: every offset past 2^31 is a negative int on a 32-bit
     * target, and past 2^32 does not fit in one at all. */
    int64_t size = 3LL * 1024 * 1024 * 1024 + 12345;
    begin(size);
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    CHECK(h.size == size, "size %lld, expected %lld", LL(h.size), LL(size));
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_SIZE) == size, "size query");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_END) == size, "END 0");

    int64_t at = 0xC0000000LL; /* 3 GiB */
    CHECK(h.seek(h.opaque, at, VT_LOCAL_SEEK_SET) == at, "SET 3 GiB");
    CHECK(rd(&h, &at, 1000) == 1000, "read at 3 GiB");

    at = (1LL << 31) + 5; /* the first offset that is negative as an int */
    CHECK(h.seek(h.opaque, at, VT_LOCAL_SEEK_SET) == at, "SET 2^31 + 5");
    CHECK(rd(&h, &at, 1000) == 1000, "read at 2^31 + 5");

    CHECK(h.seek(h.opaque, -100, VT_LOCAL_SEEK_END) == size - 100, "END -100");
    at = size - 100;
    CHECK(rd(&h, &at, 500) == 100, "the last 100 bytes");
    CHECK(rd(&h, &at, 500) == 0, "EOF");

    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_SET) == 0, "SET 0");
    CHECK(h.seek(h.opaque, 1LL << 32, VT_LOCAL_SEEK_CUR) == (1LL << 32), "CUR + 4 GiB");
    CHECK(h.seek(h.opaque, 1LL << 32, VT_LOCAL_SEEK_CUR) == (2LL << 32), "CUR + 4 GiB again");
    CHECK(rd(&h, &at, 10) == 0, "beyond the 3 GB file: EOF");

    /* Absurd values from a corrupt index are refused, not wrapped. */
    CHECK(h.seek(h.opaque, INT64_MAX, VT_LOCAL_SEEK_SET) == INT64_MAX, "SET INT64_MAX");
    CHECK(h.seek(h.opaque, 1, VT_LOCAL_SEEK_CUR) < 0, "CUR past INT64_MAX");
    CHECK(h.seek(h.opaque, 0, VT_LOCAL_SEEK_CUR) == INT64_MAX, "position kept");
    CHECK(h.seek(h.opaque, INT64_MAX, VT_LOCAL_SEEK_END) < 0, "END + INT64_MAX");
    CHECK(h.seek(h.opaque, INT64_MIN, VT_LOCAL_SEEK_CUR) < 0, "CUR + INT64_MIN");
    CHECK(h.seek(h.opaque, INT64_MIN, VT_LOCAL_SEEK_END) < 0, "END + INT64_MIN");
    h.close(h.opaque);
    finish("big_offsets");

    /* Past 2^32 as well, and a read that straddles it. */
    begin(6LL * 1024 * 1024 * 1024 + 7);
    CHECK(open_stream(&h) == 0, "open the 6 GiB file");
    CHECK(h.size == 6LL * 1024 * 1024 * 1024 + 7, "size %lld", LL(h.size));
    at = (1LL << 32) - 3;
    CHECK(h.seek(h.opaque, at, VT_LOCAL_SEEK_SET) == at, "SET 2^32 - 3");
    CHECK(rd(&h, &at, 1000) == 1000, "read across 2^32");
    CHECK(h.seek(h.opaque, -7, VT_LOCAL_SEEK_END) == h.size - 7, "END -7");
    at = h.size - 7;
    CHECK(rd(&h, &at, 100) == 7, "the tail of the 6 GiB file");
    h.close(h.opaque);
    finish("big_offsets_6g");
}

static void test_independent_cursors(void) {
    /* The decoder opens two per session: video and audio read the same file
     * at their own pace. */
    begin(5 * (int64_t)WINDOW);
    VtDecoderStreamHandle a, b;
    CHECK(open_stream(&a) == 0 && open_stream(&b) == 0, "two cursors");
    CHECK(a.opaque != b.opaque, "distinct state");
    int64_t pa = 0, pb = 3 * (int64_t)WINDOW + 17;
    b.seek(b.opaque, pb, VT_LOCAL_SEEK_SET);
    CHECK(rd(&a, &pa, 1000) == 1000, "a reads at the start");
    CHECK(rd(&b, &pb, 1000) == 1000, "b reads at its own place");
    CHECK(rd(&a, &pa, 1000) == 1000, "a continues");
    CHECK(rd(&b, &pb, 1000) == 1000, "b continues");
    CHECK(a.seek(a.opaque, 0, VT_LOCAL_SEEK_CUR) == 2000, "a's position");
    CHECK(b.seek(b.opaque, 0, VT_LOCAL_SEEK_CUR) == 3 * (int64_t)WINDOW + 17 + 2000,
          "b's position");
    a.close(a.opaque);
    CHECK(rd(&b, &pb, 1000) == 1000, "b survives a's close");
    CHECK(card.closes == 1, "only a's descriptor closed: %d", card.closes);
    b.close(b.opaque);
    finish("independent_cursors");
}

static void test_without_lock(void) {
    /* A table without lock functions is legal (a private file). */
    memset(&card, 0, sizeof card);
    card.size = 300000;
    CHECK(vt_local_stream_factory_init_io(&factory, "p", &io_unlocked) == 0, "init");
    VtDecoderStreamHandle h;
    CHECK(open_stream(&h) == 0, "open");
    int64_t at = 0;
    while (at < card.size) if (rd(&h, &at, 50000) <= 0) break;
    CHECK(at == card.size, "all read");
    h.close(h.opaque);
    CHECK(card.opens == card.closes && card.lock_calls == 0, "no lock calls");
}

int main(void) {
    test_open_and_size();
    test_sequential();
    test_window_boundary();
    test_seek_whence();
    test_seek_costs();
    test_past_the_end();
    test_short_reads();
    test_read_errors();
    test_open_errors();
    test_factory_init();
    test_big_offsets();
    test_independent_cursors();
    test_without_lock();
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL LOCAL STREAM TESTS PASSED\n");
    return 0;
}
