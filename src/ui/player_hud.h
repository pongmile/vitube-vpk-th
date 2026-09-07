/* ViTube — player HUD overlay and the quality picker modal. */
#ifndef VITUBE_UI_PLAYER_HUD_H
#define VITUBE_UI_PLAYER_HUD_H

#include <stddef.h>
#include <stdint.h>

#include "media/vita_decoder.h"
#include "yt/model.h"

/* Fills buf with a short label for the given quality: "720p HD" if is_hd,
 * otherwise the direct format's resolution (or itag, if height is
 * unknown). Used both for the on-screen badge and the quality picker. */
void quality_label(int is_hd, const yt_stream_t *stream, int fmt_index,
                   char *buf, size_t cap);

/* Picker entries: index 0 = 720p HD (HLS), 1..n = direct formats. Keeps
 * presenting `player` underneath every frame so browsing the menu doesn't
 * starve the still-playing video either. Returns the chosen entry index,
 * or -1 if cancelled. current_entry is the entry index currently playing. */
int quality_picker_run(VtDecoderPlayer *player, int fill_screen,
                       const yt_stream_t *stream, int current_entry);

/* Draws the title bar, the play/pause button, position/duration + hint bar,
 * progress bar, and (if switching_label is non-NULL) a small transient toast
 * — over whatever the caller already drew this frame. Doesn't clear or
 * present anything itself.
 *
 * scrub_ms is the position the user's finger is pointing at during a drag, or
 * -1 when nothing is being scrubbed; while set, the readout and the knob show
 * it instead of the playback position.
 *
 * is_live drops the progress bar and puts LIVE where the total time goes: a
 * live stream's "duration" is only as much of the window as the demuxer can
 * still see, and its raw position is the encoder's own clock — a bar drawn
 * from those two would be pure fiction. The caller passes the time since the
 * join as position_ms instead, and that is what is printed next to LIVE.
 *
 * quality_pickable picks the key legend: when the Triangle menu is closed for
 * this video (Shorts, live), the hint must not advertise it. */
void draw_hud(const yt_video_t *video, const VtDecoderPlayerStatus *status,
             int volume, const char *backend_name, const char *qlabel,
             int64_t scrub_ms, int is_live, int quality_pickable);

/* The "Wechsle auf ..." banner, with a moving indicator so a switch that
 * takes seconds does not look like a hang. Drawn over whatever is already
 * there, independently of the HUD — a quality switch is now something the
 * user asked for and waits on, not a fallback that happens to them.
 *
 * now_us drives the animation, so it moves at the same speed whatever the
 * decode load does to the frame rate.
 *
 * `animated` asks for the moving indicator. Pass 0 for a switch that is over
 * before anything could move: a block that appears and immediately freezes is
 * worse than no block, since a stopped indicator is what a hang looks like. */
void draw_switch_overlay(const char *label, uint64_t now_us, int animated);

#endif
