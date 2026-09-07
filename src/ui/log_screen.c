/* ViTube — on-device log viewer. See log_screen.h. */
#include "ui/log_screen.h"

#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>

#include "app.h"
#include "log.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "yt/oauth.h"

/* The tail is what anyone reads a log for, and 32 KB of it is several hundred
 * lines — well past what a crash investigation needs and small enough to sit
 * in BSS rather than on a thread stack. */
#define LOG_VIEW_BYTES (32 * 1024)
#define LOG_VIEW_LINES 900
#define LOG_LINE_H 18
#define LOG_TOP 56
#define LOG_VISIBLE ((VT_SCREEN_H - LOG_TOP - 36) / LOG_LINE_H)

static char g_buf[LOG_VIEW_BYTES + 1];
static const char *g_lines[LOG_VIEW_LINES];
static int g_count;
static int g_truncated;

/* Loads the tail of `path` and cuts it into NUL-terminated lines in place.
 * Returns 0 when there was something to show. */
static int load_log(const char *path) {
    g_count = 0;
    g_truncated = 0;
    g_buf[0] = '\0';

    /* vt_log() keeps its own descriptor open for the whole run and every
     * ux0: access in this app goes through one lock — including this one. */
    vt_io_lock();
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int read = -1;
    if (fd >= 0) {
        SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
        SceOff from = size > LOG_VIEW_BYTES ? size - LOG_VIEW_BYTES : 0;
        g_truncated = from > 0;
        sceIoLseek(fd, from, SCE_SEEK_SET);
        read = sceIoRead(fd, g_buf, LOG_VIEW_BYTES);
        sceIoClose(fd);
    }
    vt_io_unlock();
    if (read <= 0) return -1;
    g_buf[read] = '\0';

    char *p = g_buf;
    /* Starting mid-file almost certainly landed inside a line; drop that
     * fragment rather than showing half a timestamp. */
    if (g_truncated) {
        char *nl = strchr(p, '\n');
        if (nl) p = nl + 1;
    }
    while (*p && g_count < LOG_VIEW_LINES) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r') p[len - 1] = '\0';
        g_lines[g_count++] = p;
        if (!nl) break;
        p = nl + 1;
    }
    return g_count > 0 ? 0 : -1;
}

void log_screen_run(void) {
    int prev_file = 0;
    int ok = load_log(VITUBE_DATA_DIR "/log.txt");
    /* Bottom first: a log is read from its end. */
    int top = g_count > LOG_VISIBLE ? g_count - LOG_VISIBLE : 0;

    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    VtNavRepeat nav = {0};

    for (;;) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        unsigned held = ctrl.buttons;
        previous = ctrl;
        if (pressed & SCE_CTRL_CIRCLE) return;
        if (pressed & SCE_CTRL_SQUARE) {
            prev_file = !prev_file;
            ok = load_log(prev_file ? VITUBE_DATA_DIR "/log.prev.txt"
                                    : VITUBE_DATA_DIR "/log.txt");
            top = g_count > LOG_VISIBLE ? g_count - LOG_VISIBLE : 0;
        }

        unsigned direction = vt_nav_repeat(&nav, pressed, held, ctrl.lx,
                                           ctrl.ly);
        int max_top = g_count > LOG_VISIBLE ? g_count - LOG_VISIBLE : 0;
        if (direction & SCE_CTRL_UP) top--;
        if (direction & SCE_CTRL_DOWN) top++;
        if (direction & SCE_CTRL_LEFT) top -= LOG_VISIBLE;
        if (direction & SCE_CTRL_RIGHT) top += LOG_VISIBLE;
        if (top > max_top) top = max_top;
        if (top < 0) top = 0;

        vita2d_start_drawing();
        vita2d_clear_screen();
        char subtitle[64];
        snprintf(subtitle, sizeof subtitle, "%s%s",
                 prev_file ? "log.prev.txt" : "log.txt",
                 g_truncated ? vt_str(S_LOG_TAIL_SUFFIX) : "");
        vt_ui_top_bar(subtitle, yt_oauth_logged_in());
        if (ok < 0) {
            const char *msg = prev_file ? vt_str(S_LOG_NO_PREVIOUS)
                                        : vt_str(S_LOG_EMPTY);
            int w = vt_ui_text_width(1.0f, msg);
            vt_ui_text((VT_SCREEN_W - w) / 2, 240, VT_COLOR_MUTED, 1.0f, msg);
        } else {
            for (int i = 0; i < LOG_VISIBLE && top + i < g_count; i++)
                vt_ui_text_clipped(16, LOG_TOP + i * LOG_LINE_H,
                                   VT_COLOR_MUTED, 0.75f, VT_SCREEN_W - 32,
                                   g_lines[top + i]);
            /* Which slice of the file is on screen; without it, scrolling a
             * few hundred near-identical lines gives no sense of position. */
            char pos[48];
            snprintf(pos, sizeof pos, "%d\xe2\x80\x93%d / %d", top + 1,
                     top + LOG_VISIBLE < g_count ? top + LOG_VISIBLE : g_count,
                     g_count);
            int pw = vt_ui_text_width(0.75f, pos);
            vt_ui_text(VT_SCREEN_W - 16 - pw, VT_SCREEN_H - 30, VT_COLOR_FAINT,
                       0.75f, pos);
        }
        vt_ui_hint_bar(vt_str(prev_file ? S_HINT_LOG_CURRENT
                                        : S_HINT_LOG_PREVIOUS));
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }
}
