/* ViTube — decoder stream over a file on the memory card. The contract with
 * the decoder, the read-ahead size and the lock rule are in local_stream.h;
 * this is the bookkeeping behind them. No psp2 and no log here on purpose (see
 * the header): the build machine compiles this file as it is. */
#include "media/local_stream.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* One cursor: one descriptor, one read-ahead window, one position.
 *
 * `pos` is where the DEMUXER is. `file_pos` is where the descriptor actually
 * is, and the two are kept apart on purpose: seek() only moves `pos`, and the
 * descriptor is repositioned lazily, by the refill that needs it. The
 * demuxer seeks a lot and mostly short — back a few bytes to re-read a box
 * header, to the other end of the file for the index and straight back — and
 * a seek that lands inside the window (or is overwritten by the next seek
 * before anything is read) then costs no card access at all. A sequential
 * read never repositions anything: the descriptor is already where the next
 * refill starts. file_pos is -1 whenever a call failed halfway and the
 * descriptor's position is no longer known; the next refill then repositions
 * unconditionally. */
typedef struct LocalCursor {
    const VtLocalIo *io;
    int fd;
    int64_t size;
    int64_t pos;
    int64_t file_pos;
    int64_t window_start; /* file offset of data[0] */
    size_t window_len;    /* valid bytes in data, 0 = nothing */
    unsigned char data[]; /* VT_LOCAL_STREAM_READAHEAD bytes */
} LocalCursor;

/* --- the file operations, each under the I/O lock for exactly that call -----
 * Nothing else in this file touches the table, so "the lock is held for one
 * call and no longer" is a property of these four functions alone. */

static int io_open(const VtLocalIo *io, const char *path) {
    if (io->lock) io->lock();
    int fd = io->open(io->ctx, path);
    if (io->unlock) io->unlock();
    return fd;
}

static int io_read(const VtLocalIo *io, int fd, void *buffer, size_t size) {
    if (io->lock) io->lock();
    int n = io->read(io->ctx, fd, buffer, size);
    if (io->unlock) io->unlock();
    return n;
}

static int64_t io_seek(const VtLocalIo *io, int fd, int64_t offset,
                       int whence) {
    if (io->lock) io->lock();
    int64_t at = io->seek(io->ctx, fd, offset, whence);
    if (io->unlock) io->unlock();
    return at;
}

static void io_close(const VtLocalIo *io, int fd) {
    if (io->lock) io->lock();
    io->close(io->ctx, fd);
    if (io->unlock) io->unlock();
}

/* --- cursor ---------------------------------------------------------------- */

/* Reads the window that starts at c->pos. Returns the bytes now in the window
 * (> 0), 0 at the end of the file, or a negative error. On any failure the
 * window is empty and, if the descriptor may have moved, file_pos is unknown —
 * so the next call starts from a clean slate and a transient error is simply
 * retried by the demuxer's next read. */
static int refill(LocalCursor *c) {
    c->window_len = 0;
    if (c->file_pos != c->pos) {
        int64_t at = io_seek(c->io, c->fd, c->pos, VT_LOCAL_SEEK_SET);
        if (at != c->pos) {
            c->file_pos = -1;
            return at < 0 ? (int)at : -EIO;
        }
        c->file_pos = at;
    }
    int n = io_read(c->io, c->fd, c->data, VT_LOCAL_STREAM_READAHEAD);
    if (n < 0) {
        c->file_pos = -1;
        return n;
    }
    if (n == 0) return 0;
    c->window_start = c->pos;
    c->window_len = (size_t)n;
    c->file_pos = c->pos + n;
    return n;
}

static int local_read(void *opaque, void *buffer, size_t size) {
    LocalCursor *c = opaque;
    if (!c || !buffer) return -EINVAL;
    if (size == 0) return 0;
    /* The length is known and a finished file does not grow: at or past the
     * end the answer is EOF without asking the card. This is also what makes a
     * seek beyond the end harmless — whatever the card does for a position it
     * does not have is never put to the test. */
    if (c->pos >= c->size) return 0;

    int in_window = c->window_len > 0 && c->pos >= c->window_start &&
                    c->pos < c->window_start + (int64_t)c->window_len;
    if (!in_window) {
        int n = refill(c);
        if (n <= 0) return n;
    }
    size_t offset = (size_t)(c->pos - c->window_start);
    size_t available = c->window_len - offset;
    size_t take = size < available ? size : available;
    memcpy(buffer, c->data + offset, take);
    c->pos += (int64_t)take;
    return (int)take;
}

static int64_t local_seek(void *opaque, int64_t offset, int whence) {
    LocalCursor *c = opaque;
    if (!c) return -EINVAL;
    whence &= ~VT_LOCAL_SEEK_FORCE;
    if (whence == VT_LOCAL_SEEK_SIZE) return c->size;

    int64_t base;
    switch (whence) {
    case VT_LOCAL_SEEK_SET: base = 0; break;
    case VT_LOCAL_SEEK_CUR: base = c->pos; break;
    case VT_LOCAL_SEEK_END: base = c->size; break;
    default: return -EINVAL;
    }
    /* A 3 GB file puts offsets past 32 bits, so this is int64 throughout; the
     * guards are for a corrupt index handing back something absurd, not for
     * anything a real file does. A failed seek leaves the position alone. */
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return -EINVAL;
    int64_t target = base + offset;
    if (target < 0) return -EINVAL;
    /* Past the end is allowed, as for any file: the next read says EOF. */
    c->pos = target;
    return target;
}

static void local_close(void *opaque) {
    LocalCursor *c = opaque;
    if (!c) return;
    if (c->fd >= 0) io_close(c->io, c->fd);
    free(c);
}

/* --- factory --------------------------------------------------------------- */

static int local_open(void *opaque, VtDecoderStreamHandle *out) {
    VtLocalStreamFactory *factory = opaque;
    if (!factory || !factory->io || !out) return -EINVAL;
    const VtLocalIo *io = factory->io;

    LocalCursor *c = calloc(1, sizeof *c + VT_LOCAL_STREAM_READAHEAD);
    if (!c) return factory->last_error = -ENOMEM;

    int fd = io_open(io, factory->path);
    if (fd < 0) {
        free(c);
        return factory->last_error = fd;
    }
    /* Length, then back to the start. If the second seek fails the descriptor
     * is somewhere unknown; refuse the file rather than track that. */
    int64_t size = io_seek(io, fd, 0, VT_LOCAL_SEEK_END);
    if (size >= 0 && io_seek(io, fd, 0, VT_LOCAL_SEEK_SET) != 0) size = -EIO;
    if (size <= 0) {
        io_close(io, fd);
        free(c);
        return factory->last_error = size < 0 ? (int)size : -EINVAL;
    }

    c->io = io;
    c->fd = fd;
    c->size = size;
    c->file_pos = 0;
    /* fetch and url stay NULL: this is a media file, not an HLS playlist. */
    *out = (VtDecoderStreamHandle){
        .opaque = c,
        .read = local_read,
        .seek = local_seek,
        .close = local_close,
        .size = size,
    };
    return 0;
}

int vt_local_stream_factory_init_io(VtLocalStreamFactory *factory,
                                    const char *path, const VtLocalIo *io) {
    if (!factory || !path || !path[0] || !io || !io->open || !io->read ||
        !io->seek || !io->close)
        return -1;
    if (strlen(path) >= sizeof factory->path) return -1;
    memset(factory, 0, sizeof *factory);
    memcpy(factory->path, path, strlen(path) + 1);
    factory->io = io;
    factory->factory.opaque = factory;
    factory->factory.open = local_open;
    return 0;
}

void vt_local_stream_factory_term(VtLocalStreamFactory *factory) {
    if (!factory) return;
    /* The decoder closed its cursors with the player. What is left is a table
     * pointer and a path; clearing open makes a use after term fail at the
     * decoder's open instead of reading a stale path. */
    factory->factory.open = NULL;
    factory->io = NULL;
}
