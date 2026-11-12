/* ViTube — decoder stream over a file on the memory card (offline playback).
 *
 * The third shape a PlayerSource can take next to the HTTPS range stream and
 * HLS: a finished download, read with no network at all. The decoder wants a
 * factory whose open() hands out a fresh, independent cursor each time (two per
 * session — video and audio demux the same container separately — and two more
 * on every seek, since vita_hw_decoder_seek() closes the session and reopens
 * both), and a cursor is four things: read, seek, close and a size.
 *
 * What the decoder (external/vita-hw-decoder/src/vita_hw_decoder.c) expects of
 * them, and what this implements:
 *
 *  - read(buf, n) returns the bytes delivered (1..n, a short count is fine and
 *    cheaper than looping), 0 at end of file, <0 on error. The decoder maps 0
 *    to AVERROR_EOF and any negative to AVERROR(EIO).
 *  - seek(offset, whence) returns the NEW ABSOLUTE position (>= 0) or <0. The
 *    decoder forwards whence as 0/1/2 (SET/CUR/END) with FFmpeg's FORCE bit
 *    stripped. The AVSEEK_SIZE query never reaches seek(): the decoder answers
 *    it from handle.size itself. It is handled here too, by the same number
 *    FFmpeg uses, so the stream also behaves behind any other caller.
 *  - handle.size is the file's length in bytes, or -1 if unknown. It is always
 *    known here, and a file of no length is refused at open: it cannot be a
 *    finished download, and the player reports it as such.
 *  - close() is called exactly once per successful open().
 *
 * Every sceIo call this makes holds the app-wide I/O lock (log.h: vt_io_lock)
 * for that ONE call and nothing else. A real-hardware crash was traced to
 * concurrent unserialised SceIo; the download engine writing a file while the
 * decoder reads another is that situation by construction. The lock is never
 * held across two calls, so a slow 128 KiB read delays the engine's write or a
 * log line by one read's time, never by a whole refill-and-seek sequence.
 *
 * The file operations are a table (VtLocalIo) so that the logic — the
 * read-ahead window, the position bookkeeping, the lock discipline — runs
 * unchanged on the build machine against an in-memory file with injected
 * faults (tests/test_local_stream.c). The table for the real card lives in
 * local_stream_sce.c; this file has no psp2 and no log dependency. */
#ifndef VITUBE_MEDIA_LOCAL_STREAM_H
#define VITUBE_MEDIA_LOCAL_STREAM_H

#include <stddef.h>
#include <stdint.h>

#include "media/vita_decoder.h"

/* Bytes read from the card per request, and kept in memory per cursor.
 *
 * The decoder's own AVIO buffer asks for 64 KiB at a time, and a memory card
 * is markedly slower at small requests than at large ones, so this takes
 * twice that per call and serves the demuxer's reads from the copy. 128 KiB is
 * a little over a second of a 360p MP4 (about 100 KB/s of media), so a refill
 * is rare; two cursors per session cost 256 KiB of heap in all. Bigger would
 * make every refill after a far seek read more than it needs (a seek reopens
 * the cursors and the demuxer's first reads are the container index), and it
 * would hold the I/O lock for correspondingly longer per read. */
#define VT_LOCAL_STREAM_READAHEAD (128 * 1024)

/* Longest path a factory keeps (the engine's are ~45 characters). */
#define VT_LOCAL_STREAM_PATH_MAX 256

/* whence values. 0/1/2 are the C and sceIo numbers; the two flag bits are
 * FFmpeg's AVSEEK_SIZE and AVSEEK_FORCE. */
#define VT_LOCAL_SEEK_SET 0
#define VT_LOCAL_SEEK_CUR 1
#define VT_LOCAL_SEEK_END 2
#define VT_LOCAL_SEEK_SIZE 0x10000
#define VT_LOCAL_SEEK_FORCE 0x20000

/* The file operations one cursor needs, shaped like the sceIo calls they wrap:
 * a negative return is an error, read returns 0 at end of file, seek returns
 * the new absolute position. fd is whatever open returned. seek is only ever
 * called with VT_LOCAL_SEEK_SET or VT_LOCAL_SEEK_END. lock/unlock bracket each
 * call individually; NULL means no locking (a table for a private file). */
typedef struct VtLocalIo {
    void *ctx;
    void (*lock)(void);
    void (*unlock)(void);
    int (*open)(void *ctx, const char *path);
    int (*read)(void *ctx, int fd, void *buffer, size_t size);
    int64_t (*seek)(void *ctx, int fd, int64_t offset, int whence);
    void (*close)(void *ctx, int fd);
} VtLocalIo;

typedef struct VtLocalStreamFactory {
    char path[VT_LOCAL_STREAM_PATH_MAX];
    const VtLocalIo *io;
    /* The negative code of the last open() that failed (the sceIo error for a
     * file that is gone, -EINVAL for one of no length), 0 if none has. For the
     * caller's log: the decoder reports only that the stream failed. */
    int last_error;
    VtDecoderStreamFactory factory; /* pass .factory to the decoder */
} VtLocalStreamFactory;

/* Prepares a factory for `path` on the memory card (local_stream_sce.c). It
 * does not touch the file: a missing or unreadable one makes the decoder's
 * open fail, which is where the player looks. Returns 0, or -1 for a path
 * that is empty or does not fit. */
int vt_local_stream_factory_init(VtLocalStreamFactory *factory,
                                 const char *path);
/* Same over an arbitrary file-operations table, which must outlive the
 * factory. The tests' entry point. */
int vt_local_stream_factory_init_io(VtLocalStreamFactory *factory,
                                    const char *path, const VtLocalIo *io);
/* After the decoder player is destroyed (it owns the cursors and closes them);
 * the factory itself holds nothing to release. */
void vt_local_stream_factory_term(VtLocalStreamFactory *factory);

#endif
