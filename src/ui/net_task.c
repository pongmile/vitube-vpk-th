/* ViTube — network loads that wait out a reconnect instead of failing.
 *
 * Why this exists (reported on hardware, 2026-11-13): with the app left
 * open, lock the console with PS/power, wake it and go back in — and the
 * Start tab sat on "no videos loaded", with no sign that anything would
 * ever try again, although the Wi-Fi was back a few seconds later. Suspend
 * kills every socket and the console needs a moment to re-associate; a
 * load that ran in that window (or was in flight when the console went to
 * sleep) failed once, and every list load in this app treated one failure
 * as final. vt_ui_run_task() itself is unchanged and still used for what
 * must not be repeated behind the user's back.
 *
 * The policy — which failures are worth another attempt, the backoff, the
 * budget — is net/net_retry.c, kept free of psp2 so it is tested on the
 * build machine. Waking and the network state come from net/netwatch.c.
 * This file is only the frames and the attempt thread around them. */
#include "ui/net_task.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "log.h"
#include "net/http.h"
#include "net/net_retry.h"
#include "net/netwatch.h"
#include "ui/strings.h"
#include "yt/oauth.h"

typedef struct {
    VtTaskFn fn;
    void *arg;
    volatile int done;
    int result;
} Attempt;

static void *attempt_main(void *opaque) {
    Attempt *attempt = opaque;
    attempt->result = attempt->fn(attempt->arg);
    __sync_synchronize();
    attempt->done = 1;
    return NULL;
}

static uint64_t now_ms(void) {
    return sceKernelGetProcessTimeWide() / 1000;
}

/* One run of fn on a worker thread behind the usual spinner, as in
 * vt_ui_run_task(), plus the cut-off on wake. *user_cancelled: Circle was
 * pressed. *aborted: the console slept while this was in flight and the
 * request was cancelled on wake. Left alone, a socket that died with the
 * suspend is only noticed when curl's connect (12 s) or low-speed (15 s)
 * timeout runs out — seconds of a spinner that no longer meant anything,
 * ending in the very failure the retry then has to undo anyway. */
static int run_attempt(const char *label, const char *detail, VtTaskFn fn,
                       void *arg, volatile int *cancel_flag,
                       int *user_cancelled, int *aborted) {
    Attempt attempt = { fn, arg, 0, -999 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, attempt_main, &attempt) != 0)
        return -999;

    unsigned resumes = vt_netwatch_resume_count();
    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    unsigned frame = 0;
    while (!attempt.done) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        int ending = *user_cancelled || *aborted;
        /* Circle still counts once the attempt is being cut off after a
         * wake: the hint bar keeps promising it, and that cut-off can take
         * a while — curl is built without the threaded resolver, so a DNS
         * lookup on the still-dead network does not see the cancel flag
         * until it times out. Ignoring the press there made the user press
         * again on the reconnect screen. The flag is already 1 then, and
         * user_cancelled makes vt_ui_run_net_task() leave it set, so the
         * caller takes its normal "cancelled" path. */
        if (cancel_flag && !*user_cancelled && (pressed & SCE_CTRL_CIRCLE)) {
            *cancel_flag = 1;
            *user_cancelled = 1;
        } else if (cancel_flag && !ending &&
                   vt_netwatch_resume_count() != resumes) {
            *cancel_flag = 1;
            *aborted = 1;
            vt_log("net: request was in flight across a suspend, cut off");
        }
        const char *text = *user_cancelled ? vt_str(S_CANCELLING) : label;
        const char *sub = *user_cancelled ? NULL
                        : *aborted        ? vt_str(S_RECONNECTING)
                                          : detail;
        vt_ui_busy_frame(text, sub, frame++, cancel_flag && !*user_cancelled);
    }
    pthread_join(thread, NULL);
    return attempt.result;
}

/* The reconnect screen between attempts. Returns 1 when the next attempt is
 * due, 0 when Circle ended the load (and sets *cancel_flag, so the caller
 * takes the same "cancelled" path as for the spinner). */
static int wait_for_retry(VtNetRetry *retry, volatile int *cancel_flag) {
    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    unsigned resumes = vt_netwatch_resume_count();
    char detail[96];
    for (unsigned frame = 0;; frame++) {
        uint64_t now = now_ms();
        /* Slept again on this screen: what was counted before says nothing
         * about the network after it. */
        if (vt_netwatch_resume_count() != resumes) {
            resumes = vt_netwatch_resume_count();
            vt_net_retry_resumed(retry, now);
        }
        int connected = vt_netwatch_connected();
        if (vt_net_retry_poll(retry, now, connected) == VT_NET_RETRY_GO)
            return 1;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        if (cancel_flag && (pressed & SCE_CTRL_CIRCLE)) {
            *cancel_flag = 1;
            return 0;
        }
        /* Offline, a countdown would be a promise the next attempt cannot
         * keep — it is made on schedule all the same (SceNetCtl is not
         * trusted enough to hold a load back on its word alone), but it is
         * the network coming back that will make it work. */
        if (connected)
            snprintf(detail, sizeof detail, vt_str(S_RETRY_IN),
                     vt_net_retry_seconds_left(retry, now));
        else
            snprintf(detail, sizeof detail, "%s", vt_str(S_WAITING_FOR_WIFI));
        vt_ui_busy_frame(vt_str(S_RECONNECTING), detail, frame,
                         cancel_flag != NULL);
    }
}

/* vt_netwatch_generation() when the OAuth refresh hold was last dropped. */
static unsigned g_hold_generation;

int vt_ui_run_net_task(const char *label, VtTaskFn fn, void *arg,
                       volatile int *cancel_flag) {
    /* A refresh failure from before the latest wake or reconnect says
     * nothing about the network now. Without this, a load that gave up and
     * left the hold behind answered the next one — the Abos tab opened a
     * few seconds later on a working network — with "token refresh failed"
     * without even trying, and that failure is not a transport error, so it
     * would not have been retried either. */
    if (vt_netwatch_generation() != g_hold_generation) {
        g_hold_generation = vt_netwatch_generation();
        yt_oauth_forget_refresh_failure();
    }
    VtNetRetry retry;
    vt_net_retry_start(&retry, now_ms());
    char detail[96];
    for (int attempt = 1;; attempt++) {
        unsigned transport_before = http_transport_failures();
        unsigned resumes = vt_netwatch_resume_count();
        int user_cancelled = 0, aborted = 0;
        if (attempt > 1)
            snprintf(detail, sizeof detail, vt_str(S_RETRY_ATTEMPT), attempt);
        int ret = run_attempt(label, attempt > 1 ? detail : NULL, fn, arg,
                              cancel_flag, &user_cancelled, &aborted);
        if (user_cancelled) return ret;
        /* Our own cancel, not the user's: the caller must not see it. The
         * worker has been joined, so nothing reads the flag any more. */
        if (aborted) *cancel_flag = 0;
        if (ret >= 0) return ret;
        /* -2 is "not signed in", -999 "no thread": answers, not outages. */
        if (ret == -2 || ret == -999) return ret;

        uint64_t now = now_ms();
        int connected = vt_netwatch_connected();
        int transient = vt_net_failure_transient(
            aborted, connected, http_transport_failures() != transport_before);
        if (vt_netwatch_resume_count() != resumes)
            vt_net_retry_resumed(&retry, now);
        if (!vt_net_retry_failed(&retry, now, transient, connected)) {
            if (transient)
                vt_log("net: giving up after %d attempt(s), ret=%d", attempt,
                       ret);
            return ret;
        }
        vt_log("net: attempt %d failed (ret=%d, %s), retrying", attempt, ret,
               aborted ? "cut off by suspend"
               : !connected ? "offline" : "transport error");
        if (!wait_for_retry(&retry, cancel_flag)) return ret;
        /* The failed attempt may have left the OAuth refresh on its 30 s
         * hold; the retry is exactly the request that hold must not answer
         * (see yt_oauth_forget_refresh_failure). */
        yt_oauth_forget_refresh_failure();
    }
}
