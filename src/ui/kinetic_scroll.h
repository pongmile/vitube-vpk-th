/* ViTube — pixel-based kinetic scrolling for the list screens: the finger
 * drag, the fling that follows it, the rubber band past either end, and the
 * eased scroll that keeps a D-pad selection on screen.
 *
 * Why this exists: the lists used to scroll in whole rows, and only when the
 * finger came up — a vertical drag of any length was read on release as
 * "page up/down by three rows". Nothing moved while the finger did, and then
 * everything moved at once, which on hardware felt like the list ignoring the
 * finger and then jumping. A phone list does the opposite: the content is
 * glued to the finger while it is down, keeps the finger's speed when it
 * lifts, and slows down on its own.
 *
 * Everything here is in screen pixels and microseconds. The offset is a
 * float: a fling decelerates through fractions of a pixel per frame, and
 * rounding it each frame would stall it early or make it step. Drawing rounds
 * once, at the end (vt_kinetic_draw_offset), so every part of a row moves
 * together.
 *
 * Time comes in from the caller (sceKernelGetProcessTimeWide() on the
 * console) and every rate below is per second, never per frame: the list must
 * travel the same distance whether the loop runs at 60 fps or drops to 30
 * while thumbnails decode.
 *
 * No vita2d and no psp2 headers, only the flag names from touch.h: the
 * physics is covered by tests/test_kinetic_scroll.c on the host. */
#ifndef VITUBE_UI_KINETIC_SCROLL_H
#define VITUBE_UI_KINETIC_SCROLL_H

#include <stdint.h>

#include "ui/touch.h"

/* How far the finger may travel before a press becomes a drag. Below this it
 * is still a tap; at or past it the content follows the finger and a tap can
 * no longer happen. One threshold for both on purpose: two (as touch.c's
 * 12 px tap slop next to a separate 36 px swipe distance used to be) leave a
 * band in which a touch is neither, and that band is where a missed tap
 * comes from. 10 px is about the width of a fingertip's wobble on the Vita's
 * panel. */
#define VT_KINETIC_SLOP_PX 10
/* A press held longer than this is not a tap. There is no long-press action
 * on a list, so this is generous; it exists so a finger that came to rest on
 * a row while the user reads can be lifted without opening anything. */
#define VT_KINETIC_TAP_MAX_US 500000ULL

/* Fling deceleration: velocity decays as exp(-t / TAU), so a fling launched
 * at v px/s comes to rest v * TAU px further on. 0.45 s is a little shorter
 * than iOS's "normal" rate (~0.5 s): the Vita's list band is 430 px tall,
 * and a phone-length coast overshoots what the eye can follow on it. */
#define VT_KINETIC_FLING_TAU_S 0.45f
/* Releases slower than this do not fling — the finger stopped and lifted. */
#define VT_KINETIC_MIN_FLING_PX_S 60.0f
/* The panel occasionally reports a jump between two samples; this caps what
 * one bad sample can turn a fling into (~22 rows of coast). */
#define VT_KINETIC_MAX_FLING_PX_S 5000.0f
/* A fling below this speed is over. Low enough that stopping is invisible:
 * at 8 px/s the list moves a quarter of a pixel per frame. */
#define VT_KINETIC_STOP_PX_S 8.0f
/* Touching a list that is still coasting faster than this catches it: the
 * list stops and the touch is not a tap, the way a phone behaves — the user
 * was aiming at the motion, not at whichever row happened to be under it. */
#define VT_KINETIC_CATCH_PX_S 60.0f

/* Release velocity is a least-squares fit over the samples from this last
 * stretch of the drag. One frame-to-frame delta is far too noisy (the panel
 * quantises to whole pixels and the loop samples once per frame), and a
 * longer window remembers a speed the finger already gave up — a drag that
 * slowed to a stop must not launch. */
#define VT_KINETIC_VELOCITY_WINDOW_US 80000ULL
#define VT_KINETIC_SAMPLES 8

/* Time constant of the eased scroll that brings a D-pad selection into view,
 * and of the rubber band springing back. Short: each D-pad repeat (95 ms)
 * retargets the ease, and a long one would leave the selected row hanging
 * off the edge while a direction is held. */
#define VT_KINETIC_EASE_TAU_S 0.05f
#define VT_KINETIC_SPRING_TAU_S 0.08f

/* Pulling past either end moves the content less than the finger, ever less
 * the further it goes (the iOS curve: ~0.55 at first, never more than one
 * viewport in total). It shows the end has been reached without the list
 * appearing stuck. */
#define VT_KINETIC_RUBBER_COEFF 0.55f

/* A gap this long between two updates means the loop was somewhere else —
 * the player, a fetch spinner, another tab. Carrying a fling across it would
 * show up as a jump, so motion is ended instead. */
#define VT_KINETIC_STALL_US 250000ULL

typedef enum {
    VT_KINETIC_IDLE = 0,
    /* Finger down, still inside the slop: may yet be a tap. */
    VT_KINETIC_PRESSED,
    /* Finger down and moving: offset follows it 1:1 (rubber-banded past the
     * ends). */
    VT_KINETIC_DRAGGING,
    /* Finger up, coasting. */
    VT_KINETIC_FLINGING,
    /* Released past an end; easing back onto it. */
    VT_KINETIC_SPRINGING,
    /* Easing towards `target` to show a selection moved by the buttons. */
    VT_KINETIC_EASING
} VtKineticMode;

/* vt_kinetic_take_settled() bits. */
#define VT_KINETIC_SETTLED        1u /* a touch-driven motion came to rest */
#define VT_KINETIC_SETTLED_AT_END 2u /* ...and the user pushed past the end */

typedef struct {
    int y;
    uint64_t t_us;
} VtKineticSample;

/* One per list, living with the list's other state. All-zero is a valid,
 * resting state at the top, so a memset list is a reset list. */
typedef struct {
    float offset;     /* content px scrolled past the viewport top; may be
                         outside [0, max_offset] while rubber-banding */
    float velocity;   /* content px/s, positive = towards the end */
    float max_offset; /* content height - viewport height, never negative */
    float viewport;   /* viewport height in px */
    float target;     /* EASING only */
    VtKineticMode mode;

    int down_x, down_y;     /* viewport-local, where the finger went down */
    uint64_t down_us;
    float raw_at_down;      /* un-rubber-banded offset at touch-down */
    int down_draw_offset;   /* what was on screen when the finger went down */
    int caught;             /* this touch stopped a moving list: not a tap */

    int touch_motion;       /* touch moved the list since the last settle */
    int hit_end;            /* ...and was pushed past the end doing it */
    unsigned settled;       /* pending VT_KINETIC_SETTLED* bits */

    VtKineticSample samples[VT_KINETIC_SAMPLES];
    int sample_count;
    int sample_head;        /* index the next sample is written to */

    uint64_t last_us;       /* time of the previous vt_kinetic_update() */
} VtKineticScroll;

/* Viewport and content height in px. Clamps a resting list that the content
 * shrank under (a reload with fewer rows); a list being dragged or springing
 * is left to finish, since both end on the bounds anyway. */
void vt_kinetic_set_bounds(VtKineticScroll *k, float viewport, float content);

/* One frame of touch input, with the flags straight from ui_touch_poll() —
 * call it every frame the list is on screen, including frames where the
 * flags are 0. (x, y) are relative to the viewport's top-left corner.
 *
 * A touch that goes down outside the viewport is not this list's and is
 * ignored to its release. A frame without any event while a finger was being
 * tracked means touch.c lost the contact (ui_touch_reset() across a screen
 * change, or a failed read); that ends the gesture with no fling and no tap.
 *
 * Returns 1 for a tap and writes *tap_content_y: the content-space y the
 * finger came DOWN on, against what was drawn at that moment — the spot the
 * user aimed at, not wherever the list had eased to by the release. */
int vt_kinetic_touch(VtKineticScroll *k, unsigned flags, int x, int y,
                     uint64_t now_us, int *tap_content_y);

/* Advances the fling / spring / ease to now_us. Call once per frame. */
void vt_kinetic_update(VtKineticScroll *k, uint64_t now_us);

/* Scrolls (eased) just far enough that content [top, bottom) is fully in
 * view; a no-op if it already is. Stops a fling first — a button press is a
 * request for the selection, and coasting on past it would be fighting the
 * user. Ignored while the finger is down: the finger owns the list then. */
void vt_kinetic_ensure_visible(VtKineticScroll *k, float top, float bottom);

/* Moves straight to `offset` (clamped) and stops all motion. */
void vt_kinetic_jump(VtKineticScroll *k, float offset);

/* Returns and clears the pending VT_KINETIC_SETTLED* bits. */
unsigned vt_kinetic_take_settled(VtKineticScroll *k);

/* The finger is on the list (pressed or dragging). */
int vt_kinetic_touching(const VtKineticScroll *k);

/* The offset to draw with, rounded to a whole pixel. */
int vt_kinetic_draw_offset(const VtKineticScroll *k);

#endif
