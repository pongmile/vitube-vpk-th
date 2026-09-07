/* ViTube — stateless row/empty-state rendering for list screens. */
#include "ui/rows_view.h"

#include "app.h"
#include "ui/thumbs.h"
#include "ui/strings.h"
#include "ui/ui.h"

#define ROW_H 96
#define CHANNEL_ROW_H 72
#define THUMB_W 152
#define THUMB_H 85
#define AVATAR_SIZE 48

void draw_video_row(const yt_video_t *video, int y, int selected) {
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
    int text_x = tx + THUMB_W + 16;
    int text_w = VT_SCREEN_W - text_x - 24;
    vt_ui_text_clipped(text_x, y + 10, VT_COLOR_TEXT, 1.0f, text_w,
                       video->title);
    vt_ui_text_clipped(text_x, y + 38, VT_COLOR_MUTED, 0.85f, text_w,
                       video->channel);
    vt_ui_text_clipped(text_x, y + 60, VT_COLOR_FAINT, 0.85f, text_w,
                       video->views);
}

void draw_channel_row(const yt_channel_t *channel, int y, int selected) {
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
    vt_ui_text_clipped(cx + AVATAR_SIZE + 16, y + 22, VT_COLOR_TEXT, 1.0f,
                       VT_SCREEN_W - cx - AVATAR_SIZE - 40, channel->title);
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
