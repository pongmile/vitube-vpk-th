/* ViTube — persisted HD quality preference (local only). See hd_helper.h. */
#include "yt/hd_helper.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "app.h"
#include "log.h"

/* Both helpers hold vt_io_lock: a real-hardware crash (2026-08-27) faulted
 * inside yt_hd_pref_set's sceIo calls while the HTTPS prefetch threads were
 * writing log lines concurrently — all of this app's ux0: file access has
 * to be serialized through the one lock (see log.c). */
/* Cached after the first read: the Mehr tab shows this value on every frame,
 * and reading ux0: sixty times a second (under the app-wide I/O lock that
 * the log, the thumbnail worker and the HLS workers share) is exactly the
 * dense SceIo traffic the crash history singles out. -1 = not read yet. */
static int g_hd_pref = -1;

int yt_hd_pref_get(void) {
    if (g_hd_pref >= 0) return g_hd_pref;
    vt_io_lock();
    SceUID fd = sceIoOpen(VITUBE_HD_PREF_PATH, SCE_O_RDONLY, 0);
    /* No stored choice means HD: 720p is the intended default experience,
     * playback falls back to a direct format on its own when HLS fails
     * (see player_screen.c). The file only exists once the user has made a
     * manual pick in the Triangle menu. */
    char c = '1';
    if (fd >= 0) {
        sceIoRead(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
    g_hd_pref = c == '1';
    return g_hd_pref;
}

void yt_hd_pref_set(int enabled) {
    g_hd_pref = enabled ? 1 : 0;
    vt_io_lock();
    sceIoMkdir(VITUBE_DATA_DIR, 0777);
    SceUID fd = sceIoOpen(VITUBE_HD_PREF_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        char c = enabled ? '1' : '0';
        sceIoWrite(fd, &c, 1);
        sceIoClose(fd);
    }
    vt_io_unlock();
}
