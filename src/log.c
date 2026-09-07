/* ViTube — tiny append-only diagnostics log (ux0:data/ViTube/log.txt). */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#include "app.h"

#define VT_LOG_PATH VITUBE_DATA_DIR "/log.txt"
#define VT_LOG_PREV_PATH VITUBE_DATA_DIR "/log.prev.txt"

static int g_first = 1;
/* Opened once on the first vt_log() call of a run and kept for the whole
 * process lifetime — see vt_log() below for why this replaced a
 * reopen-every-call design. -1 means "not opened yet" or "open failed". */
static SceUID g_fd = -1;
/* Serializes EVERY ux0: file access this app makes — not just the log.
 * vt_log() is called from up to seven threads (main, the HTTPS prefetch
 * threads, both demux threads), densest right around a quality switch, and
 * that is exactly where every pinned real-hardware crash landed
 * (2026-08-27, eight core dumps): PC inside sceIoWrite under vt_log
 * (twice, once before and once after the persistent-fd change), and once
 * inside yt_hd_pref_set's own sceIo calls while prefetch threads were
 * logging concurrently — different fault sites, one common factor:
 * concurrent SceIo traffic. Whether the fragility is in SceIo itself or in
 * the community storage stack under it (sd2vita-style setups route ux0:
 * through homebrew kernel code), fully serializing our own I/O removes the
 * collisions either way. Other ux0: writers (yt_hd_pref_set/get) must hold
 * this same lock via vt_io_lock()/vt_io_unlock(). */
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;

void vt_io_lock(void)   { pthread_mutex_lock(&g_io_lock); }
void vt_io_unlock(void) { pthread_mutex_unlock(&g_io_lock); }

void vt_log_init(void) {
    vt_io_lock();
    g_first = 1;
    if (g_fd >= 0) sceIoClose(g_fd);
    g_fd = -1;
    vt_io_unlock();
}

/* Truncating log.txt at the start of every run (below) means a crash
 * followed by a relaunch — the usual case, since the app needs restarting
 * before anyone thinks to pull the log — silently destroyed the one log
 * that would explain it (confirmed 2026-08-27: a crash dump's own
 * timestamp landed before the log file's first line). Rotate the previous
 * run's full log out of the way first, so it survives as log.prev.txt
 * through at least one relaunch instead of being overwritten. */
static void rotate_log(void) {
    sceIoRemove(VT_LOG_PREV_PATH);
    sceIoRename(VT_LOG_PATH, VT_LOG_PREV_PATH);
}

void vt_log(const char *fmt, ...) {
    /* The line is formatted before taking the lock — only the actual file
     * access is serialized, so a slow SD write stalls other loggers, never
     * their formatting. */
    char line[384];
    SceDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d ", now.hour,
                     now.minute, now.second);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m <= 0) return;
    n += m;
    if ((size_t)n >= sizeof line - 1) n = (int)sizeof line - 2;
    line[n++] = '\n';

    vt_io_lock();
    if (g_first) {
        sceIoMkdir(VITUBE_DATA_DIR, 0777);
        rotate_log();
        g_first = 0;
        g_fd = sceIoOpen(VT_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                         0777);
    }
    /* One fd for the whole run: reopening per call (open/write/close,
     * several times a second during playback) was itself pinned as a crash
     * site earlier the same day — see g_io_lock's comment for the full
     * incident chain. */
    if (g_fd >= 0) sceIoWrite(g_fd, line, n);
    vt_io_unlock();
}

/* Strong definition of the diagnostics sink that the decoder package
 * declares weak and empty (external/vita-hw-decoder/src/module_log.c).
 * Without it, every line the library emits is dropped on the floor —
 * which is why a "decode error" in this log has never come with the
 * sceAudiodec/FFmpeg error code that caused it (2026-08-28: a playback
 * that died at 46 ms on hardware and again at 0 ms after the software
 * fallback the app still had then, with nothing in the log to say which
 * of the two decode threads failed or why). Its call sites are one-shot,
 * error-path, or throttled (powers of two for the per-frame ones, a 30 s
 * interval for the pacing summary), so bridging them adds no per-frame
 * ux0: writes. */
void log_printf(const char *fmt, ...) {
    char line[352];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    vt_log("%s", line);
}
