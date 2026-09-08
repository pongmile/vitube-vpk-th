/* ViTube — shared app constants. */
#ifndef VITUBE_APP_H
#define VITUBE_APP_H

#define VITUBE_VERSION "1.2"
#define VITUBE_DATA_DIR "ux0:data/ViTube"
#define VITUBE_OAUTH_PATH VITUBE_DATA_DIR "/oauth.json"
#define VITUBE_HD_PREF_PATH VITUBE_DATA_DIR "/hd.flag"
#define VITUBE_REGION_PATH VITUBE_DATA_DIR "/region.txt"
#define VITUBE_LANG_PATH VITUBE_DATA_DIR "/lang.txt"
#define VITUBE_STATE_PATH VITUBE_DATA_DIR "/state.json"

/* HD-via-server helper (yt-dlp on the user's own VPS): resolves a video to a
 * cached 720p H.264/AAC MP4 the Vita can play directly. Token gates the
 * endpoint against drive-by abuse; it is the user's own private server. */

#define VT_SCREEN_W 960
#define VT_SCREEN_H 544

/* Palette (dark). */
#define VT_COLOR_BG        RGBA8(12, 12, 14, 255)
#define VT_COLOR_SURFACE   RGBA8(24, 24, 28, 255)
#define VT_COLOR_RAISED    RGBA8(38, 38, 44, 255)
#define VT_COLOR_FOCUS     RGBA8(56, 56, 66, 255)
#define VT_COLOR_ACCENT    RGBA8(255, 0, 0, 255)
#define VT_COLOR_TEXT      RGBA8(240, 240, 240, 255)
#define VT_COLOR_MUTED     RGBA8(160, 160, 168, 255)
#define VT_COLOR_FAINT     RGBA8(105, 105, 112, 255)
#define VT_COLOR_BORDER    RGBA8(52, 52, 60, 255)
#define VT_COLOR_DANGER    RGBA8(230, 80, 80, 255)
#define VT_COLOR_OK        RGBA8(90, 200, 120, 255)

#endif
