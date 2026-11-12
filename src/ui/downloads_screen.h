/* ViTube — the Downloads tab, the download menu that video rows open, and the
 * small markers that say a row is downloaded or on its way.
 *
 * Everything here reads the engine (dl/dl.h) through ONE copy of its list,
 * refreshed when dl_revision() moves; no function below blocks, except the
 * ones that start the player, and those say so. */
#ifndef VITUBE_UI_DOWNLOADS_SCREEN_H
#define VITUBE_UI_DOWNLOADS_SCREEN_H

#include <stddef.h>

#include <stdint.h>

#include "ui/kinetic_scroll.h"
#include "ui/touch.h"
#include "yt/model.h"

/* --- the tab -------------------------------------------------------------
 * `list_top`/`list_bottom` are the band below the tab bar and above the hint
 * bar. The tab keeps a strip of its own at the top of it (the storage line),
 * so the rows scroll in a slightly smaller band; the caller does not need to
 * know. */

/* The tab was just switched to: read the list and the card's free space now
 * instead of showing what was true when it was last on screen. */
void downloads_screen_enter(void);

/* Rows on the tab. 0 means SELECT keeps its old meaning (the Mehr tab)
 * there, since there is no row for a menu to be about. */
int downloads_screen_count(void);

/* One frame of input and scrolling for the tab, called after the buttons
 * were read and before drawing. `direction` is vt_nav_repeat()'s answer.
 * Returns 1 when it ran something that took over the screen for a while (the
 * player): the caller must then re-read the buttons and reset touch, as it
 * does after every other blocking call. */
int downloads_screen_input(unsigned pressed, unsigned direction,
                           unsigned touch_flags, const UiTouchEvent *touch,
                           int list_top, int list_bottom);

/* The tab's content, inside the caller's clip rectangle. The menu is not
 * drawn here but by downloads_menu_draw(), once for every screen. */
void downloads_screen_draw(int list_top, int list_bottom);

/* The hint bar for the focused row — its ✕ names what ✕ will do for that
 * row's state. Written into `buf`; returns it. */
const char *downloads_screen_hint(char *buf, size_t cap);

/* Moves the tab's selection to this video and scrolls it into view ("Open in
 * Downloads"). Ignored when the video is not on the list. */
void downloads_screen_select(const char *video_id);

/* --- the menu ------------------------------------------------------------
 * One modal for both places it opens from: SELECT on a video row of a list
 * (Download, or what to do with the copy already there) and SELECT on a row
 * of the Downloads tab (play, pause, retry, delete). It remembers the video
 * id it was opened for, not an index: the list may change under it, and an
 * item that has gone makes it close. */

/* Opens the menu for a row of a video list. `video` is copied. */
void downloads_menu_open_video(const yt_video_t *video);
/* Opens the menu for the selected row of the tab (no-op without one). */
void downloads_menu_open_selected(void);
int downloads_menu_active(void);

typedef enum {
    DLM_NOTHING = 0,
    /* It ran the player: the caller must resync buttons and touch. */
    DLM_RESYNC,
    /* "Open in Downloads": the caller switches to the tab; the selection is
     * already set. */
    DLM_OPEN_DOWNLOADS
} DlMenuResult;

DlMenuResult downloads_menu_input(unsigned pressed, unsigned direction,
                                  unsigned touch_flags,
                                  const UiTouchEvent *touch);
void downloads_menu_draw(void);

/* A long press is the touch way to the menu: SELECT has no touch equivalent,
 * and a finger cannot reach the Downloads rows' menu (delete) any other way.
 * Returns the row of `rows` (row height `row_h`) a finger has been resting on
 * for DLS_LONG_PRESS_US without moving, or -1. Call it every frame with the
 * list's scroll state, after the scroll step; the touch layer is reset when
 * the menu opens, which also ends the press, so it fires once. */
#define DLS_LONG_PRESS_US 600000ULL
int downloads_long_press_row(const VtKineticScroll *k, uint64_t now_us,
                             int row_h, int count);

/* --- markers and messages --------------------------------------------------- */

typedef enum {
    DL_BADGE_NONE = 0,
    DL_BADGE_ACTIVE,   /* queued, resolving, downloading, waiting, verifying */
    DL_BADGE_PAUSED,
    DL_BADGE_DONE,
    DL_BADGE_FAILED
} DlBadge;

/* What the downloads list knows about this video, for a marker on its row.
 * Cheap enough to ask for every visible row of every frame. */
DlBadge downloads_badge(const char *video_id);
/* A 22 px round marker with its top-left corner at (x, y). */
void downloads_draw_badge(int x, int y, DlBadge badge);

/* The confirmation of an action ("Added to downloads"), drawn over the list
 * for a couple of seconds without stopping anything: queueing five videos in
 * a row must not cost five blocking toasts. `bottom` is the bottom edge of
 * the list band; call with clipping off, after the list, before the hint
 * bar. */
void downloads_snackbar_draw(int bottom);

#endif
