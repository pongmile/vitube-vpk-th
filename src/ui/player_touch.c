/* ViTube — player touch gestures. See player_touch.h. */
#include "ui/player_touch.h"

/* Local rather than ui_touch_hit_rect(): that one lives in touch.c behind
 * psp2/touch.h, and keeping this file free of Vita headers is what lets the
 * host test compile it. Same convention — left/top inclusive, right/bottom
 * exclusive, so adjacent rectangles do not overlap. */
static int inside(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

uint64_t vt_player_touch_track_ms(int x, uint64_t duration_ms) {
    int rel = x - VT_HUD_TRACK_X;
    if (rel < 0) rel = 0;
    if (rel > VT_HUD_TRACK_W) rel = VT_HUD_TRACK_W;
    return (uint64_t)rel * duration_ms / VT_HUD_TRACK_W;
}

VtPlayerTouchAction vt_player_touch_step(unsigned flags,
                                         const UiTouchEvent *touch,
                                         int hud_visible, int seekable,
                                         uint64_t duration_ms,
                                         VtPlayerGesture *gesture,
                                         uint64_t *seek_ms) {
    if (gesture->active) {
        /* A quality switch started under the drag (or the duration went
         * away): drop it silently rather than seek into a stream that is
         * being replaced. */
        if (!seekable) {
            gesture->active = 0;
            return VT_PLAYER_TOUCH_NONE;
        }
        /* The drag owns every event until the finger lifts, wherever it has
         * wandered to — including off the track and out of the bar. Letting
         * it go on leaving the band would strand the gesture mid-swipe, which
         * is precisely when a thumb drifts. */
        if (flags & UI_TOUCH_EVENT_UP) {
            gesture->active = 0;
            *seek_ms = vt_player_touch_track_ms(touch->x, duration_ms);
            return VT_PLAYER_TOUCH_SEEK;
        }
        if (flags & (UI_TOUCH_EVENT_MOVE | UI_TOUCH_EVENT_HOLD |
                     UI_TOUCH_EVENT_DOWN))
            gesture->target_ms = vt_player_touch_track_ms(touch->x, duration_ms);
        return VT_PLAYER_TOUCH_SCRUBBING;
    }

    if (flags & UI_TOUCH_EVENT_DOWN) {
        /* Starting a drag needs the bar on screen: the track cannot be aimed
         * at while it is invisible, and a blind press there is the press that
         * was meant to reveal the HUD. */
        if (hud_visible && seekable &&
            inside(touch->x, touch->y, VT_HUD_TRACK_X, VT_HUD_TRACK_HIT_Y,
                   VT_HUD_TRACK_W, VT_HUD_TRACK_HIT_H)) {
            gesture->active = 1;
            gesture->pressing = 0;
            gesture->target_ms = vt_player_touch_track_ms(touch->x,
                                                          duration_ms);
            return VT_PLAYER_TOUCH_SCRUBBING;
        }
        /* Anything else: remember where it started and decide on release. */
        gesture->pressing = 1;
        gesture->down_x = touch->x;
        gesture->down_y = touch->y;
        return VT_PLAYER_TOUCH_NONE;
    }

    if (flags & UI_TOUCH_EVENT_UP) {
        int was_pressing = gesture->pressing;
        gesture->pressing = 0;
        if (!was_pressing) return VT_PLAYER_TOUCH_NONE;
        int dx = touch->x - gesture->down_x;
        int dy = touch->y - gesture->down_y;
        if (dx * dx + dy * dy >
            VT_HUD_PRESS_SLOP_PX * VT_HUD_PRESS_SLOP_PX)
            return VT_PLAYER_TOUCH_NONE; /* a swipe, not a press */
        /* Classified by where the finger went DOWN, not where it came up:
         * that is the spot the user aimed at, and it is the one the slop
         * above is measured from. No time limit — holding the pause button
         * for a moment means the same thing as stabbing at it. */
        if (!hud_visible) return VT_PLAYER_TOUCH_SHOW_HUD;
        if (inside(gesture->down_x, gesture->down_y, VT_HUD_PLAY_HIT_X,
                   VT_HUD_PLAY_HIT_Y, VT_HUD_PLAY_HIT_W, VT_HUD_PLAY_HIT_H))
            return VT_PLAYER_TOUCH_TOGGLE_PAUSE;
        /* Everything else inside the bar is deliberately dead. The bar also
         * carries the elapsed/total readout and the button hints, and before
         * the zones were split a tap on that text seeked. */
        if (gesture->down_y >= VT_HUD_BAR_Y) return VT_PLAYER_TOUCH_NONE;
        /* On the video itself, with the controls up: play/pause, the way a
         * phone player behaves. The bar goes away on its own afterwards. */
        return VT_PLAYER_TOUCH_TOGGLE_PAUSE;
    }
    return VT_PLAYER_TOUCH_NONE;
}
