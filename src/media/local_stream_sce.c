/* ViTube — the memory card behind local_stream.c: sceIo, serialised through
 * the app's I/O lock (the lock itself is taken by the stream, around each
 * call, so that the rule is in one place and the tests can check it). */
#include "media/local_stream.h"

#include <psp2/io/fcntl.h>

#include "log.h"

/* The stream hands these numbers straight to sceIoLseek. */
_Static_assert(SCE_SEEK_SET == VT_LOCAL_SEEK_SET &&
               SCE_SEEK_END == VT_LOCAL_SEEK_END,
               "sceIoLseek origins differ from the stream's whence values");

static int sce_open(void *ctx, const char *path) {
    (void)ctx;
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int sce_read(void *ctx, int fd, void *buffer, size_t size) {
    (void)ctx;
    return sceIoRead(fd, buffer, size);
}

static int64_t sce_seek(void *ctx, int fd, int64_t offset, int whence) {
    (void)ctx;
    return sceIoLseek(fd, offset, whence);
}

static void sce_close(void *ctx, int fd) {
    (void)ctx;
    sceIoClose(fd);
}

static const VtLocalIo g_sce_io = {
    .ctx = NULL,
    .lock = vt_io_lock,
    .unlock = vt_io_unlock,
    .open = sce_open,
    .read = sce_read,
    .seek = sce_seek,
    .close = sce_close,
};

int vt_local_stream_factory_init(VtLocalStreamFactory *factory,
                                 const char *path) {
    return vt_local_stream_factory_init_io(factory, path, &g_sce_io);
}
