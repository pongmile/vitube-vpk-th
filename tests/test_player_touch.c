/* Host-side tests for the player's touch gestures (src/ui/player_touch.c).
 * The module is deliberately free of vita2d and psp2 headers so this can run
 * on the build machine: the edges of the scrub mapping, the boundaries
 * between the hit zones, and how a press is told apart from a drag are
 * exactly the things that are invisible until someone is holding the console.
 *
 *   gcc -Isrc -Wall -o /tmp/tpt tests/test_player_touch.c \
 *       src/ui/player_touch.c && /tmp/tpt
 */
#include "../src/ui/player_touch.h"

#include <stdio.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

#define DURATION 600000ULL /* 10 minutes */

static VtPlayerGesture g;
static uint64_t seek;

static UiTouchEvent at(int x, int y) {
    UiTouchEvent e = {0};
    e.x = x;
    e.y = y;
    return e;
}

static VtPlayerTouchAction step(unsigned flags, int x, int y, int hud,
                                int seekable) {
    UiTouchEvent e = at(x, y);
    return vt_player_touch_step(flags, &e, hud, seekable, DURATION, &g, &seek);
}

/* Press and release at (almost) one spot, the way a finger actually behaves:
 * a DOWN frame, some frames of contact, then UP a few pixels away. */
static VtPlayerTouchAction press(int x, int y, int drift, int hud,
                                 int seekable) {
    step(UI_TOUCH_EVENT_DOWN, x, y, hud, seekable);
    step(UI_TOUCH_EVENT_HOLD, x, y, hud, seekable);
    step(UI_TOUCH_EVENT_MOVE, x + drift, y, hud, seekable);
    return step(UI_TOUCH_EVENT_UP, x + drift, y, hud, seekable);
}

int main(void) {
    VtPlayerTouchAction a;

    /* --- track mapping --------------------------------------------------- */
    CHECK(vt_player_touch_track_ms(VT_HUD_TRACK_X, DURATION) == 0,
          "left end of the track is not 0");
    CHECK(vt_player_touch_track_ms(VT_HUD_TRACK_X + VT_HUD_TRACK_W, DURATION)
              == DURATION,
          "right end of the track is not the full duration");
    CHECK(vt_player_touch_track_ms(0, DURATION) == 0, "left of the track");
    CHECK(vt_player_touch_track_ms(VT_SCREEN_W + 500, DURATION) == DURATION,
          "right of the track");
    uint64_t mid = vt_player_touch_track_ms(VT_HUD_TRACK_X + VT_HUD_TRACK_W / 2,
                                            DURATION);
    CHECK(mid > DURATION / 2 - 2000 && mid < DURATION / 2 + 2000,
          "middle of the track is %llu, expected ~%llu",
          (unsigned long long)mid, (unsigned long long)(DURATION / 2));
    CHECK(vt_player_touch_track_ms(VT_HUD_TRACK_X + 10, 0) == 0,
          "zero duration must stay zero");

    /* --- geometry: the zones tile the bar without overlapping ------------ */
    CHECK(VT_HUD_TRACK_HIT_Y == VT_HUD_PLAY_HIT_Y + VT_HUD_PLAY_HIT_H,
          "the track band must start exactly where the button's ends");
    CHECK(VT_HUD_TRACK_HIT_Y + VT_HUD_TRACK_HIT_H <= VT_SCREEN_H,
          "the track band runs off the bottom of the screen");
    CHECK(VT_HUD_TRACK_Y >= VT_HUD_TRACK_HIT_Y &&
          VT_HUD_TRACK_Y + VT_HUD_TRACK_H <= VT_HUD_TRACK_HIT_Y + VT_HUD_TRACK_HIT_H,
          "the drawn track is not inside its own touch band");
    CHECK(VT_HUD_PLAY_Y >= VT_HUD_BAR_Y, "the button is drawn above the bar");
    CHECK(VT_HUD_PLAY_Y >= VT_HUD_PLAY_HIT_Y &&
          VT_HUD_PLAY_Y + VT_HUD_PLAY_SIZE <= VT_HUD_PLAY_HIT_Y + VT_HUD_PLAY_HIT_H,
          "the drawn button sticks out of its own touch target");
    CHECK(VT_HUD_PLAY_X >= VT_HUD_PLAY_HIT_X &&
          VT_HUD_PLAY_X + VT_HUD_PLAY_SIZE <= VT_HUD_PLAY_HIT_X + VT_HUD_PLAY_HIT_W,
          "the drawn button sticks out of its own touch target sideways");

    /* --- the regression this file exists for -----------------------------
     * A press must not depend on UI_TOUCH_EVENT_TAP. That flag needs under
     * 350 ms and under 12 px; on hardware the button and the video-area
     * toggle both did nothing because a thumb on a video overruns both.
     * Every press below is deliberately reported WITHOUT the TAP bit. */
    a = press(VT_HUD_PLAY_HIT_X + 4, VT_HUD_PLAY_HIT_Y + 4, 18, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_TOGGLE_PAUSE,
          "a slow, drifting press on the button must still pause: got %d", a);
    a = press(480, 200, 20, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_TOGGLE_PAUSE,
          "a slow, drifting press on the video must still pause: got %d", a);

    /* The button works even when seeking is off (mid quality switch). */
    a = press(VT_HUD_PLAY_HIT_X + 4, VT_HUD_PLAY_HIT_Y + 4, 0, 1, 0);
    CHECK(a == VT_PLAYER_TOUCH_TOGGLE_PAUSE,
          "play button during a quality switch: got %d", a);

    /* --- a real swipe is not a press ------------------------------------- */
    a = press(480, 200, VT_HUD_PRESS_SLOP_PX + 10, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_NONE, "a swipe must not toggle: got %d", a);

    /* --- hidden HUD: any press just brings it back ------------------------ */
    a = press(480, 200, 2, 0, 1);
    CHECK(a == VT_PLAYER_TOUCH_SHOW_HUD, "hidden HUD + press: got %d", a);
    /* Even on the track: while hidden there is nothing there to aim at. */
    a = step(UI_TOUCH_EVENT_DOWN, 500, VT_HUD_TRACK_Y, 0, 1);
    CHECK(a == VT_PLAYER_TOUCH_NONE && !g.active,
          "hidden HUD must not start a scrub: got %d", a);
    a = step(UI_TOUCH_EVENT_UP, 500, VT_HUD_TRACK_Y, 0, 1);
    CHECK(a == VT_PLAYER_TOUCH_SHOW_HUD, "release should reveal it: got %d", a);

    /* --- the rest of the bar is dead ------------------------------------- */
    a = press(700, VT_HUD_BAR_Y + 4, 0, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_NONE, "press on the hint text: got %d", a);

    /* --- a release with no press behind it is ignored -------------------- */
    a = step(UI_TOUCH_EVENT_UP, VT_HUD_PLAY_HIT_X + 4, VT_HUD_PLAY_HIT_Y + 4,
             1, 1);
    CHECK(a == VT_PLAYER_TOUCH_NONE,
          "a stray release (contact inherited across a screen change) "
          "must do nothing: got %d", a);

    /* --- a full drag: many moves, exactly one seek, on release ----------- */
    int seeks = 0, scrubs = 0;
    a = step(UI_TOUCH_EVENT_DOWN, VT_HUD_TRACK_X + 100,
             VT_HUD_TRACK_HIT_Y + 4, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_SCRUBBING && g.active,
          "press on the track did not start a scrub: got %d", a);
    CHECK(!g.pressing, "a drag must not also count as a press");
    for (int x = VT_HUD_TRACK_X + 100; x < VT_HUD_TRACK_X + 500; x += 20) {
        a = step(UI_TOUCH_EVENT_MOVE, x, VT_HUD_TRACK_HIT_Y + 4, 1, 1);
        if (a == VT_PLAYER_TOUCH_SCRUBBING) scrubs++;
        if (a == VT_PLAYER_TOUCH_SEEK) seeks++;
        CHECK(g.target_ms == vt_player_touch_track_ms(x, DURATION),
              "scrub target did not follow the finger at x=%d", x);
    }
    /* Wandering off the bar mid-drag keeps the gesture alive. */
    a = step(UI_TOUCH_EVENT_MOVE, VT_HUD_TRACK_X + 500, 100, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_SCRUBBING, "drag lost when leaving the bar");
    /* A frame with no event at all still counts as dragging. */
    a = step(0, VT_HUD_TRACK_X + 500, 100, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_SCRUBBING, "drag lost on an idle frame");
    a = step(UI_TOUCH_EVENT_UP, VT_HUD_TRACK_X + 500,
             VT_HUD_TRACK_HIT_Y + 4, 1, 1);
    if (a == VT_PLAYER_TOUCH_SEEK) seeks++;
    CHECK(seeks == 1, "expected exactly one seek from one drag, got %d", seeks);
    CHECK(!g.active, "scrub still active after release");
    CHECK(seek == vt_player_touch_track_ms(VT_HUD_TRACK_X + 500, DURATION),
          "seek target is not where the finger was released");
    printf("drag: %d scrub frames, %d seek\n", scrubs, seeks);

    /* --- a quality switch starting mid-drag cancels it ------------------- */
    step(UI_TOUCH_EVENT_DOWN, VT_HUD_TRACK_X + 50, VT_HUD_TRACK_HIT_Y + 4, 1, 1);
    CHECK(g.active, "scrub did not start");
    a = step(UI_TOUCH_EVENT_MOVE, VT_HUD_TRACK_X + 50, VT_HUD_TRACK_HIT_Y + 4,
             1, 0);
    CHECK(a == VT_PLAYER_TOUCH_NONE && !g.active,
          "an unseekable player must drop the drag: got %d", a);
    a = step(UI_TOUCH_EVENT_UP, VT_HUD_TRACK_X + 50, VT_HUD_TRACK_HIT_Y + 4,
             1, 1);
    CHECK(a != VT_PLAYER_TOUCH_SEEK, "dropped drag still seeked on release");

    /* --- a quick press on the track still seeks there -------------------- */
    a = step(UI_TOUCH_EVENT_DOWN, VT_HUD_TRACK_X + 300,
             VT_HUD_TRACK_HIT_Y + 10, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_SCRUBBING, "press-on-track: no scrub started");
    a = step(UI_TOUCH_EVENT_UP, VT_HUD_TRACK_X + 300,
             VT_HUD_TRACK_HIT_Y + 10, 1, 1);
    CHECK(a == VT_PLAYER_TOUCH_SEEK, "press-on-track did not seek: got %d", a);
    CHECK(seek == vt_player_touch_track_ms(VT_HUD_TRACK_X + 300, DURATION),
          "press-on-track seeked to the wrong place");

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL PLAYER TOUCH TESTS PASSED\n");
    return 0;
}
