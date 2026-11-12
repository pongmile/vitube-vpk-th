/* ViTube — player HUD overlay and the quality picker modal. */
#include "ui/player_hud.h"

#include <stdio.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include "app.h"
#include "ui/player_touch.h"
#include "ui/strings.h"
#include "ui/ui.h"

static void format_time(char *buf, size_t cap, uint64_t ms) {
    unsigned total = (unsigned)(ms / 1000);
    if (total >= 3600)
        snprintf(buf, cap, "%u:%02u:%02u", total / 3600, (total / 60) % 60,
                 total % 60);
    else
        snprintf(buf, cap, "%u:%02u", total / 60, total % 60);
}

/* YouTube names a rendition by its 16:9 tier, not by its pixel height: a 2:1
 * video's 1280x640 rendition IS its "720p", the 640x320 one its "360p".
 * Printing the raw height read as a downgrade to the viewer. */
static unsigned quality_tier(unsigned width, unsigned height) {
    unsigned by_width = width * 9u / 16u;
    return by_width > height ? by_width : height;
}

void quality_label(int is_hd, const yt_stream_t *stream, int fmt_index,
                   char *buf, size_t cap) {
    if (is_hd) {
        snprintf(buf, cap, "720p HD");
    } else if (stream->formats[fmt_index].height > 0) {
        const yt_format_t *f = &stream->formats[fmt_index];
        snprintf(buf, cap, "%up",
                 quality_tier(f->width > 0 ? (unsigned)f->width : 0,
                              (unsigned)f->height));
    } else {
        snprintf(buf, cap, "itag %d", stream->formats[fmt_index].itag);
    }
}

int quality_picker_run(VtDecoderPlayer *player, int fill_screen,
                       const yt_stream_t *stream, int current_entry) {
    /* Entry 0 is HLS 720p, the rest are the direct formats — bounded by
     * YT_FORMATS_MAX, so the label storage can live on the stack. */
    int n_entries = 1 + stream->format_count;
    char labels[1 + YT_FORMATS_MAX][40];
    VtMenuItem items[1 + YT_FORMATS_MAX];
    for (int i = 0; i < n_entries; i++) {
        quality_label(i == 0, stream, i == 0 ? 0 : i - 1, labels[i],
                      sizeof labels[i]);
        items[i].label = labels[i];
        items[i].value = NULL;
        items[i].marked = (i == current_entry);
    }

    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    int cursor = current_entry;
    int result = -1;
    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        if (pressed & SCE_CTRL_CIRCLE) { result = -1; break; }
        if (pressed & SCE_CTRL_CROSS) { result = cursor; break; }
        if (pressed & SCE_CTRL_UP && cursor > 0) cursor--;
        if (pressed & SCE_CTRL_DOWN && cursor < n_entries - 1) cursor++;

        vita2d_start_drawing();
        vita2d_clear_screen();
        /* The video keeps being presented under the menu, so browsing it
         * does not starve playback. */
        vt_decoder_present(player, fill_screen);
        vt_ui_menu_draw(vt_str(S_QUALITY_THIS_VIDEO_ONLY), items, n_entries,
                        cursor);
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vt_decoder_render_complete(player);
        vita2d_swap_buffers();
        sceKernelDelayThread(1000);
    }
    return result;
}

/* Both icons are rectangles rather than glyphs: font.ttf has U+25B6 for play
 * but no pause sign at all — the only one in the bundle is U+23F8 in
 * emoji.ttf, which would draw as a colour emoji beside a flat triangle. */
static void draw_pause_icon(int x, int y, int size, unsigned int color) {
    int bar = size / 4;
    vt_ui_rect(x, y, bar, size, color);
    vt_ui_rect(x + size - bar, y, bar, size, color);
}

static void draw_play_icon(int x, int y, int size, unsigned int color) {
    /* Horizontal slices: zero wide at the top and bottom row, widest in the
     * middle. Two-pixel steps, which the panel does not resolve as a
     * staircase at this size. */
    int width = size * 6 / 7;
    for (int row = 0; row < size; row += 2) {
        int span = row * 2 < size ? row * 2 : (size - row) * 2;
        int w = span * width / size;
        if (w > 0) vt_ui_rect(x, y + row, w, 2, color);
    }
}

/* Pixels from the left end of the track for a position in the video. */
static int track_x_of(uint64_t position_ms, uint64_t duration_ms) {
    if (duration_ms == 0) return 0;
    uint64_t x = (uint64_t)VT_HUD_TRACK_W * position_ms / duration_ms;
    return x > (uint64_t)VT_HUD_TRACK_W ? VT_HUD_TRACK_W : (int)x;
}

/* Track and sliding block are the same shapes vt_ui_run_task's spinner uses,
 * because that is already what "the app is working" looks like in this app.
 * It bounces rather than wrapping: a block that jumps back to the start reads
 * as a stutter on a screen the user is watching for signs of one. */
void draw_switch_overlay(const char *label, uint64_t now_us, int animated) {
    if (!label || !label[0]) return;
    const int bar_w = 150, block_w = 26;
    int tw = vt_ui_text_width(0.85f, label);
    int box_w = (animated && bar_w > tw ? bar_w : tw) + 48;
    int box_x = (VT_SCREEN_W - box_w) / 2;
    vt_ui_rect(box_x, 56, box_w, animated ? 66 : 40, RGBA8(0, 0, 0, 210));
    vt_ui_text((VT_SCREEN_W - tw) / 2, 66, VT_COLOR_ACCENT, 0.85f, label);
    if (!animated) return;

    int track_x = (VT_SCREEN_W - bar_w) / 2, track_y = 100;
    vt_ui_rect(track_x, track_y, bar_w, 4, VT_COLOR_RAISED);
    const uint64_t period = 1600000ULL; /* one there-and-back per 1.6 s */
    uint64_t t = now_us % period, half = period / 2;
    int span = bar_w - block_w;
    int pos = t < half ? (int)(t * (uint64_t)span / half)
                       : span - (int)((t - half) * (uint64_t)span / half);
    vt_ui_rect(track_x + pos, track_y, block_w, 4, VT_COLOR_ACCENT);
}

void draw_hud(const yt_video_t *video, const VtDecoderPlayerStatus *status,
             int volume, const char *backend_name, const char *qlabel,
             int64_t scrub_ms, int is_live, int quality_pickable) {
    vt_ui_rect(0, 0, VT_SCREEN_W, 52, RGBA8(0, 0, 0, 180));
    /* What is actually on screen, not what was asked for. This printed a
     * hardcoded "720p HD" for every HLS source, so a 480p30 live variant and a
     * 720p60 one looked identical here — including to the person trying to
     * report that something is wrong with it. The decoder knows what it
     * negotiated; ask it. */
    char measured[24];
    const char *shown = qlabel;
    if (status->height > 0) {
        unsigned tier = quality_tier(status->width, status->height);
        if (status->fps > 0)
            snprintf(measured, sizeof measured, "%up%d", tier, status->fps);
        else
            snprintf(measured, sizeof measured, "%up", tier);
        shown = measured;
    }
    /* The (△) says where the quality menu is, so it is only there when the
     * menu is: a Short, a live stream or a downloaded file has no such menu
     * and the legend below already leaves the key out for them. */
    char badge[64];
    snprintf(badge, sizeof badge, "%s \xc2\xb7 %s%s \xc2\xb7 Vol %d%%",
             backend_name, shown, quality_pickable ? " (\xe2\x96\xb3)" : "",
             volume);
    float badge_scale = 0.85f;
    int bw = vt_ui_text_width(badge_scale, badge);
    if (bw > 280) { badge_scale = 0.7f; bw = vt_ui_text_width(badge_scale, badge); }
    int title_max_w = VT_SCREEN_W - bw - 40;
    if (title_max_w < 120) title_max_w = 120;
    vt_ui_text_clipped(20, 14, VT_COLOR_TEXT, 1.0f, title_max_w, video->title);
    vt_ui_text(VT_SCREEN_W - 20 - bw, 16, VT_COLOR_MUTED, badge_scale, badge);

    vt_ui_rect(0, VT_HUD_BAR_Y, VT_SCREEN_W, VT_HUD_BAR_H,
               RGBA8(0, 0, 0, 180));
    /* The icon reads as STATE, not as the action the press would take: a play
     * sign means this is playing. The "✕ Pause" hint beside it is a key
     * legend and says the other thing by nature — that pairing is what a
     * transport bar with a button row looks like everywhere else. */
    if (status->paused)
        draw_pause_icon(VT_HUD_PLAY_X, VT_HUD_PLAY_Y, VT_HUD_PLAY_SIZE,
                        VT_COLOR_TEXT);
    else
        draw_play_icon(VT_HUD_PLAY_X, VT_HUD_PLAY_Y, VT_HUD_PLAY_SIZE,
                       VT_COLOR_TEXT);

    /* While scrubbing the readout shows where the finger points, not where
     * playback is, and switches to the accent colour so the two cannot be
     * confused — the video keeps playing underneath during the drag. */
    int scrubbing = !is_live && scrub_ms >= 0 && status->duration_ms > 0;
    uint64_t marker_ms = scrubbing ? (uint64_t)scrub_ms : status->position_ms;
    char position[32], duration[32], line[80];
    format_time(position, sizeof position, marker_ms);
    format_time(duration, sizeof duration, status->duration_ms);
    if (is_live)
        /* Time since this viewer joined (player_screen subtracts the join
         * timestamp — the raw position is the encoder's clock), then LIVE:
         * the one honest pair of numbers a stream with no end has. */
        snprintf(line, sizeof line, "%s \xc2\xb7 %s", position, vt_str(S_LIVE));
    else
        snprintf(line, sizeof line, "%s / %s", position, duration);
    vt_ui_text(VT_HUD_TIME_X, VT_HUD_TEXT_Y,
               scrubbing ? VT_COLOR_ACCENT : VT_COLOR_TEXT, 0.9f, line);
    const char *hint = vt_str(
        status->paused
            ? (quality_pickable ? S_HINT_PLAYER_PAUSED
                                : S_HINT_PLAYER_PAUSED_PLAIN)
            : (quality_pickable ? S_HINT_PLAYER_PLAYING
                                : S_HINT_PLAYER_PLAYING_PLAIN));
    int hw = vt_ui_text_width(0.9f, hint);
    vt_ui_text(VT_SCREEN_W - 20 - hw, VT_HUD_TEXT_Y, VT_COLOR_MUTED, 0.9f,
               hint);

    /* No track at all for live, rather than an empty one: a bar that never
     * fills reads as a stuck download, not as "there is nowhere to go". */
    if (is_live) return;
    vt_ui_rect(VT_HUD_TRACK_X, VT_HUD_TRACK_Y, VT_HUD_TRACK_W, VT_HUD_TRACK_H,
               VT_COLOR_RAISED);
    if (status->duration_ms > 0) {
        int played = track_x_of(status->position_ms, status->duration_ms);
        vt_ui_rect(VT_HUD_TRACK_X, VT_HUD_TRACK_Y, played, VT_HUD_TRACK_H,
                   VT_COLOR_ACCENT);
        int knob = track_x_of(marker_ms, status->duration_ms);
        if (scrubbing)
            /* Taller and full height of the touch band: the thumb covers the
             * track itself, so the part that has to stay visible is the part
             * above and below it. */
            vt_ui_rect(VT_HUD_TRACK_X + knob - 2, VT_HUD_TRACK_Y - 14, 4,
                       VT_HUD_TRACK_H + 28, VT_COLOR_TEXT);
        else
            vt_ui_rect(VT_HUD_TRACK_X + knob - 3, VT_HUD_TRACK_Y - 5, 6, 16,
                       VT_COLOR_TEXT);
    }
}
