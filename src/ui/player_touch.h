/* ViTube — front-panel touch handling for the player: the bottom bar's
 * geometry, its hit zones, and the drag-to-scrub state machine.
 *
 * The geometry lives here rather than next to the drawing code because two
 * places need it and they must not drift: player_hud.c paints the bar, this
 * module decides what a finger on it meant. Both used to carry their own
 * `bar_x = 20, bar_w = VT_SCREEN_W - 40`, which is how a progress bar starts
 * seeking somewhere other than where it looks.
 *
 * No vita2d and no psp2 headers: the decisions below are plain arithmetic and
 * are covered by tests/test_player_touch.c on the host. */
#ifndef VITUBE_UI_PLAYER_TOUCH_H
#define VITUBE_UI_PLAYER_TOUCH_H

#include <stdint.h>

#include "app.h"
#include "ui/touch.h"

/* --- bottom overlay geometry -------------------------------------------- */

#define VT_HUD_BAR_H 72
#define VT_HUD_BAR_Y (VT_SCREEN_H - VT_HUD_BAR_H)

/* Play/pause button. The glyph is drawn small; the target around it is not —
 * a 32 px square is not something a thumb hits reliably on a handheld that is
 * being held rather than set down. */
#define VT_HUD_PLAY_SIZE 26
#define VT_HUD_PLAY_X 16
#define VT_HUD_PLAY_Y (VT_SCREEN_H - 68)
#define VT_HUD_PLAY_HIT_X 8
#define VT_HUD_PLAY_HIT_Y (VT_SCREEN_H - 72)
#define VT_HUD_PLAY_HIT_W 52
#define VT_HUD_PLAY_HIT_H 34

/* Position/duration text, right of the button and sharing its centre line. */
#define VT_HUD_TIME_X (VT_HUD_PLAY_X + VT_HUD_PLAY_SIZE + 12)
#define VT_HUD_TEXT_Y (VT_SCREEN_H - 64)

/* Progress track, and the much taller band that counts as touching it. The
 * band starts exactly where the button's ends, so the two never overlap and
 * neither can steal the other's press. */
#define VT_HUD_TRACK_X 20
#define VT_HUD_TRACK_W (VT_SCREEN_W - 40)
#define VT_HUD_TRACK_Y (VT_SCREEN_H - 26)
#define VT_HUD_TRACK_H 6
#define VT_HUD_TRACK_HIT_Y (VT_HUD_PLAY_HIT_Y + VT_HUD_PLAY_HIT_H)
#define VT_HUD_TRACK_HIT_H 30

/* --- gestures ------------------------------------------------------------ */

/* Where on the track x points, clamped to its ends. */
uint64_t vt_player_touch_track_ms(int x, uint64_t duration_ms);

/* How far a finger may travel between press and release and still count as a
 * press on one spot. Deliberately looser than touch.c's 12 px tap slop: that
 * one guards list rows against a flick being read as a click, whereas here
 * the alternative gesture is a drag on the track, which starts somewhere
 * else entirely and is recognised on the way down. */
#define VT_HUD_PRESS_SLOP_PX 24

/* Lives across frames in the caller's loop. `target_ms` is only meaningful
 * while `active`; `down_x`/`down_y` only while `pressing`. */
typedef struct {
    int active;
    uint64_t target_ms;
    int pressing;
    int down_x;
    int down_y;
} VtPlayerGesture;

typedef enum {
    VT_PLAYER_TOUCH_NONE = 0,
    /* The HUD was hidden and the screen was tapped: bring it back. */
    VT_PLAYER_TOUCH_SHOW_HUD,
    VT_PLAYER_TOUCH_TOGGLE_PAUSE,
    /* A drag on the track is in progress; scrub->target_ms follows the
     * finger. Nothing is seeked yet — see below. */
    VT_PLAYER_TOUCH_SCRUBBING,
    /* The drag ended: *seek_ms is where to go, exactly once. */
    VT_PLAYER_TOUCH_SEEK
} VtPlayerTouchAction;

/* One frame of touch input. Call once per frame with the poll's flags, even
 * when they are zero — a drag that receives no event this frame must still
 * report itself as running.
 *
 * `seekable` is the caller's "a seek would be safe and meaningful right now"
 * (a known duration, no quality switch in flight); a drag already running is
 * dropped when it goes false.
 *
 * A seek is reported ONCE, on release. Seeking per move event is not an
 * option: vt_decoder_seek reopens the stream behind a blocking spinner, so a
 * seek per frame would tear playback apart rather than scrub it.
 *
 * Presses are classified here from DOWN and UP rather than from
 * UI_TOUCH_EVENT_TAP. That flag needs the finger down for under 350 ms and
 * moved by under 12 px, which is right for flicking through a list and wrong
 * for a video player: resting a thumb on the pause button for half a second
 * is not a different intention, and on hardware it simply did nothing. */
VtPlayerTouchAction vt_player_touch_step(unsigned flags,
                                         const UiTouchEvent *touch,
                                         int hud_visible, int seekable,
                                         uint64_t duration_ms,
                                         VtPlayerGesture *gesture,
                                         uint64_t *seek_ms);

#endif
