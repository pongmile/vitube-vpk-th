/* ViTube — suspend / connectivity watcher; see netwatch.h.
 *
 * Suspend is detected from the wall clock, not from a power callback: every
 * thread of the app is frozen while the console sleeps, but the RTC keeps
 * running, so a thread that samples it every quarter second and suddenly
 * finds seconds between two samples has just been woken. That needs nothing
 * but sceRtcGetCurrentTick(), keeps working whichever loop the main thread
 * was in at the time, and cannot miss a callback because no thread happened
 * to be waiting in a callback-processing state. A user setting the clock
 * forward reads as a wake too; all that costs is one extra retry. */
#include "net/netwatch.h"

#include <pthread.h>
#include <stdint.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include "log.h"
#include "net/http.h"
#include "net/net_retry.h"

#define WATCH_POLL_US 250000

static pthread_t g_thread;
static int g_started;
static volatile int g_run;
static volatile unsigned g_resumes;
static volatile unsigned g_generation;
static volatile int g_connected = 1;

/* SceRtcTick counts microseconds (sceRtcGetTickResolution() is 1000000). */
static uint64_t wall_us(void) {
    SceRtcTick tick;
    if (sceRtcGetCurrentTick(&tick) < 0) return 0;
    return tick.tick;
}

static void *watch_main(void *unused) {
    (void)unused;
    uint64_t last = wall_us();
    int was = http_connected();
    g_connected = was;
    while (g_run) {
        sceKernelDelayThread(WATCH_POLL_US);
        uint64_t now = wall_us();
        if (last && now && vt_net_is_resume_gap(last, now)) {
            g_resumes++;
            g_generation++;
            vt_log("net: woke from suspend (%llu s asleep)",
                   (unsigned long long)((now - last) / 1000000));
        }
        last = now;
        int connected = http_connected();
        if (connected != was) {
            vt_log("net: %s", connected ? "connected" : "disconnected");
            if (connected) g_generation++;
            was = connected;
        }
        g_connected = connected;
    }
    return NULL;
}

void vt_netwatch_start(void) {
    if (g_started) return;
    g_run = 1;
    if (pthread_create(&g_thread, NULL, watch_main, NULL) == 0) {
        g_started = 1;
    } else {
        g_run = 0;
        vt_log("net: watcher thread failed to start");
    }
}

void vt_netwatch_stop(void) {
    if (!g_started) return;
    g_run = 0;
    pthread_join(g_thread, NULL);
    g_started = 0;
}

unsigned vt_netwatch_resume_count(void) { return g_resumes; }
unsigned vt_netwatch_generation(void) { return g_generation; }
int vt_netwatch_connected(void) { return g_connected; }
