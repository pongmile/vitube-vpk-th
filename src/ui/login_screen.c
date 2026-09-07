/* ViTube — Google device-flow login screen. */
#include "ui/login_screen.h"

#include <pthread.h>
#include <stdio.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "yt/oauth.h"

static void draw_frame(const char *heading, const char *big, const char *info,
                       const char *hint) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vt_ui_top_bar(vt_str(S_SIGNIN_TITLE), yt_oauth_logged_in());
    int w = vt_ui_text_width(1.0f, heading);
    vt_ui_text((VT_SCREEN_W - w) / 2, 120, VT_COLOR_MUTED, 1.0f, heading);
    if (big && big[0]) {
        int bw = vt_ui_text_width(2.0f, big);
        vt_ui_rect((VT_SCREEN_W - bw) / 2 - 24, 180, bw + 48, 72,
                   VT_COLOR_SURFACE);
        vt_ui_text((VT_SCREEN_W - bw) / 2, 196, VT_COLOR_TEXT, 2.0f, big);
    }
    if (info && info[0]) {
        int iw = vt_ui_text_width(1.0f, info);
        vt_ui_text((VT_SCREEN_W - iw) / 2, 290, VT_COLOR_TEXT, 1.0f, info);
    }
    vt_ui_hint_bar(hint);
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
    vita2d_swap_buffers();
}

static volatile int g_revoke_cancel;

static int revoke_task(void *opaque) {
    (void)opaque;
    return yt_oauth_revoke(&g_revoke_cancel);
}

static int logged_in_menu(void) {
    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    /* Cross means "confirm/open" everywhere else in the app, and this screen
     * is opened just to check who is signed in — so one press must not be
     * able to end the session. The first press arms the question instead. */
    int confirming = 0;
    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        if (pressed & SCE_CTRL_CIRCLE) {
            if (!confirming) return 0;
            confirming = 0;
            continue;
        }
        if (pressed & SCE_CTRL_CROSS) {
            if (!confirming) {
                confirming = 1;
                continue;
            }
            g_revoke_cancel = 0;
            int revoked = vt_ui_run_task(vt_str(S_SIGNING_OUT), revoke_task, NULL,
                                         &g_revoke_cancel);
            /* The local logout runs whatever Google answered: a token the
             * server will not take back is still one this device must
             * forget, and a dead network must not trap the user signed in. */
            yt_oauth_logout();
            vt_ui_toast(vt_str(S_SIGNED_OUT),
                        revoked == 0 ? NULL
                                     : vt_str(S_REVOKE_FAILED),
                        revoked == 0 ? 90 : 150);
            return 1;
        }
        if (confirming)
            draw_frame(vt_str(S_REALLY_SIGN_OUT), NULL,
                       vt_str(S_SIGNOUT_CONSEQUENCE),
                       vt_str(S_HINT_CONFIRM_SIGNOUT));
        else
            draw_frame(vt_str(S_YOU_ARE_SIGNED_IN), NULL,
                       vt_str(S_SUBS_APPEAR_IN_FEED),
                       vt_str(S_HINT_SIGNOUT));
    }
}

typedef struct {
    yt_device_auth_t *auth;
    volatile int *cancel;
    volatile int done;
    int result;
} PollState;

static void *poll_thread(void *opaque) {
    PollState *state = opaque;
    state->result = yt_oauth_device_poll(state->auth, state->cancel);
    __sync_synchronize();
    state->done = 1;
    return NULL;
}

typedef struct {
    yt_device_auth_t *auth;
    volatile int *cancel;
} StartTask;

static int start_task(void *opaque) {
    StartTask *task = opaque;
    return yt_oauth_device_start(task->auth, task->cancel);
}

int login_screen_run(void) {
    if (yt_oauth_logged_in()) return logged_in_menu();

    static volatile int cancel;
    cancel = 0;
    yt_device_auth_t auth;
    StartTask start = { &auth, &cancel };
    int ret = vt_ui_run_task(vt_str(S_SIGNIN_STARTING), start_task, &start,
                             &cancel);
    if (cancel) return 0;
    if (ret < 0) {
        vt_ui_toast(vt_str(S_SIGNIN_IMPOSSIBLE), vt_str(S_GOOGLE_UNREACHABLE),
                    150);
        return 0;
    }

    char info[192];
    snprintf(info, sizeof info,
             vt_str(S_SIGNIN_INSTRUCTION),
             auth.verification_url);

    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    uint64_t interval_us = (uint64_t)auth.interval_seconds * 1000000ULL;
    uint64_t next_poll = sceKernelGetProcessTimeWide() + interval_us;
    uint64_t deadline = sceKernelGetProcessTimeWide() +
                        (uint64_t)auth.expires_in_seconds * 1000000ULL;
    PollState poll = { &auth, &cancel, 1, 1 }; /* "done" until first spawn */
    pthread_t thread;
    int transient_failures = 0;
    int polling = 0;

    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        uint64_t now = sceKernelGetProcessTimeWide();

        if (pressed & SCE_CTRL_CIRCLE) {
            cancel = 1;
            if (polling) pthread_join(thread, NULL);
            return 0;
        }
        if (polling && poll.done) {
            pthread_join(thread, NULL);
            polling = 0;
            if (poll.result == 0) {
                vt_ui_toast(vt_str(S_SIGNED_IN), NULL, 90);
                return 1;
            }
            if (poll.result == YT_OAUTH_POLL_NOT_PERSISTED) {
                /* Signed in for this session, but the token did not reach the
                 * memory card — say so instead of a plain "Angemeldet!", which
                 * is how this failure used to hide until the next restart. */
                vt_ui_toast(vt_str(S_SIGNED_IN_NOT_SAVED),
                            vt_str(S_SIGNIN_AGAIN_AFTER_RESTART), 240);
                return 1;
            }
            if (poll.result == YT_OAUTH_POLL_EXPIRED) {
                vt_ui_toast(vt_str(S_CODE_EXPIRED), vt_str(S_TRY_AGAIN), 120);
                return 0;
            }
            if (poll.result == YT_OAUTH_POLL_DENIED) {
                vt_ui_toast(vt_str(S_SIGNIN_DENIED), NULL, 120);
                return 0;
            }
            if (poll.result == YT_OAUTH_POLL_TRANSIENT) {
                /* One dropped round trip is not a failed sign-in: the code
                 * on screen is still valid, so keep polling. The deadline
                 * below bounds the whole attempt; a long run of failures
                 * (the link is really gone) ends it earlier. */
                if (++transient_failures >= 8) {
                    vt_ui_toast(vt_str(S_SIGNIN_IMPOSSIBLE),
                                vt_str(S_NETWORK_ERROR), 150);
                    return 0;
                }
            } else if (poll.result < 0) {
                vt_ui_toast(vt_str(S_SIGNIN_IMPOSSIBLE), NULL, 120);
                return 0;
            } else {
                transient_failures = 0;
            }
            next_poll = sceKernelGetProcessTimeWide() + interval_us;
        }
        if (!polling && now >= next_poll) {
            if (now > deadline) {
                vt_ui_toast(vt_str(S_CODE_EXPIRED), vt_str(S_TRY_AGAIN), 120);
                return 0;
            }
            poll.done = 0;
            poll.result = 1;
            if (pthread_create(&thread, NULL, poll_thread, &poll) == 0)
                polling = 1;
            else
                next_poll = now + interval_us;
        }
        draw_frame(vt_str(S_SIGNIN_WITH_GOOGLE), auth.user_code, info,
                   vt_str(S_CANCEL_HINT));
    }
}
