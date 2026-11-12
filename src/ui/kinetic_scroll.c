/* ViTube — kinetic list scrolling. See kinetic_scroll.h. */
#include "ui/kinetic_scroll.h"

#include <math.h>

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- rubber band ---------------------------------------------------------
 * `over` is how far the finger has gone past an end; the result is how far
 * the content goes. inv_band() undoes it, so that a finger landing on a list
 * that is still springing back picks it up exactly where it is drawn instead
 * of making it jump to where the unbanded drag would put it. */

static float band(float over, float viewport) {
    if (over <= 0.0f || viewport <= 0.0f) return over;
    return (1.0f - 1.0f / (over * VT_KINETIC_RUBBER_COEFF / viewport + 1.0f)) *
           viewport;
}

static float inv_band(float shown, float viewport) {
    if (shown <= 0.0f || viewport <= 0.0f) return shown;
    /* band() tends to `viewport` but never reaches it. */
    if (shown >= viewport * 0.999f) shown = viewport * 0.999f;
    return viewport / VT_KINETIC_RUBBER_COEFF *
           (1.0f / (1.0f - shown / viewport) - 1.0f);
}

/* Offset to show for an unbanded drag position. Inside the bounds this is
 * the identity: the content is exactly where the finger puts it. */
static float banded(const VtKineticScroll *k, float raw) {
    if (raw < 0.0f) return -band(-raw, k->viewport);
    if (raw > k->max_offset)
        return k->max_offset + band(raw - k->max_offset, k->viewport);
    return raw;
}

static float unbanded(const VtKineticScroll *k, float shown) {
    if (shown < 0.0f) return -inv_band(-shown, k->viewport);
    if (shown > k->max_offset)
        return k->max_offset + inv_band(shown - k->max_offset, k->viewport);
    return shown;
}

static int out_of_bounds(const VtKineticScroll *k) {
    return k->offset < 0.0f || k->offset > k->max_offset;
}

/* --- velocity ------------------------------------------------------------ */

static void push_sample(VtKineticScroll *k, int y, uint64_t t_us) {
    k->samples[k->sample_head].y = y;
    k->samples[k->sample_head].t_us = t_us;
    k->sample_head = (k->sample_head + 1) % VT_KINETIC_SAMPLES;
    if (k->sample_count < VT_KINETIC_SAMPLES) k->sample_count++;
}

/* Finger velocity in px/s: the slope of a least-squares line through the
 * samples of the last VT_KINETIC_VELOCITY_WINDOW_US. A fit rather than
 * first-to-last, because the two end samples are exactly the noisy ones —
 * the panel's last report before lift-off is often a pixel or two off the
 * line the finger was on. Samples are taken once per frame and include the
 * still frames (HOLD), so a finger that stopped before lifting fits a flat
 * line and does not launch anything. */
static float finger_velocity(const VtKineticScroll *k) {
    if (k->sample_count < 2) return 0.0f;
    int newest = (k->sample_head + VT_KINETIC_SAMPLES - 1) % VT_KINETIC_SAMPLES;
    uint64_t t_end = k->samples[newest].t_us;
    double n = 0, st = 0, sy = 0, stt = 0, sty = 0;
    for (int i = 0; i < k->sample_count; i++) {
        int idx = (newest + VT_KINETIC_SAMPLES - i) % VT_KINETIC_SAMPLES;
        const VtKineticSample *s = &k->samples[idx];
        if (t_end - s->t_us > VT_KINETIC_VELOCITY_WINDOW_US) break;
        /* Relative to the newest sample, in seconds: absolute microsecond
         * timestamps squared would lose everything to rounding. */
        double t = -(double)(t_end - s->t_us) / 1e6;
        n += 1;
        st += t;
        sy += s->y;
        stt += t * t;
        sty += t * s->y;
    }
    double denom = n * stt - st * st;
    /* Fewer than two distinct times (two reports in one frame): no line. */
    if (n < 2 || denom < 1e-9) return 0.0f;
    return (float)((n * sty - st * sy) / denom);
}

/* --- motion -------------------------------------------------------------- */

/* Ends whatever touch-driven motion was running and records that it did, so
 * the owner can tidy up once the list is still (move its selection on
 * screen, load the next page). */
static void settle(VtKineticScroll *k) {
    k->mode = VT_KINETIC_IDLE;
    k->velocity = 0.0f;
    if (k->touch_motion) {
        k->settled |= VT_KINETIC_SETTLED;
        if (k->hit_end) k->settled |= VT_KINETIC_SETTLED_AT_END;
    }
    k->touch_motion = 0;
    k->hit_end = 0;
}

/* The finger has left a list that was being dragged (or is no longer being
 * tracked). Past an end it springs back and never flings: a fling would only
 * run further out. */
static void release(VtKineticScroll *k, float content_velocity) {
    if (out_of_bounds(k)) {
        k->mode = VT_KINETIC_SPRINGING;
        k->velocity = 0.0f;
        return;
    }
    if (fabsf(content_velocity) >= VT_KINETIC_MIN_FLING_PX_S) {
        k->mode = VT_KINETIC_FLINGING;
        k->velocity = clampf(content_velocity, -VT_KINETIC_MAX_FLING_PX_S,
                             VT_KINETIC_MAX_FLING_PX_S);
        return;
    }
    settle(k);
}

void vt_kinetic_set_bounds(VtKineticScroll *k, float viewport, float content) {
    k->viewport = viewport > 0.0f ? viewport : 0.0f;
    float max = content - viewport;
    k->max_offset = max > 0.0f ? max : 0.0f;
    switch (k->mode) {
    case VT_KINETIC_IDLE:
        k->offset = clampf(k->offset, 0.0f, k->max_offset);
        break;
    case VT_KINETIC_EASING:
        k->target = clampf(k->target, 0.0f, k->max_offset);
        break;
    default:
        /* Pressed/dragging: the finger decides. Flinging and springing both
         * stop on the bounds by themselves. */
        break;
    }
}

int vt_kinetic_touch(VtKineticScroll *k, unsigned flags, int x, int y,
                     uint64_t now_us, int *tap_content_y) {
    if (flags & UI_TOUCH_EVENT_DOWN) {
        if (y < 0 || (float)y >= k->viewport) return 0; /* not on the list */
        /* Catching a list in motion stops it where it is drawn. Easing
         * (a D-pad scroll) is not caught: it is short, it was not started by
         * the finger, and a tap during it is still meant for a row. */
        k->caught = k->mode == VT_KINETIC_SPRINGING ||
                    (k->mode == VT_KINETIC_FLINGING &&
                     fabsf(k->velocity) > VT_KINETIC_CATCH_PX_S);
        if (k->mode == VT_KINETIC_FLINGING || k->mode == VT_KINETIC_SPRINGING)
            k->touch_motion = 1; /* stopping it is the motion being settled */
        k->mode = VT_KINETIC_PRESSED;
        k->velocity = 0.0f;
        k->raw_at_down = unbanded(k, k->offset);
        k->down_draw_offset = vt_kinetic_draw_offset(k);
        k->down_x = x;
        k->down_y = y;
        k->down_us = now_us;
        k->sample_count = 0;
        k->sample_head = 0;
        push_sample(k, y, now_us);
        return 0;
    }
    if (k->mode != VT_KINETIC_PRESSED && k->mode != VT_KINETIC_DRAGGING)
        return 0;

    if (flags & UI_TOUCH_EVENT_UP) {
        /* No sample for the release itself: touch.c reports it with the last
         * contact's coordinates one frame later, and that extra still sample
         * would read as the finger having stopped. */
        if (k->mode == VT_KINETIC_PRESSED) {
            int tap = !k->caught && now_us - k->down_us <= VT_KINETIC_TAP_MAX_US;
            if (out_of_bounds(k)) k->mode = VT_KINETIC_SPRINGING;
            else settle(k);
            if (tap && tap_content_y)
                *tap_content_y = k->down_y + k->down_draw_offset;
            return tap;
        }
        /* The finger moving down drags the content down, i.e. towards the
         * start: content velocity is the finger's, negated. */
        release(k, -finger_velocity(k));
        return 0;
    }

    if (!(flags & (UI_TOUCH_EVENT_MOVE | UI_TOUCH_EVENT_HOLD))) {
        /* touch.c reports MOVE or HOLD on every frame a contact is held, so
         * silence here means it dropped the contact. Nothing about where the
         * finger went is known: no fling, no tap. */
        release(k, 0.0f);
        return 0;
    }

    push_sample(k, y, now_us);
    if (k->mode == VT_KINETIC_PRESSED) {
        int dx = x - k->down_x, dy = y - k->down_y;
        if (dx * dx + dy * dy < VT_KINETIC_SLOP_PX * VT_KINETIC_SLOP_PX)
            return 0;
        k->mode = VT_KINETIC_DRAGGING;
        k->touch_motion = 1;
    }
    /* Anchored to the touch-down point rather than accumulated per frame:
     * the content row that was under the finger stays under it for the
     * whole drag, with nothing to drift. Only the vertical component counts
     * — the lists have no horizontal gesture, so a diagonal drag scrolls by
     * how far it went down or up, and a sideways one does nothing at all
     * (but is no longer a tap either). The slop is not subtracted: the
     * content catches up with the finger on the frame the drag starts,
     * which is a few pixels while the finger is already moving, and from
     * then on it is glued to it. */
    float raw = k->raw_at_down + (float)(k->down_y - y);
    if (raw > k->max_offset) k->hit_end = 1;
    k->offset = banded(k, raw);
    return 0;
}

void vt_kinetic_update(VtKineticScroll *k, uint64_t now_us) {
    uint64_t elapsed = k->last_us && now_us > k->last_us ? now_us - k->last_us
                                                         : 0;
    int first = !k->last_us;
    k->last_us = now_us;
    if (k->mode == VT_KINETIC_IDLE || k->mode == VT_KINETIC_PRESSED ||
        k->mode == VT_KINETIC_DRAGGING)
        return;

    if (first || elapsed > VT_KINETIC_STALL_US) {
        /* See VT_KINETIC_STALL_US. End where the motion was heading for the
         * two that have a destination, and where it is for the fling. */
        if (k->mode == VT_KINETIC_EASING) k->offset = k->target;
        k->offset = clampf(k->offset, 0.0f, k->max_offset);
        if (k->mode == VT_KINETIC_EASING) k->mode = VT_KINETIC_IDLE;
        else settle(k);
        return;
    }
    float dt = (float)elapsed / 1e6f;

    switch (k->mode) {
    case VT_KINETIC_FLINGING: {
        /* Exact integral of v(t) = v0 * exp(-t / TAU) over this frame,
         * not offset += v * dt: the closed form gives the same path for any
         * frame length, so 30 and 60 fps coast to the same row. */
        float decay = expf(-dt / VT_KINETIC_FLING_TAU_S);
        k->offset += k->velocity * VT_KINETIC_FLING_TAU_S * (1.0f - decay);
        k->velocity *= decay;
        /* Hitting an end stops dead rather than bouncing: it settles on the
         * same frame, with nothing left to animate. */
        if (k->offset >= k->max_offset) {
            if (k->velocity > 0.0f) k->hit_end = 1;
            k->offset = k->max_offset;
            settle(k);
        } else if (k->offset <= 0.0f) {
            k->offset = 0.0f;
            settle(k);
        } else if (fabsf(k->velocity) < VT_KINETIC_STOP_PX_S) {
            settle(k);
        }
        break;
    }
    case VT_KINETIC_SPRINGING: {
        float bound = clampf(k->offset, 0.0f, k->max_offset);
        if (k->offset > bound) k->hit_end = 1;
        k->offset = bound + (k->offset - bound) *
                                expf(-dt / VT_KINETIC_SPRING_TAU_S);
        if (fabsf(k->offset - bound) < 0.5f) {
            k->offset = bound;
            settle(k);
        }
        break;
    }
    case VT_KINETIC_EASING:
        k->offset = k->target + (k->offset - k->target) *
                                    expf(-dt / VT_KINETIC_EASE_TAU_S);
        if (fabsf(k->offset - k->target) < 0.5f) {
            k->offset = k->target;
            k->mode = VT_KINETIC_IDLE;
        }
        break;
    default:
        break;
    }
}

void vt_kinetic_ensure_visible(VtKineticScroll *k, float top, float bottom) {
    if (vt_kinetic_touching(k)) return;
    /* Measured from where an ease already running is going, not from where
     * it is now: with a direction held, the next repeat arrives before the
     * previous ease has landed, and measuring from the lagging position
     * would scroll by less than a row each time. */
    float base = k->mode == VT_KINETIC_EASING ? k->target : k->offset;
    if (k->mode == VT_KINETIC_FLINGING || k->mode == VT_KINETIC_SPRINGING) {
        /* Stopped without reporting a settle: the owner tidies its selection
         * up on a settle, and here the selection is exactly what the buttons
         * just chose. */
        base = clampf(k->offset, 0.0f, k->max_offset);
        k->offset = base;
        k->mode = VT_KINETIC_IDLE;
        k->touch_motion = 0;
        k->hit_end = 0;
    }
    float target = base;
    if (top < base) target = top;
    else if (bottom > base + k->viewport) target = bottom - k->viewport;
    target = clampf(target, 0.0f, k->max_offset);
    if (target == k->offset) {
        if (k->mode == VT_KINETIC_EASING) k->mode = VT_KINETIC_IDLE;
        return;
    }
    k->target = target;
    k->velocity = 0.0f;
    k->mode = VT_KINETIC_EASING;
}

void vt_kinetic_jump(VtKineticScroll *k, float offset) {
    k->offset = clampf(offset, 0.0f, k->max_offset);
    k->velocity = 0.0f;
    k->mode = VT_KINETIC_IDLE;
    k->touch_motion = 0;
    k->hit_end = 0;
}

unsigned vt_kinetic_take_settled(VtKineticScroll *k) {
    unsigned bits = k->settled;
    k->settled = 0;
    return bits;
}

int vt_kinetic_touching(const VtKineticScroll *k) {
    return k->mode == VT_KINETIC_PRESSED || k->mode == VT_KINETIC_DRAGGING;
}

int vt_kinetic_draw_offset(const VtKineticScroll *k) {
    return (int)floorf(k->offset + 0.5f);
}
