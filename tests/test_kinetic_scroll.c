/* Host-side tests for the list screens' kinetic scrolling
 * (src/ui/kinetic_scroll.c). The module is free of vita2d and psp2 headers so
 * it runs on the build machine with a clock the test drives by hand: whether
 * the content stays glued to the finger, where a fling comes to rest, and
 * whether it comes to rest at the same place at 30 fps as at 60 are exactly
 * the things that otherwise only show up as "it feels wrong" on the console.
 *
 *   gcc -Isrc -Wall -Wextra -o /tmp/tks tests/test_kinetic_scroll.c \
 *       src/ui/kinetic_scroll.c -lm && /tmp/tks
 */
#include "../src/ui/kinetic_scroll.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* The real list band: LIST_Y 78 to the hint bar at 508, 96 px rows. */
#define VIEWPORT 430.0f
#define ROW 96.0f

static VtKineticScroll k;
static uint64_t now;
static uint64_t frame_us = 16667; /* 60 fps unless a test says otherwise */

static void reset(int rows) {
    memset(&k, 0, sizeof k);
    now = 1000000; /* not 0: 0 is "never updated" */
    vt_kinetic_set_bounds(&k, VIEWPORT, rows * ROW);
    vt_kinetic_update(&k, now);
}

/* One main-loop frame: the touch event of that frame, then the update, in
 * the order list_screen.c does them. Returns vt_kinetic_touch()'s tap. */
static int frame(unsigned flags, int x, int y, int *tap_y) {
    now += frame_us;
    int tap = vt_kinetic_touch(&k, flags, x, y, now, tap_y);
    vt_kinetic_update(&k, now);
    return tap;
}

static void idle_frames(int n) {
    for (int i = 0; i < n; i++) frame(0, 0, 0, NULL);
}

/* Runs until nothing is animating; returns how long that took in seconds. */
static float run_to_rest(void) {
    uint64_t start = now;
    for (int i = 0; i < 2000 && k.mode != VT_KINETIC_IDLE; i++) idle_frames(1);
    return (float)(now - start) / 1e6f;
}

/* A drag at a constant finger speed (px/s, positive = finger moving down)
 * for `ms`, starting at y0, then lifted. Returns the finger's last y. */
static int drag(int y0, float speed, int ms) {
    frame(UI_TOUCH_EVENT_DOWN, 400, y0, NULL);
    int frames = (int)((uint64_t)ms * 1000 / frame_us);
    int y = y0;
    for (int i = 1; i <= frames; i++) {
        y = y0 + (int)lroundf(speed * (float)(i * frame_us) / 1e6f);
        frame(UI_TOUCH_EVENT_MOVE, 400, y, NULL);
    }
    frame(UI_TOUCH_EVENT_UP, 400, y, NULL);
    return y;
}

int main(void) {
    int tap, tap_y;

    /* --- the content follows the finger 1:1 ------------------------------
     * The regression this file exists for: nothing used to move until the
     * finger lifted. Every move frame past the slop must put the content
     * exactly as far from where it started as the finger is. */
    reset(40);
    vt_kinetic_jump(&k, 500.0f);
    frame(UI_TOUCH_EVENT_DOWN, 400, 300, NULL);
    int glued = 1, worst = 0;
    for (int i = 1; i <= 40; i++) {
        int y = 300 - i * 4; /* finger moving up: content scrolls on */
        frame(UI_TOUCH_EVENT_MOVE, 400, y, NULL);
        if (300 - y < VT_KINETIC_SLOP_PX) {
            CHECK(vt_kinetic_draw_offset(&k) == 500,
                  "content moved inside the slop (finger moved %d)", 300 - y);
            continue;
        }
        int expect = 500 + (300 - y);
        int got = vt_kinetic_draw_offset(&k);
        if (got != expect) {
            glued = 0;
            if (abs(got - expect) > worst) worst = abs(got - expect);
        }
    }
    CHECK(glued, "drag is not 1:1: up to %d px off the finger", worst);
    CHECK(k.mode == VT_KINETIC_DRAGGING, "not dragging: mode %d", k.mode);
    /* Reversing mid-drag tracks just as exactly, and still frames hold. */
    frame(UI_TOUCH_EVENT_MOVE, 400, 250, NULL);
    CHECK(vt_kinetic_draw_offset(&k) == 550, "reverse: offset %d, want 550",
          vt_kinetic_draw_offset(&k));
    frame(UI_TOUCH_EVENT_HOLD, 400, 250, NULL);
    frame(UI_TOUCH_EVENT_HOLD, 400, 250, NULL);
    CHECK(vt_kinetic_draw_offset(&k) == 550, "a still finger moved the list");
    /* Held still for a while and lifted: no fling, it stays put. */
    for (int i = 0; i < 8; i++) frame(UI_TOUCH_EVENT_HOLD, 400, 250, NULL);
    frame(UI_TOUCH_EVENT_UP, 400, 250, NULL);
    CHECK(k.mode == VT_KINETIC_IDLE, "stopped finger flung: v=%.1f",
          k.velocity);
    idle_frames(30);
    CHECK(vt_kinetic_draw_offset(&k) == 550, "list drifted after a still lift");
    CHECK(vt_kinetic_take_settled(&k) == VT_KINETIC_SETTLED,
          "a drag ending must report a settle");
    CHECK(vt_kinetic_take_settled(&k) == 0, "settle is not one-shot");

    /* --- taps ------------------------------------------------------------ */
    reset(40);
    vt_kinetic_jump(&k, 200.0f);
    frame(UI_TOUCH_EVENT_DOWN, 400, 150, NULL);
    frame(UI_TOUCH_EVENT_HOLD, 400, 150, NULL);
    frame(UI_TOUCH_EVENT_MOVE, 404, 155, NULL); /* a fingertip's wobble */
    tap_y = -1;
    tap = frame(UI_TOUCH_EVENT_UP, 404, 155, &tap_y);
    CHECK(tap, "a still, short press is not a tap");
    CHECK(tap_y == 350, "tap content y %d, want 150 + 200", tap_y);
    CHECK(vt_kinetic_draw_offset(&k) == 200, "a tap scrolled the list");
    CHECK(vt_kinetic_take_settled(&k) == 0, "a tap is not a settle");

    /* Past the slop it is a drag, even if the finger comes back. */
    frame(UI_TOUCH_EVENT_DOWN, 400, 150, NULL);
    frame(UI_TOUCH_EVENT_MOVE, 400, 150 + VT_KINETIC_SLOP_PX, NULL);
    frame(UI_TOUCH_EVENT_MOVE, 400, 150, NULL);
    tap = frame(UI_TOUCH_EVENT_UP, 400, 150, NULL);
    CHECK(!tap, "a drag that returned to its start became a tap");
    run_to_rest();
    vt_kinetic_take_settled(&k);

    /* A sideways swipe does not scroll, and is not a tap either. */
    int before = vt_kinetic_draw_offset(&k);
    frame(UI_TOUCH_EVENT_DOWN, 300, 150, NULL);
    for (int i = 1; i <= 10; i++)
        frame(UI_TOUCH_EVENT_MOVE, 300 + i * 20, 150, NULL);
    tap = frame(UI_TOUCH_EVENT_UP, 500, 150, NULL);
    CHECK(!tap, "a sideways swipe became a tap");
    CHECK(vt_kinetic_draw_offset(&k) == before, "a sideways swipe scrolled");

    /* Held past the tap limit: lifting does nothing. */
    frame(UI_TOUCH_EVENT_DOWN, 400, 150, NULL);
    for (int i = 0; i < 40; i++) frame(UI_TOUCH_EVENT_HOLD, 400, 150, NULL);
    tap = frame(UI_TOUCH_EVENT_UP, 400, 150, NULL);
    CHECK(!tap, "a %d ms press counted as a tap",
          (int)(40 * frame_us / 1000));

    /* Touches that start outside the band are not the list's. */
    tap = 0;
    frame(UI_TOUCH_EVENT_DOWN, 400, -10, NULL); /* on the tab bar */
    tap |= frame(UI_TOUCH_EVENT_MOVE, 400, 100, NULL);
    tap |= frame(UI_TOUCH_EVENT_UP, 400, 100, NULL);
    frame(UI_TOUCH_EVENT_DOWN, 400, (int)VIEWPORT, NULL); /* on the hint bar */
    tap |= frame(UI_TOUCH_EVENT_UP, 400, (int)VIEWPORT, NULL);
    CHECK(!tap && k.mode == VT_KINETIC_IDLE &&
              vt_kinetic_draw_offset(&k) == before,
          "a touch outside the viewport reached the list");

    /* touch.c dropping the contact (ui_touch_reset on a screen change)
     * ends the gesture: no tap, no fling. */
    frame(UI_TOUCH_EVENT_DOWN, 400, 150, NULL);
    frame(0, 0, 0, NULL);
    tap = frame(UI_TOUCH_EVENT_UP, 400, 150, NULL);
    CHECK(!tap && k.mode == VT_KINETIC_IDLE, "lost contact still tapped");
    frame(UI_TOUCH_EVENT_DOWN, 400, 300, NULL);
    for (int i = 1; i <= 6; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 300 - i * 30, NULL);
    frame(0, 0, 0, NULL);
    CHECK(k.mode != VT_KINETIC_FLINGING, "lost contact launched a fling");
    run_to_rest();

    /* --- fling: launches at the finger's speed, decays, stops ------------ */
    reset(200);
    vt_kinetic_jump(&k, 2000.0f);
    drag(400, -1500.0f, 150); /* finger up at 1500 px/s */
    CHECK(k.mode == VT_KINETIC_FLINGING, "fast release did not fling");
    /* Read after the release frame's own update, i.e. one frame of decay
     * later than the launch. */
    float launch = k.velocity / expf(-(float)frame_us / 1e6f /
                                     VT_KINETIC_FLING_TAU_S);
    CHECK(fabsf(launch - 1500.0f) < 75.0f,
          "release velocity %.0f px/s, want ~1500", launch);
    float released_at = k.offset, v0 = k.velocity, last_v = k.velocity;
    int monotonic = 1;
    uint64_t start = now;
    while (k.mode == VT_KINETIC_FLINGING && now - start < 10000000) {
        idle_frames(1);
        if (k.velocity > last_v + 1e-3f) monotonic = 0;
        last_v = k.velocity;
    }
    float coast = k.offset - released_at;
    CHECK(monotonic, "fling sped up on its own");
    CHECK(k.mode == VT_KINETIC_IDLE, "fling never stopped");
    CHECK(fabsf(coast - v0 * VT_KINETIC_FLING_TAU_S) < 4.0f,
          "coasted %.1f px, want v0 * tau = %.1f", coast,
          v0 * VT_KINETIC_FLING_TAU_S);
    printf("fling: %.0f px/s -> %.0f px in %.2f s\n", v0, coast,
           (float)(now - start) / 1e6f);
    CHECK(vt_kinetic_take_settled(&k) == VT_KINETIC_SETTLED,
          "fling end did not report a settle");

    /* A drag that slowed to a stop before lifting does not launch. */
    reset(200);
    vt_kinetic_jump(&k, 2000.0f);
    frame(UI_TOUCH_EVENT_DOWN, 400, 400, NULL);
    for (int i = 1; i <= 8; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 400 - i * 25, NULL);
    for (int i = 0; i < 6; i++) frame(UI_TOUCH_EVENT_HOLD, 400, 200, NULL);
    frame(UI_TOUCH_EVENT_UP, 400, 200, NULL);
    CHECK(k.mode == VT_KINETIC_IDLE, "a paused-then-lifted drag flung at %.0f",
          k.velocity);

    /* --- catching a fling stops it, and is not a tap --------------------- */
    reset(200);
    vt_kinetic_jump(&k, 2000.0f);
    drag(400, -2000.0f, 120);
    idle_frames(5);
    CHECK(k.mode == VT_KINETIC_FLINGING, "no fling to catch");
    vt_kinetic_take_settled(&k);
    frame(UI_TOUCH_EVENT_DOWN, 400, 200, NULL);
    float caught_at = k.offset;
    frame(UI_TOUCH_EVENT_HOLD, 400, 200, NULL);
    CHECK(k.offset == caught_at, "the list kept moving under the finger");
    tap = frame(UI_TOUCH_EVENT_UP, 400, 200, NULL);
    CHECK(!tap, "catching a fling opened a row");
    CHECK(vt_kinetic_take_settled(&k) & VT_KINETIC_SETTLED,
          "a caught fling must report a settle (the selection is stale)");
    /* Once still, the next touch is a tap again. */
    frame(UI_TOUCH_EVENT_DOWN, 400, 200, NULL);
    tap = frame(UI_TOUCH_EVENT_UP, 400, 200, NULL);
    CHECK(tap, "a tap on a list at rest after a catch did not register");

    /* --- bounds ---------------------------------------------------------- */
    /* Flinging into the end stops on it and says it was pushed there. */
    reset(20); /* max offset 20 * 96 - 430 = 1490 */
    vt_kinetic_jump(&k, 1300.0f);
    drag(400, -3000.0f, 60);
    run_to_rest();
    CHECK(k.offset == k.max_offset, "fling past the end rests at %.1f, not %.1f",
          k.offset, k.max_offset);
    CHECK(vt_kinetic_take_settled(&k) ==
              (VT_KINETIC_SETTLED | VT_KINETIC_SETTLED_AT_END),
          "hitting the end was not reported as such");
    /* ...and into the start stops on 0, not reported as the end. */
    vt_kinetic_jump(&k, 200.0f);
    drag(100, 3000.0f, 60);
    run_to_rest();
    CHECK(k.offset == 0.0f, "fling past the start rests at %.1f", k.offset);
    CHECK(vt_kinetic_take_settled(&k) == VT_KINETIC_SETTLED,
          "the start was reported as the end");

    /* Pulling past the start: the content follows, but less than the
     * finger, and springs back exactly onto 0. */
    reset(20);
    frame(UI_TOUCH_EVENT_DOWN, 400, 50, NULL);
    for (int i = 1; i <= 20; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 50 + i * 10, NULL);
    CHECK(k.offset < 0.0f && k.offset > -200.0f,
          "200 px pull past the start shows %.1f", k.offset);
    float pulled = k.offset;
    frame(UI_TOUCH_EVENT_MOVE, 400, 260, NULL);
    CHECK(k.offset < pulled, "rubber band does not follow the finger at all");
    frame(UI_TOUCH_EVENT_UP, 400, 260, NULL);
    CHECK(k.mode == VT_KINETIC_SPRINGING, "release past the start: mode %d",
          k.mode);
    float t = run_to_rest();
    CHECK(k.offset == 0.0f, "spring rests at %.2f, not exactly 0", k.offset);
    CHECK(t < 1.0f, "spring took %.2f s", t);
    CHECK(vt_kinetic_take_settled(&k) == VT_KINETIC_SETTLED,
          "spring back did not report a settle");

    /* Same at the end, and that one counts as pushing past it. */
    vt_kinetic_jump(&k, k.max_offset);
    frame(UI_TOUCH_EVENT_DOWN, 400, 400, NULL);
    for (int i = 1; i <= 10; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 400 - i * 10, NULL);
    CHECK(k.offset > k.max_offset, "no rubber band at the end");
    frame(UI_TOUCH_EVENT_UP, 400, 300, NULL);
    run_to_rest();
    CHECK(k.offset == k.max_offset, "spring at the end rests at %.2f",
          k.offset);
    CHECK(vt_kinetic_take_settled(&k) ==
              (VT_KINETIC_SETTLED | VT_KINETIC_SETTLED_AT_END),
          "a pull past the end must read as one (load more)");

    /* Past the end and then all the way back, flung towards the start: it
     * comes to rest at the top, and that is not the end. */
    vt_kinetic_jump(&k, k.max_offset);
    frame(UI_TOUCH_EVENT_DOWN, 400, 300, NULL);
    for (int i = 1; i <= 5; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 300 - i * 10, NULL);
    CHECK(k.offset > k.max_offset, "no pull past the end");
    for (int i = 1; i <= 12; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 250 + i * 30, NULL);
    frame(UI_TOUCH_EVENT_UP, 400, 610, NULL);
    CHECK(k.mode == VT_KINETIC_FLINGING && k.velocity < 0.0f,
          "no fling towards the start: mode %d v %.1f", k.mode, k.velocity);
    run_to_rest();
    CHECK(vt_kinetic_take_settled(&k) == VT_KINETIC_SETTLED,
          "a pull past the end that was taken back still read as the end");

    /* Catching the spring mid-way picks the content up where it is drawn. */
    vt_kinetic_jump(&k, 0.0f);
    frame(UI_TOUCH_EVENT_DOWN, 400, 50, NULL);
    for (int i = 1; i <= 15; i++) frame(UI_TOUCH_EVENT_MOVE, 400, 50 + i * 10, NULL);
    frame(UI_TOUCH_EVENT_UP, 400, 200, NULL);
    idle_frames(3);
    int shown = vt_kinetic_draw_offset(&k);
    frame(UI_TOUCH_EVENT_DOWN, 400, 200, NULL);
    /* 10 px further down: the content must go on from where it was drawn,
     * by a little less than 10 px (the band resists). Treating the drawn
     * offset as an unbanded one would snap it most of the way back. */
    frame(UI_TOUCH_EVENT_MOVE, 400, 200 + VT_KINETIC_SLOP_PX, NULL);
    int now_shown = vt_kinetic_draw_offset(&k);
    CHECK(now_shown <= shown && now_shown >= shown - VT_KINETIC_SLOP_PX - 1,
          "catching the spring jumped from %d to %d", shown, now_shown);
    frame(UI_TOUCH_EVENT_UP, 400, 200 + VT_KINETIC_SLOP_PX, NULL);
    run_to_rest();
    vt_kinetic_take_settled(&k);

    /* A short list (fits on screen) has nowhere to go but still rubber-bands
     * and settles cleanly. */
    reset(3);
    CHECK(k.max_offset == 0.0f, "3 rows overflow a %.0f px viewport", VIEWPORT);
    drag(300, -2000.0f, 80);
    run_to_rest();
    CHECK(k.offset == 0.0f, "short list rests at %.2f", k.offset);

    /* Content shrinking under a resting list clamps it. */
    reset(40);
    vt_kinetic_jump(&k, 3000.0f);
    vt_kinetic_set_bounds(&k, VIEWPORT, 10 * ROW);
    CHECK(k.offset == 10 * ROW - VIEWPORT, "shrunk list rests at %.1f",
          k.offset);

    /* --- frame-time independence ----------------------------------------
     * The same fling, stepped at 60 and at 30 fps, must be in the same place
     * at the same moments and come to rest in the same place. */
    float at_half[2], at_rest[2];
    uint64_t rates[2] = { 16667, 33333 };
    for (int r = 0; r < 2; r++) {
        frame_us = rates[r];
        reset(400);
        vt_kinetic_jump(&k, 1000.0f);
        k.mode = VT_KINETIC_FLINGING;
        k.velocity = 2500.0f;
        /* 0.5 s is 30 frames of 16.667 ms and 15 of 33.333 ms. */
        idle_frames((int)((500000 + frame_us / 2) / frame_us));
        at_half[r] = k.offset;
        run_to_rest();
        at_rest[r] = k.offset;
    }
    frame_us = 16667;
    CHECK(fabsf(at_half[0] - at_half[1]) < 0.5f,
          "at 0.5 s: %.2f px at 60 fps vs %.2f px at 30 fps", at_half[0],
          at_half[1]);
    CHECK(fabsf(at_rest[0] - at_rest[1]) < 1.0f,
          "rest: %.2f px at 60 fps vs %.2f px at 30 fps", at_rest[0],
          at_rest[1]);
    printf("60 vs 30 fps: %.2f / %.2f px at 0.5 s, %.2f / %.2f px at rest\n",
           at_half[0], at_half[1], at_rest[0], at_rest[1]);

    /* A whole gesture at 30 fps (half the samples) launches about the same. */
    float launched[2];
    for (int r = 0; r < 2; r++) {
        frame_us = rates[r];
        reset(400);
        vt_kinetic_jump(&k, 5000.0f);
        drag(400, -1800.0f, 133);
        /* Undo the release frame's decay, which differs with the frame. */
        launched[r] = k.velocity / expf(-(float)frame_us / 1e6f /
                                        VT_KINETIC_FLING_TAU_S);
    }
    frame_us = 16667;
    CHECK(fabsf(launched[0] - launched[1]) < 0.05f * launched[0],
          "launch %.0f px/s at 60 fps vs %.0f at 30 fps", launched[0],
          launched[1]);

    /* --- buttons: ease the selection into view ------------------------- */
    reset(40);
    /* Row 10 = [960, 1056): bottom-aligned that is 1056 - 430 = 626. */
    vt_kinetic_ensure_visible(&k, 10 * ROW, 11 * ROW);
    CHECK(k.mode == VT_KINETIC_EASING && k.target == 11 * ROW - VIEWPORT,
          "ease target %.1f, want %.1f", k.target, 11 * ROW - VIEWPORT);
    idle_frames(1);
    CHECK(k.offset > 0.0f && k.offset < k.target,
          "an ease is a jump: %.1f after one frame", k.offset);
    t = run_to_rest();
    CHECK(k.offset == 11 * ROW - VIEWPORT && t < 0.5f,
          "ease rests at %.1f after %.2f s", k.offset, t);
    CHECK(vt_kinetic_take_settled(&k) == 0, "a button scroll is not a settle");
    /* Already visible: nothing moves. */
    vt_kinetic_ensure_visible(&k, 8 * ROW, 9 * ROW);
    CHECK(k.mode == VT_KINETIC_IDLE, "visible row scrolled the list");
    /* Up past the top edge aligns the row to the top. */
    vt_kinetic_ensure_visible(&k, 2 * ROW, 3 * ROW);
    run_to_rest();
    CHECK(k.offset == 2 * ROW, "ease up rests at %.1f", k.offset);
    /* Held direction: Down on rows 4..8 with a repeat every two frames,
     * long before each ease lands, still ends with row 8 bottom-aligned. */
    vt_kinetic_jump(&k, 0.0f);
    for (int row = 4; row <= 8; row++) {
        vt_kinetic_ensure_visible(&k, row * ROW, (row + 1) * ROW);
        idle_frames(2);
    }
    run_to_rest();
    CHECK(k.offset == 9 * ROW - VIEWPORT, "held-down ease rests at %.1f",
          k.offset);

    /* The finger owns the list: no ease while it is down. */
    frame(UI_TOUCH_EVENT_DOWN, 400, 200, NULL);
    frame(UI_TOUCH_EVENT_MOVE, 400, 150, NULL);
    float held = k.offset;
    vt_kinetic_ensure_visible(&k, 30 * ROW, 31 * ROW);
    frame(UI_TOUCH_EVENT_MOVE, 400, 150, NULL);
    CHECK(k.offset == held && k.mode == VT_KINETIC_DRAGGING,
          "the selection fought the finger");
    frame(UI_TOUCH_EVENT_UP, 400, 150, NULL);
    run_to_rest();
    vt_kinetic_take_settled(&k);

    /* A button during a fling stops it, without the settle that would have
     * the owner move the selection the buttons just chose. */
    vt_kinetic_jump(&k, 1000.0f);
    drag(400, -2000.0f, 100);
    idle_frames(3);
    vt_kinetic_take_settled(&k);
    CHECK(k.mode == VT_KINETIC_FLINGING, "no fling to interrupt");
    float flung_to = k.offset;
    vt_kinetic_ensure_visible(&k, flung_to + 100, flung_to + 196);
    idle_frames(10);
    CHECK(k.offset == flung_to, "the fling carried on past a button press");
    CHECK(vt_kinetic_take_settled(&k) == 0,
          "a button-stopped fling reported a settle");

    /* --- a stall (player, spinner) ends motion instead of jumping ------- */
    reset(200);
    vt_kinetic_jump(&k, 1000.0f);
    drag(400, -2000.0f, 100);
    float stalled_at = k.offset;
    now += 3000000; /* three seconds in the player */
    vt_kinetic_update(&k, now);
    CHECK(k.mode == VT_KINETIC_IDLE && k.offset == stalled_at,
          "a stall moved the list from %.1f to %.1f", stalled_at, k.offset);

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL KINETIC SCROLL TESTS PASSED\n");
    return 0;
}
