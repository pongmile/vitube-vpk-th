/* Platform stand-ins for the headless UI harness: vita2d (recording), input
 * (scripted), network, store, yt, settings, player. Everything here is
 * throwaway test scaffolding; none of it ships. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <vita2d.h>

#include "app.h"
#include "ft2build.h"
#include "ftstub.h"
#include "log.h"
#include "net/http.h"
#include "net/netwatch.h"
#include "store.h"
#include "ui/net_task.h"
#include "ui/player_screen.h"
#include "ui/settings_screen.h"
#include "ui/text_input.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "yt/innertube.h"
#include "yt/oauth.h"
#include "yt/region.h"

#include "stub_ctl.h"

/* ---- time, pad, touch ------------------------------------------------------ */
int g_frame;
unsigned g_pad_buttons;
int g_connected = 1;
ScriptFn h_script;
static unsigned g_t_flags;
static int g_t_x, g_t_y, g_t_dx, g_t_dy;
unsigned h_toast_calls;

uint64_t sceKernelGetProcessTimeWide(void) { return 1000000ULL + (uint64_t)g_frame * 16667ULL; }
int sceKernelExitProcess(int r) { exit(r); }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *d, int n) {
    (void)port; (void)n;
    memset(d, 0, sizeof *d);
    d->buttons = g_pad_buttons;
    d->lx = d->ly = 128;
    return 1;
}
int sceCtrlSetSamplingMode(int m) { (void)m; return 0; }
int scePowerGetBatteryLifePercent(void) { return 87; }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t) { memset(t, 0, sizeof *t); t->hour = 20; t->minute = 15; return 0; }
int sceIoMkdir(const char *p, int m) { (void)p; (void)m; return 0; }
SceUID sceIoOpen(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int sceIoRead(SceUID f, void *b, unsigned long n) { (void)f; (void)b; (void)n; return -1; }
int sceIoWrite(SceUID f, const void *b, unsigned long n) { (void)f; (void)b; (void)n; return -1; }
int sceIoClose(SceUID f) { (void)f; return 0; }

void h_set_touch(unsigned flags, int x, int y, int dx, int dy) {
    g_t_flags = flags; g_t_x = x; g_t_y = y; g_t_dx = dx; g_t_dy = dy;
}
int ui_touch_init(void) { return 0; }
void ui_touch_term(void) {}
int h_touch_resets;
void ui_touch_reset(void) { h_touch_resets++; }
unsigned ui_touch_poll(UiTouchEvent *ev) {
    memset(ev, 0, sizeof *ev);
    unsigned f = g_t_flags;
    if (f) { ev->flags = f; ev->x = g_t_x; ev->y = g_t_y; ev->down_x = g_t_dx; ev->down_y = g_t_dy; }
    return f;
}
int ui_touch_hit_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- log, io lock, region --------------------------------------------------- */
void vt_log_init(void) {}
void vt_log(const char *fmt, ...) { (void)fmt; }
void vt_io_lock(void) {}
void vt_io_unlock(void) {}
const char *yt_region_hl(void) { return "en"; }

/* ---- network ----------------------------------------------------------------- */
int http_connected(void) { return g_connected; }
unsigned vt_netwatch_generation(void) { return 0; }

/* ---- FreeType (ui.c links it; never initialised here) -------------------------- */
int FT_Init_FreeType(FT_Library *l) { (void)l; return 1; }
int FT_Done_FreeType(FT_Library l) { (void)l; return 0; }
int FT_New_Face(FT_Library l, const char *p, long i, FT_Face *f) { (void)l; (void)p; (void)i; (void)f; return 1; }
int FT_Done_Face(FT_Face f) { (void)f; return 0; }
unsigned int FT_Get_Char_Index(FT_Face f, unsigned long c) { (void)f; (void)c; return 0; }

/* ---- vita2d recorder ------------------------------------------------------------ */
typedef struct {
    char kind; /* R rect, T text, C circle, L line, X tex */
    float a, b, c, d;
    unsigned color;
    int px;
    int clip_on, cx0, cy0, cx1, cy1;
    int tex;
    char text[320];
} Op;
#define MAXOPS 30000
static Op g_ops[MAXOPS], g_prev[MAXOPS];
static int g_nops, g_nprev;
static int g_clip_on, g_cx0, g_cy0, g_cx1, g_cy1;
static int g_dump_pending;
static char g_dump_name[128];
int h_clip_violations;
int h_disable_calls, h_prev_disable_calls;

static void rec(Op *o) {
    if (g_nops >= MAXOPS) return;
    o->clip_on = g_clip_on; o->cx0 = g_cx0; o->cy0 = g_cy0; o->cx1 = g_cx1; o->cy1 = g_cy1;
    g_ops[g_nops++] = *o;
}
void vita2d_start_drawing(void) { g_nops = 0; h_disable_calls = 0; }
void vita2d_end_drawing(void) {}
void vita2d_clear_screen(void) {}
void vita2d_wait_rendering_done(void) {}
void vita2d_enable_clipping(void) { g_clip_on = 1; }
void vita2d_disable_clipping(void) { g_clip_on = 0; h_disable_calls++; }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) { g_cx0 = x0; g_cy0 = y0; g_cx1 = x1; g_cy1 = y1; }
int vita2d_init(void) { return 1; }
int vita2d_fini(void) { return 0; }
void vita2d_free_font(vita2d_font *f) { (void)f; }
void vita2d_free_pgf(vita2d_pgf *f) { (void)f; }
void *vita2d_get_context(void) { return NULL; }
void *vita2d_get_current_fb(void) { return NULL; }
vita2d_pgf *vita2d_load_default_pgf(void) { return NULL; }
vita2d_font *vita2d_load_font_file(const char *p) { (void)p; return NULL; }
void vita2d_set_clear_color(unsigned c) { (void)c; }
int vita2d_font_draw_text(vita2d_font *f, int x, int y, unsigned c, unsigned px, const char *t) { (void)f; (void)x; (void)y; (void)c; (void)px; (void)t; return 0; }
int vita2d_font_text_width(vita2d_font *f, unsigned px, const char *t) { (void)f; (void)px; (void)t; return 0; }
void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned c) { Op o = {0}; o.kind = 'R'; o.a = x; o.b = y; o.c = w; o.d = h; o.color = c; rec(&o); }
void vita2d_draw_line(float x0, float y0, float x1, float y1, unsigned c) { Op o = {0}; o.kind = 'L'; o.a = x0; o.b = y0; o.c = x1; o.d = y1; o.color = c; rec(&o); }
void vita2d_draw_fill_circle(float x, float y, float r, unsigned c) { Op o = {0}; o.kind = 'C'; o.a = x; o.b = y; o.c = r; o.color = c; rec(&o); }
void vita2d_free_texture(vita2d_texture *t) { (void)t; }
vita2d_texture *vita2d_load_JPEG_buffer(const void *b, unsigned long n) { (void)b; (void)n; return NULL; }
int vita2d_texture_get_width(const vita2d_texture *t) { (void)t; return 320; }
int vita2d_texture_get_height(const vita2d_texture *t) { (void)t; return 180; }
void vita2d_draw_texture_scale(const vita2d_texture *t, float x, float y, float sx, float sy) {
    Op o = {0}; o.kind = 'X'; o.a = x; o.b = y; o.c = 320 * sx; o.d = 180 * sy; o.tex = (int)(intptr_t)t; rec(&o);
}

static int px_for(float scale) { return scale <= 0.87f ? 14 : scale <= 1.05f ? 18 : scale <= 1.5f ? 21 : 36; }
/* A fixed advance per code point (about the average of the UI font, 0.55 em):
 * the scenarios read what was drawn, never where, so the font's real metrics
 * are not needed. */
int vita2d_pgf_text_width(vita2d_pgf *p, float scale, const char *text) {
    (void)p;
    int px = px_for(scale);
    int total = 0;
    for (const unsigned char *s = (const unsigned char *)text; *s; s++)
        if ((*s & 0xc0) != 0x80) total += px * 11 / 20;
    return total;
}
int vita2d_pgf_draw_text(vita2d_pgf *p, int x, int y, unsigned c, float scale, const char *text) {
    (void)p;
    Op o = {0};
    o.kind = 'T'; o.a = (float)x; o.b = (float)(y - (int)(17.0f * scale)); o.color = c; o.px = px_for(scale);
    snprintf(o.text, sizeof o.text, "%s", text);
    rec(&o);
    return 0;
}

int h_text_seen(const char *needle) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].kind == 'T' && strstr(g_prev[i].text, needle)) return 1;
    return 0;
}
int h_text_seen_prev(const char *needle) { return h_text_seen(needle); }
int h_op_count(char kind) {
    int n = 0;
    for (int i = 0; i < g_nprev; i++) if (g_prev[i].kind == kind) n++;
    return n;
}
/* colour of the first text op containing `needle` in the last frame */
unsigned h_text_color(const char *needle) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].kind == 'T' && strstr(g_prev[i].text, needle)) return g_prev[i].color;
    return 0;
}
/* first text op containing needle: its y, or -1 */
int h_text_y(const char *needle) {
    for (int i = 0; i < g_nprev; i++)
        if (g_prev[i].kind == 'T' && strstr(g_prev[i].text, needle)) return (int)g_prev[i].b;
    return -1;
}
int h_prev_disables(void) { return h_prev_disable_calls; }
void h_dump(const char *name) { g_dump_pending = 1; snprintf(g_dump_name, sizeof g_dump_name, "%s", name); }

static char g_watch[64];
static int g_watch_on;
int h_watch_appearances, h_watch_frames;
void h_watch(const char *needle) {
    snprintf(g_watch, sizeof g_watch, "%s", needle);
    g_watch_on = 0;
    h_watch_appearances = h_watch_frames = 0;
}

void vita2d_swap_buffers(void) {
    memcpy(g_prev, g_ops, (size_t)g_nops * sizeof(Op));
    g_nprev = g_nops;
    h_prev_disable_calls = h_disable_calls;
    if (g_watch[0]) {
        int on = h_text_seen(g_watch);
        if (on) h_watch_frames++;
        if (on && !g_watch_on) h_watch_appearances++;
        g_watch_on = on;
    }
    if (g_dump_pending) {
        char path[256];
        snprintf(path, sizeof path, "out/%s.ops", g_dump_name);
        FILE *f = fopen(path, "w");
        if (f) {
            for (int i = 0; i < g_nops; i++) {
                Op *o = &g_ops[i];
                /* kind a b c d px color clip_on cx0 cy0 cx1 cy1 tex \t text */
                fprintf(f, "%c %d %d %d %d %d %u %d %d %d %d %d %d\t%s\n", o->kind, (int)o->a, (int)o->b, (int)o->c,
                        (int)o->d, o->px, o->color, o->clip_on, o->cx0, o->cy0, o->cx1, o->cy1, o->tex,
                        o->kind == 'T' ? o->text : "");
            }
            fclose(f);
        }
        g_dump_pending = 0;
    }
    g_frame++;
    if (h_script && h_script(g_frame)) {
        fflush(stdout);
        exit(0);
    }
}

/* ---- thumbs ------------------------------------------------------------------------ */
static int tex_for(const char *key) {
    unsigned h = 5381;
    for (const char *p = key; *p; p++) h = h * 33 + (unsigned char)*p;
    return (int)(h % 7) + 1;
}
int vt_thumbs_init(void) { return 0; }
void vt_thumbs_term(void) {}
void vt_thumbs_pump(void) {}
void vt_thumbs_clear(void) {}
int h_thumb_net_calls, h_thumb_file_calls;
vita2d_texture *vt_thumbs_get(const char *key, const char *url) {
    (void)url; h_thumb_net_calls++;
    int t = tex_for(key);
    return t == 3 ? NULL : (vita2d_texture *)(intptr_t)t;
}
vita2d_texture *vt_thumbs_get_file(const char *key, const char *path) {
    (void)path; h_thumb_file_calls++;
    return (vita2d_texture *)(intptr_t)(tex_for(key) + 10);
}

/* ---- store ------------------------------------------------------------------------------ */
void vt_store_load(void) {}
unsigned vt_store_revision(void) { return 1; }
int vt_store_history_count(void) { return 0; }
const yt_video_t *vt_store_history_at(int i) { (void)i; return NULL; }
void vt_store_history_add(const yt_video_t *v) { (void)v; }
int vt_store_search_count(void) { return 0; }
const char *vt_store_search_at(int i) { (void)i; return ""; }
void vt_store_search_add(const char *q) { (void)q; }
void vt_store_clear_personal(void) {}
int vt_store_channel_sort(void) { return 0; }
void vt_store_set_channel_sort(int s) { (void)s; }

/* ---- yt --------------------------------------------------------------------------------- */
int h_home_count = 12;
static void fake_video(yt_video_t *v, int i) {
    memset(v, 0, sizeof *v);
    snprintf(v->video_id, sizeof v->video_id, "vid%08d", i);
    const char *titles[] = {
        "Short title", "A very long title that goes on and on about nothing in particular and keeps going past the edge of the screen for sure",
        "Lo-fi beats to study to", "\xe0\xb8\x97\xe0\xb8\x94\xe0\xb8\xaa\xe0\xb8\xad\xe0\xb8\x9a\xe0\xb8\x8a\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2", "Street food in Bangkok - the full tour", "News at 6" };
    snprintf(v->title, sizeof v->title, "%s %d", titles[i % 6], i);
    snprintf(v->channel, sizeof v->channel, "Channel %d", i % 4);
    snprintf(v->duration, sizeof v->duration, "%d:%02d", 3 + i % 9, (i * 7) % 60);
    snprintf(v->views, sizeof v->views, "%d views", i * 1234);
    snprintf(v->thumb_url, sizeof v->thumb_url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", v->video_id);
    v->age_min = i;
}
#include "p_backend.inc"

/* ---- other screens ------------------------------------------------------------------------ */
int h_cancel_next;
int h_net_tasks;
int vt_ui_run_net_task(const char *label, VtTaskFn fn, void *arg, volatile int *cancel) {
    (void)label;
    h_net_tasks++;
    if (h_cancel_next) { h_cancel_next = 0; *cancel = 1; fn(arg); return -1; } /* Circle on the spinner */
    return fn(arg);
}
int ui_text_input(const char *t, const char *i, char *out, size_t n) { (void)t; (void)i; snprintf(out, n, "%s", h_search_text); return 1; }
SettingsResult h_settings_result = SETTINGS_NOTHING;
SettingsResult settings_screen_input(unsigned p, unsigned d) { (void)p; (void)d; SettingsResult r = h_settings_result; h_settings_result = SETTINGS_NOTHING; return r; }
void settings_screen_draw(int top) { vt_ui_text(28, top + 14, VT_COLOR_TEXT, 1.0f, "[settings rows]"); }
const char *settings_screen_hint(void) { return "settings hint"; }
int settings_screen_modal(void) { return 0; }
char g_played[16][16];
int g_nplayed;
void (*h_in_player)(void);
void player_screen_run(const yt_video_t *v) {
    if (g_nplayed < 16) snprintf(g_played[g_nplayed++], 16, "%s", v->video_id);
    fprintf(stdout, "  [player] %s | %s | %s | %s\n", v->video_id, v->title, v->channel, v->duration);
    if (h_in_player) h_in_player();
}
