/* Headless scenarios for the "load more" paging of list_screen.c (issue #10).
 *
 * The REAL list_screen_run() loop runs here, with the real ui.c, kinetic
 * scroll, strings and rows view, against stand-ins for the console (vita2d
 * recording what is drawn, a scripted pad and touch panel, a fake clock) and
 * for the yt layer (p_backend.inc: feeds that page, with a switch for a
 * backend that fails, answers 200 with no rows, repeats rows or is cancelled
 * with Circle). Every continuation call is recorded with the token, surface
 * and visitor it was made with, so a scenario can say WHICH token went out
 * WHERE and how often, across Down on the last row, a fling into the end,
 * failures, a held button, sign-in and region changes, tab switches and
 * channel lists.
 *
 * list_screen.c is #included below (not linked) so the lists are visible to
 * the checks; define LIST_SCREEN_SRC to run a modified copy of it. One
 * scenario per process: `p_harness N`, N from 1 (see NAMES); run.sh runs all.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>

#include "app.h"
#include "ui/dl_format.h"
#include "ui/list_screen.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "stub_ctl.h"

#define MB (1024LL * 1024)
#define KB 1024LL
#define GB (1024LL * 1024 * 1024)

static int g_fail;
static int g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL [frame %d] %s:%d: ", g_frame, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the schedule ------------------------------------------------------------- */
typedef enum { EV_PAD, EV_TOUCH, EV_CALL, EV_DUMP } EvKind;
typedef struct {
    int frame;
    EvKind kind;
    unsigned a;
    int x, y, dx, dy;
    void (*fn)(void);
    char name[48];
} Ev;
static Ev g_ev[4000];
static int g_nev;
static int T = 2;

static void ev_add(EvKind k, int frame, unsigned a, int x, int y, int dx, int dy, void (*fn)(void), const char *name) {
    Ev *e = &g_ev[g_nev++];
    memset(e, 0, sizeof *e);
    e->frame = frame; e->kind = k; e->a = a; e->x = x; e->y = y; e->dx = dx; e->dy = dy; e->fn = fn;
    if (name) snprintf(e->name, sizeof e->name, "%s", name);
}
static void wait(int n) { T += n; }
static void press(unsigned b) { ev_add(EV_PAD, T, b, 0, 0, 0, 0, 0, 0); ev_add(EV_PAD, T + 1, 0, 0, 0, 0, 0, 0, 0); T += 3; }
static void call(void (*fn)(void)) { ev_add(EV_CALL, T, 0, 0, 0, 0, 0, fn, 0); T += 1; }
static void dump(const char *name) { ev_add(EV_DUMP, T, 0, 0, 0, 0, 0, 0, name); T += 1; }
static void tap(int x, int y) {
    ev_add(EV_TOUCH, T, UI_TOUCH_EVENT_DOWN, x, y, x, y, 0, 0);
    ev_add(EV_TOUCH, T + 1, UI_TOUCH_EVENT_HOLD, x, y, x, y, 0, 0);
    ev_add(EV_TOUCH, T + 2, UI_TOUCH_EVENT_UP | UI_TOUCH_EVENT_TAP, x, y, x, y, 0, 0);
    T += 4;
}
/* A drag from y0 to y1 in n frames, then lift (no tap: it moved). */
static void drag(int x, int y0, int y1, int n) {
    ev_add(EV_TOUCH, T, UI_TOUCH_EVENT_DOWN, x, y0, x, y0, 0, 0);
    for (int i = 1; i <= n; i++)
        ev_add(EV_TOUCH, T + i, UI_TOUCH_EVENT_MOVE, x, y0 + (y1 - y0) * i / n, x, y0, 0, 0);
    ev_add(EV_TOUCH, T + n + 1, UI_TOUCH_EVENT_UP, x, y1, x, y0, 0, 0);
    T += n + 3;
}

/* A slow drag that comes to rest before the finger lifts: no fling. */
static void drag_hold(int x, int y0, int y1, int n, int hold) {
    ev_add(EV_TOUCH, T, UI_TOUCH_EVENT_DOWN, x, y0, x, y0, 0, 0);
    for (int i = 1; i <= n; i++)
        ev_add(EV_TOUCH, T + i, UI_TOUCH_EVENT_MOVE, x, y0 + (y1 - y0) * i / n, x, y0, 0, 0);
    for (int i = 1; i <= hold; i++)
        ev_add(EV_TOUCH, T + n + i, UI_TOUCH_EVENT_HOLD, x, y1, x, y0, 0, 0);
    ev_add(EV_TOUCH, T + n + hold + 1, UI_TOUCH_EVENT_UP, x, y1, x, y0, 0, 0);
    T += n + hold + 3;
}

static int script(int f) {
    h_set_touch(0, 0, 0, 0, 0);
    for (int i = 0; i < g_nev; i++) {
        Ev *e = &g_ev[i];
        if (e->frame != f) continue;
        switch (e->kind) {
        case EV_PAD: g_pad_buttons = e->a; break;
        case EV_TOUCH: h_set_touch(e->a, e->x, e->y, e->dx, e->dy); break;
        case EV_CALL: e->fn(); break;
        case EV_DUMP: h_dump(e->name); break;
        }
    }
    return f > T + 2;
}

#define BTN_SELECT 0x1
#define BTN_START 0x8
#define BTN_UP 0x10
#define BTN_RIGHT 0x20
#define BTN_DOWN 0x40
#define BTN_LEFT 0x80
#define BTN_L 0x100
#define BTN_R 0x200
#define BTN_TRI 0x1000
#define BTN_CIRCLE 0x2000
#define BTN_CROSS 0x4000
#define BTN_SQUARE 0x8000

static void seen(const char *s) { CHECK(h_text_seen(s), "expected text \"%s\" on screen", s); }
static void unseen(const char *s) { CHECK(!h_text_seen(s), "did not expect text \"%s\" on screen", s); }
static int badge_circles(void) { return h_op_count('C'); }

static const char *ID(int i) { static char b[4][16]; static int k; k = (k + 1) & 3; snprintf(b[k], 16, "vid%08d", i); return b[k]; }

static const char *call_at(int i) { return i < stub_ncalls ? stub_calls[i] : "(none)"; }

#ifndef LIST_SCREEN_SRC
#define LIST_SCREEN_SRC "ui/list_screen.c"
#endif
#include LIST_SCREEN_SRC

#include "p_scen.inc"
#include "p_scen2.inc"

static void report(void) { printf("%d checks, %d failed\n", g_checks, g_fail); }

static void (*const SCEN[])(void) = { s1, s2, s2b, s3, s4, s5, s6, s7, s8, s9, s10, s11, s13, s14, s15, s16, s16b, s17, s18, s18b, s18c, s19, s20 };
static const char *const NAMES[] = { "home pages by Down", "failure+hold", "3 failures", "empty page", "repeats followed",
    "search pages", "channel pages", "signed in", "reset on sign-in/region", "cancel", "touch at end", "no token mix", "fling failing", "tab round trip", "region wipes Suche", "ITEMS_MAX exact", "ITEMS_MAX 239", "channel A/B/search", "hold fail 120", "hold ok 120", "hold tiny pages", "stale 200/0 rows", "feed ends" };

int main(int argc, char **argv) {
    int which = argc > 1 ? atoi(argv[1]) : 1;
    if (which == 0) { printf("scenarios: %d\n", (int)(sizeof SCEN / sizeof *SCEN)); return 0; }
    if (which < 0 || which > (int)(sizeof SCEN / sizeof *SCEN)) { fprintf(stderr, "no such scenario\n"); return 2; }
    vt_lang_set_choice(VT_LANG_EN);
    h_home_count = 76;
    if (which == 8) h_logged_in = 1;
    printf("scenario %d: %s\n", which, NAMES[which - 1]);
    atexit(report);
    SCEN[which - 1]();
    h_script = script;
    list_screen_run();
    return 0;
}
