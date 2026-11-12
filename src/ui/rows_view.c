/* ViTube — stateless row/empty-state rendering for list screens. */
#include "ui/rows_view.h"

#include "app.h"
#include "ui/downloads_screen.h"
#include "ui/thumbs.h"
#include "ui/strings.h"
#include "ui/ui.h"

#define ROW_H 96
#define CHANNEL_ROW_H 72
#define THUMB_W 152
#define THUMB_H 85
#define AVATAR_SIZE 48

/* Row text is clipped to its column, like vt_ui_text_clipped() does — but
 * that helper turns clipping OFF when it is done, and from then on the rest
 * of the frame's rows draw unclipped. While the lists only ever scrolled in
 * whole rows that went unnoticed: the one row hanging off the bottom was
 * painted over by the hint bar afterwards. Now that a row can sit half
 * under the tab bar, which is drawn BEFORE the list, its title would be
 * painted over the tabs. So the column clip here is intersected with the
 * list band by hand, and the band is put back afterwards.
 *
 * Vertically the clip is the whole band rather than the text line: the
 * line clip in vt_ui_text_clipped() only exists because a rectangle needs a
 * height, and it has to leave slack for Thai marks anyway. One clip for all
 * of a row's lines (they share a column) also keeps the cost down — every
 * vita2d_set_clip_rectangle() redraws the stencil. */
static void begin_column(int x, int w, int clip_top, int clip_bottom) {
    vita2d_set_clip_rectangle(x, clip_top, x + w, clip_bottom);
}

static void end_column(int clip_top, int clip_bottom) {
    vita2d_set_clip_rectangle(0, clip_top, VT_SCREEN_W, clip_bottom);
}

void draw_video_row(const yt_video_t *video, int y, int selected,
                    int clip_top, int clip_bottom) {
    if (selected) {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, ROW_H - 4, VT_COLOR_FOCUS);
        vt_ui_rect(8, y, 4, ROW_H - 4, VT_COLOR_ACCENT);
    }
    vita2d_texture *thumb = vt_thumbs_get(video->video_id, video->thumb_url);
    int tx = 24, ty = y + (ROW_H - 4 - THUMB_H) / 2;
    if (thumb) {
        float sx = (float)THUMB_W / vita2d_texture_get_width(thumb);
        float sy = (float)THUMB_H / vita2d_texture_get_height(thumb);
        vita2d_draw_texture_scale(thumb, (float)tx, (float)ty, sx, sy);
    } else {
        vt_ui_rect(tx, ty, THUMB_W, THUMB_H, VT_COLOR_RAISED);
    }
    if (video->duration[0]) {
        int dw = vt_ui_text_width(0.75f, video->duration);
        vt_ui_rect(tx + THUMB_W - dw - 10, ty + THUMB_H - 22, dw + 8, 20,
                   RGBA8(0, 0, 0, 200));
        vt_ui_text(tx + THUMB_W - dw - 6, ty + THUMB_H - 23, VT_COLOR_TEXT,
                   0.75f, video->duration);
    }
    /* Top-left corner, the one the duration is not in: a check for a video
     * that is downloaded, an arrow for one that is queued or on its way. */
    downloads_draw_badge(tx + 6, ty + 6, downloads_badge(video->video_id));
    int text_x = tx + THUMB_W + 16;
    int text_w = VT_SCREEN_W - text_x - 24;
    begin_column(text_x, text_w, clip_top, clip_bottom);
    vt_ui_text(text_x, y + 10, VT_COLOR_TEXT, 1.0f, video->title);
    vt_ui_text(text_x, y + 38, VT_COLOR_MUTED, 0.85f, video->channel);
    vt_ui_text(text_x, y + 60, VT_COLOR_FAINT, 0.85f, video->views);
    end_column(clip_top, clip_bottom);
}

void draw_channel_row(const yt_channel_t *channel, int y, int selected,
                      int clip_top, int clip_bottom) {
    if (selected) {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, CHANNEL_ROW_H - 4, VT_COLOR_FOCUS);
        vt_ui_rect(8, y, 4, CHANNEL_ROW_H - 4, VT_COLOR_ACCENT);
    }
    int cx = 24, cy = y + (CHANNEL_ROW_H - 4 - AVATAR_SIZE) / 2;
    vita2d_texture *avatar = vt_thumbs_get(channel->channel_id,
                                          channel->avatar_url);
    if (avatar) {
        float sx = (float)AVATAR_SIZE / vita2d_texture_get_width(avatar);
        float sy = (float)AVATAR_SIZE / vita2d_texture_get_height(avatar);
        vita2d_draw_texture_scale(avatar, (float)cx, (float)cy, sx, sy);
    } else {
        vt_ui_rect(cx, cy, AVATAR_SIZE, AVATAR_SIZE, VT_COLOR_RAISED);
    }
    int text_x = cx + AVATAR_SIZE + 16;
    begin_column(text_x, VT_SCREEN_W - cx - AVATAR_SIZE - 40, clip_top,
                 clip_bottom);
    vt_ui_text(text_x, y + 22, VT_COLOR_TEXT, 1.0f, channel->title);
    end_column(clip_top, clip_bottom);
}

/* Since the home feed also loads signed out (see yt_home_feed), an empty
 * Start tab no longer means "not logged in yet" — it means the fetch came
 * back with nothing, so this reads as a failure with a way out rather than
 * as a welcome screen. */
void draw_empty_home(int list_top_y, int logged_in) {
    const char *heading = vt_str(S_NO_VIDEOS_LOADED);
    int w = vt_ui_text_width(1.2f, heading);
    vt_ui_text((VT_SCREEN_W - w) / 2, list_top_y + 82, VT_COLOR_TEXT, 1.2f,
              heading);
    const char *reason = vt_str(S_CHECK_WIFI_RESTART);
    int rw = vt_ui_text_width(0.85f, reason);
    vt_ui_text((VT_SCREEN_W - rw) / 2, list_top_y + 116, VT_COLOR_FAINT, 0.85f,
              reason);
    const char *lines[2];
    int n = 0;
    lines[n++] = vt_str(S_HOME_LINE_SEARCH);
    lines[n++] = logged_in ? vt_str(S_HOME_LINE_SUBS)
                           : vt_str(S_HOME_LINE_SIGNIN);
    for (int i = 0; i < n; i++) {
        int lw = vt_ui_text_width(1.0f, lines[i]);
        vt_ui_text((VT_SCREEN_W - lw) / 2, list_top_y + 158 + i * 34,
                  VT_COLOR_MUTED, 1.0f, lines[i]);
    }
}
