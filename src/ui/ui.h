/* ViTube — vita2d runtime, font and small draw helpers. */
#ifndef VITUBE_UI_UI_H
#define VITUBE_UI_UI_H

#include <stdint.h>
#include <vita2d.h>

int vt_ui_init(void);
void vt_ui_term(void);

/* `scale` is kept as the call-site vocabulary, but it now selects one of a
 * few pre-rasterised font sizes rather than stretching a bitmap — see the
 * comment above the font table in ui.c.
 * y is the TOP of the line (not the baseline). */
void vt_ui_text(int x, int y, unsigned int color, float scale, const char *text);
int vt_ui_text_width(float scale, const char *text);
/* Draw clipped to max_w pixels. */
void vt_ui_text_clipped(int x, int y, unsigned int color, float scale,
                        int max_w, const char *text);
void vt_ui_rect(int x, int y, int w, int h, unsigned int color);

/* Header with app title, connection/login state, clock and battery. */
void vt_ui_top_bar(const char *subtitle, int logged_in);
/* Bottom hint bar, e.g. "△ Suche   ✕ Abspielen". */
void vt_ui_hint_bar(const char *hint);
/* Persistent tab row below the top bar (Start/Suche/Abos). y is fixed at
 * the top bar's bottom edge; returns the row's height in pixels. */
int vt_ui_tab_bar(const char *const *tabs, int count, int active);

/* D-pad + left-stick navigation with initial delay and repeat.
 * Returns direction bits (SCE_CTRL_UP/DOWN/LEFT/RIGHT). */
typedef struct {
    unsigned int direction;
    uint64_t next_time_us;
} VtNavRepeat;
unsigned int vt_nav_repeat(VtNavRepeat *nav, unsigned int pressed,
                           unsigned int held, unsigned char lx,
                           unsigned char ly);

/* Runs fn(arg) on a worker thread while drawing a spinner with `label`.
 * Circle sets *cancel_flag = 1 (the task must honor it to finish early).
 * Returns the task's return value, or -999 on thread failure. */
typedef int (*VtTaskFn)(void *arg);
int vt_ui_run_task(const char *label, VtTaskFn fn, void *arg,
                   volatile int *cancel_flag);

/* Full-frame message with a small pause (error/info toast). */
void vt_ui_toast(const char *line1, const char *line2, int frames);

/* One entry of a modal menu. `value` is drawn right-aligned and may be NULL;
 * `marked` puts a dot beside it — the entry currently in effect, as opposed
 * to the one the cursor happens to be on. */
typedef struct {
    const char *label;
    const char *value;
    int marked;
} VtMenuItem;

/* Dims the screen, centres a titled box and draws the entries with `cursor`
 * highlighted. The box grows to fit its widest row.
 *
 * Drawing only, no input loop: the player has to present a video frame under
 * every iteration and the list screens must not, so a shared loop would have
 * to hand that difference back out through a callback. Callers keep their
 * own loop and call this once per frame. */
void vt_ui_menu_draw(const char *title, const VtMenuItem *items, int count,
                     int cursor);

#endif
