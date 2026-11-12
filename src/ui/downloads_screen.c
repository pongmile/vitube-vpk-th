/* ViTube — the Downloads tab, the download menu and the row markers. See
 * downloads_screen.h. */
#include "ui/downloads_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "app.h"
#include "dl/dl.h"
#include "log.h"
#include "net/http.h"
#include "ui/dl_format.h"
#include "ui/kinetic_scroll.h"
#include "ui/player_screen.h"
#include "ui/strings.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"

/* The same row as the video lists (list_screen.c, rows_view.c) so the tab
 * reads as one of them and a finger lands where it does there. */
#define ROW_H 96
#define THUMB_W 152
#define THUMB_H 85
#define HEADER_H 30
/* How far Left/Right page the cursor, as in the video lists. */
#define VISIBLE_ROWS 4
/* More than a Vita's card holds of 36 MB videos at the sizes in question; an
 * entry past this is not shown rather than allocated for. */
#define DLS_MAX 200

#define SEP " \xc2\xb7 " /* " · " */

/* --- the list --------------------------------------------------------------
 *
 * ONE copy of the engine's list, refreshed when dl_revision() moves (and
 * once a second while a retry countdown is on screen, which the engine does
 * not announce). The tab, the markers on the video rows and the menu all read
 * it, so a frame costs a revision compare however many of them look.
 *
 * g_snap is in the engine's order (oldest first); g_order says how the tab
 * shows it: what is not finished first, in that order — the queue reads as a
 * queue, and pausing a row does not make it jump — then the finished ones,
 * newest first. A download that completes moves from the first group to the
 * second; the selection follows it (see rebuild()). */
static DlItem g_snap[DLS_MAX];
static short g_order[DLS_MAX];
static int g_count;
static unsigned g_rev;
static int g_synced;
static uint64_t g_snap_us;
static int g_countdown;  /* an item shows "Retrying in N s" */
static int g_pausable;   /* QUEUED, RESOLVING, DOWNLOADING, WAITING */
static int g_paused;

/* The card, refreshed on entry, with the list, and otherwise every 2 s while
 * somebody looks: free space moves with every byte written, but a devctl per
 * progress step would be a waste. */
static DlStorage g_storage;
static int g_storage_ok;
static uint64_t g_storage_us;
#define STORAGE_REFRESH_US 2000000ULL
#define COUNTDOWN_REFRESH_US 1000000ULL

/* Selection by identity as well as by index: the order above changes under
 * the cursor (a finished download moves, one is deleted, one is added), and
 * the row the user had picked must stay the row they picked. */
static int g_cursor;
static char g_cursor_id[YT_VIDEO_ID_LEN];

/* Scroll state, as list_screen.c's ListScroll, minus `top`: nothing outside
 * this file ever scrolls this list. */
static struct {
    VtKineticScroll kinetic;
    int synced_cursor;
} g_scroll;
static int g_band_top, g_band_bottom;

static const DlItem *item_at(int display_index) {
    return &g_snap[g_order[display_index]];
}

static const DlItem *find_item(const char *video_id) {
    if (!video_id || !video_id[0]) return NULL;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_snap[i].video_id, video_id) == 0) return &g_snap[i];
    return NULL;
}

static int display_index_of(const char *video_id) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(item_at(i)->video_id, video_id) == 0) return i;
    return -1;
}

static void remember_cursor(void) {
    if (g_count > 0 && g_cursor >= 0 && g_cursor < g_count)
        snprintf(g_cursor_id, sizeof g_cursor_id, "%s",
                 item_at(g_cursor)->video_id);
    else
        g_cursor_id[0] = '\0';
}

static void rebuild(uint64_t now) {
    /* The newest are the ones that fall off the end, so the log says so: a
     * list that silently stops growing would be a puzzling bug report. Once
     * per crossing, not once per refresh. */
    static int logged_overflow;
    int total = dl_count();
    if (total > DLS_MAX && !logged_overflow)
        vt_log("downloads: %d items, the screen shows the first %d", total,
               DLS_MAX);
    logged_overflow = total > DLS_MAX;
    g_count = dl_snapshot(g_snap, DLS_MAX);
    if (g_count < 0) g_count = 0;
    if (g_count > DLS_MAX) g_count = DLS_MAX;
    g_snap_us = now;

    int n = 0;
    g_pausable = g_paused = g_countdown = 0;
    for (int i = 0; i < g_count; i++) {
        const DlItem *it = &g_snap[i];
        if (it->state != DL_DONE) g_order[n++] = (short)i;
        if (it->state == DL_QUEUED || it->state == DL_RESOLVING ||
            it->state == DL_DOWNLOADING || it->state == DL_WAITING)
            g_pausable++;
        if (it->state == DL_PAUSED) g_paused++;
        if (it->state == DL_WAITING && it->retry_in_s >= 0) g_countdown = 1;
    }
    for (int i = g_count - 1; i >= 0; i--)
        if (g_snap[i].state == DL_DONE) g_order[n++] = (short)i;

    /* Find the selected download again. Gone (deleted, or a finished one the
     * engine dropped because its file vanished): the index stays, so the
     * row that slid into its place is selected — what a list does. */
    if (g_cursor_id[0]) {
        int at = display_index_of(g_cursor_id);
        if (at >= 0) g_cursor = at;
    }
    if (g_cursor >= g_count) g_cursor = g_count - 1;
    if (g_cursor < 0) g_cursor = 0;
    remember_cursor();
    /* The selection moving because the LIST moved is not the user asking to
     * see it: a cursor index that merely changed must not ease the scroll
     * (and stop a fling) as a button press does. A cursor that ends up off
     * screen at rest is brought back by the scroll step's own invariant. */
    g_scroll.synced_cursor = g_cursor;
}

static void sync_items(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    unsigned rev = dl_revision();
    if (!g_synced || rev != g_rev ||
        (g_countdown && now - g_snap_us >= COUNTDOWN_REFRESH_US)) {
        /* The revision is read BEFORE the snapshot: a change landing in
         * between is then seen as one by the next call, never lost. */
        g_rev = rev;
        g_synced = 1;
        rebuild(now);
    }
}

static void sync_storage(int force) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!force && g_storage_us && now - g_storage_us < STORAGE_REFRESH_US)
        return;
    g_storage_us = now;
    g_storage_ok = dl_storage(&g_storage) == 0;
}

/* --- text ------------------------------------------------------------------ */

static char decimal_mark(void) {
    char c = vt_str(S_DL_DECIMAL_MARK)[0];
    return c == ',' ? ',' : '.';
}

static void size_text(char *out, int64_t bytes) {
    vt_fmt_bytes(out, VT_FMT_SIZE_MAX, bytes, decimal_mark());
}

/* A line built from pieces joined with " · ". */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} Line;

static void line_add(Line *l, const char *piece) {
    if (!piece[0] || l->len + 1 >= l->cap) return;
    int n = snprintf(l->buf + l->len, l->cap - l->len, "%s%s",
                     l->len ? SEP : "", piece);
    if (n < 0) return;
    l->len += (size_t)n < l->cap - l->len ? (size_t)n : l->cap - l->len - 1;
}

/* Why a download failed, in words. NO_SPACE says what was needed and what is
 * free; "needed" includes the reserve the engine keeps back (DL_RESERVE_BYTES)
 * because that is the number free space has to reach — "Needs 36.4 MB, only
 * 100 MB free" would be a sentence that contradicts itself. The engine
 * recorded the failure when it happened, though: if the card has room by now
 * (the user deleted something) saying "only 20 MB free" would be a lie, so
 * the line says the retry will work. */
static void error_text(const DlItem *it, char *out, size_t cap) {
    switch (it->error) {
    case DL_ERR_NO_SPACE: {
        int64_t needed = it->bytes_total + DL_RESERVE_BYTES;
        if (it->bytes_total <= 0 || !g_storage_ok) {
            snprintf(out, cap, "%s", vt_str(S_DL_ERR_DISK_FULL));
        } else if (g_storage.free_bytes >= needed) {
            snprintf(out, cap, "%s", vt_str(S_DL_ERR_SPACE_FREED));
        } else {
            char a[VT_FMT_SIZE_MAX], b[VT_FMT_SIZE_MAX];
            size_text(a, needed);
            size_text(b, g_storage.free_bytes);
            snprintf(out, cap, vt_str(S_DL_ERR_NO_SPACE), a, b);
        }
        return;
    }
    case DL_ERR_DISK_FULL:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_DISK_FULL));
        return;
    case DL_ERR_NOT_DOWNLOADABLE:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_NOT_DOWNLOADABLE));
        return;
    case DL_ERR_VERIFY:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_VERIFY));
        return;
    case DL_ERR_NETWORK:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_NETWORK));
        return;
    case DL_ERR_URL:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_URL));
        return;
    case DL_ERR_IO:
        snprintf(out, cap, "%s", vt_str(S_DL_ERR_IO));
        return;
    default:
        snprintf(out, cap, "%s", vt_str(S_UNKNOWN_ERROR));
        return;
    }
}

/* The percentage piece, or an empty string when the total is not known. */
static void percent_piece(const DlItem *it, char *out, size_t cap) {
    int pct = vt_fmt_percent(it->bytes_done, it->bytes_total);
    if (pct < 0) out[0] = '\0';
    else snprintf(out, cap, vt_str(S_DL_ST_PERCENT), pct);
}

/* One row's status line and its colour. */
static void status_line(const DlItem *it, char *out, size_t cap,
                        unsigned *color) {
    Line l = { out, cap, 0 };
    char piece[96], a[VT_FMT_SIZE_MAX], b[VT_FMT_SIZE_MAX];
    out[0] = '\0';
    *color = VT_COLOR_MUTED;
    switch (it->state) {
    case DL_QUEUED:
        line_add(&l, vt_str(S_DL_ST_QUEUED));
        break;
    case DL_RESOLVING:
        line_add(&l, vt_str(S_DL_ST_RESOLVING));
        break;
    case DL_DOWNLOADING:
        *color = VT_COLOR_TEXT;
        percent_piece(it, piece, sizeof piece);
        line_add(&l, piece);
        if (it->bytes_total > 0) {
            /* "15.3 of 36.4 MB": the progress in the total's unit, so the
             * unit is said once and both numbers are read against it. */
            int unit = vt_fmt_unit(it->bytes_total);
            vt_fmt_bytes_in(a, sizeof a, it->bytes_done, unit, 0,
                            decimal_mark());
            size_text(b, it->bytes_total);
            snprintf(piece, sizeof piece, vt_str(S_DL_ST_OF), a, b);
            line_add(&l, piece);
        } else {
            size_text(a, it->bytes_done);
            line_add(&l, a);
        }
        if (it->bytes_per_sec > 0) {
            vt_fmt_speed(piece, sizeof piece, it->bytes_per_sec,
                         decimal_mark());
            line_add(&l, piece);
        }
        break;
    case DL_PAUSED:
        line_add(&l, vt_str(S_DL_ST_PAUSED));
        percent_piece(it, piece, sizeof piece);
        line_add(&l, piece);
        break;
    case DL_WAITING:
        percent_piece(it, piece, sizeof piece);
        line_add(&l, piece);
        if (it->retry_in_s > 0) {
            vt_fmt_wait(a, sizeof a, it->retry_in_s);
            snprintf(piece, sizeof piece, vt_str(S_DL_ST_RETRY_IN), a);
            line_add(&l, piece);
        } else if (it->retry_in_s == 0) {
            line_add(&l, vt_str(S_DL_ST_RETRY_NOW));
        } else {
            /* Waits for an event: either the network is gone, or a video
             * is being streamed and the engine gave way to it. The engine
             * does not say which, and it does not need to — if the console
             * is connected, it can only be the video. */
            line_add(&l, vt_str(http_connected() ? S_DL_ST_WAIT_PLAYBACK
                                                 : S_WAITING_FOR_WIFI));
        }
        break;
    case DL_VERIFYING:
        line_add(&l, vt_str(S_DL_ST_VERIFYING));
        break;
    case DL_DONE:
        *color = VT_COLOR_OK;
        line_add(&l, vt_str(S_DL_ST_READY));
        if (it->bytes_total > 0) {
            size_text(a, it->bytes_total);
            line_add(&l, a);
        }
        if (it->height > 0) {
            snprintf(piece, sizeof piece, "%dp", it->height);
            line_add(&l, piece);
        }
        line_add(&l, it->duration);
        break;
    case DL_FAILED:
        *color = VT_COLOR_DANGER;
        error_text(it, piece, sizeof piece);
        line_add(&l, piece);
        break;
    }
}

/* --- thumbnails ------------------------------------------------------------ */

/* Whether a download has its thumbnail saved is a question to the card, so it
 * is asked once and remembered; asking for every row of every frame would be
 * hundreds of file lookups a second, taking the card's lock each time. A
 * "no" is remembered for a few seconds only: the engine saves the picture a
 * moment after it queues the video. */
#define THUMB_MEMO 24
#define THUMB_MISS_US 3000000ULL
static struct {
    char id[YT_VIDEO_ID_LEN];
    int has;
    uint64_t at_us;
    char path[72];
} g_memo[THUMB_MEMO];
static int g_memo_next;

static const char *thumb_file_for(const char *video_id) {
    uint64_t now = sceKernelGetProcessTimeWide();
    int slot = -1;
    for (int i = 0; i < THUMB_MEMO; i++) {
        if (strcmp(g_memo[i].id, video_id) != 0) continue;
        if (g_memo[i].has) return g_memo[i].path;
        if (now - g_memo[i].at_us < THUMB_MISS_US) return NULL;
        slot = i;
        break;
    }
    if (slot < 0) {
        slot = g_memo_next;
        g_memo_next = (g_memo_next + 1) % THUMB_MEMO;
    }
    snprintf(g_memo[slot].id, sizeof g_memo[slot].id, "%s", video_id);
    g_memo[slot].at_us = now;
    g_memo[slot].has = dl_thumb_path(video_id, g_memo[slot].path,
                                     sizeof g_memo[slot].path);
    return g_memo[slot].has ? g_memo[slot].path : NULL;
}

/* The picture for a row: the saved file (no network needed — this is the
 * screen people open without Wi-Fi), else YouTube's standard thumbnail when
 * the console is online, else nothing and the caller draws a placeholder.
 * Offline never produces an error, only the placeholder. */
static vita2d_texture *row_thumb(const DlItem *it) {
    const char *file = thumb_file_for(it->video_id);
    if (file) return vt_thumbs_get_file(it->video_id, file);
    if (!http_connected()) return NULL;
    char url[96];
    snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg",
             it->video_id);
    return vt_thumbs_get(it->video_id, url);
}

/* --- markers ----------------------------------------------------------------- */

static void thick_line(float x0, float y0, float x1, float y1,
                       unsigned color) {
    vita2d_draw_line(x0, y0, x1, y1, color);
    vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, color);
}

DlBadge downloads_badge(const char *video_id) {
    sync_items();
    const DlItem *it = find_item(video_id);
    if (!it) return DL_BADGE_NONE;
    switch (it->state) {
    case DL_DONE:   return DL_BADGE_DONE;
    case DL_PAUSED: return DL_BADGE_PAUSED;
    case DL_FAILED: return DL_BADGE_FAILED;
    default:        return DL_BADGE_ACTIVE;
    }
}

/* Vector shapes, like the face buttons in ui.c: the font has no check or
 * arrow to rely on, and these are the same on every language and size. */
void downloads_draw_badge(int x, int y, DlBadge badge) {
    if (badge == DL_BADGE_NONE) return;
    float cx = (float)x + 11.0f, cy = (float)y + 11.0f;
    vita2d_draw_fill_circle(cx, cy, 11.0f, RGBA8(0, 0, 0, 205));
    switch (badge) {
    case DL_BADGE_DONE:
        thick_line(cx - 5, cy, cx - 1, cy + 4, VT_COLOR_OK);
        thick_line(cx - 1, cy + 4, cx + 6, cy - 4, VT_COLOR_OK);
        break;
    case DL_BADGE_ACTIVE:
        thick_line(cx, cy - 6, cx, cy + 5, VT_COLOR_TEXT);
        thick_line(cx - 5, cy, cx, cy + 5, VT_COLOR_TEXT);
        thick_line(cx + 5, cy, cx, cy + 5, VT_COLOR_TEXT);
        break;
    case DL_BADGE_PAUSED:
        vt_ui_rect((int)cx - 4, (int)cy - 5, 3, 10, VT_COLOR_MUTED);
        vt_ui_rect((int)cx + 1, (int)cy - 5, 3, 10, VT_COLOR_MUTED);
        break;
    case DL_BADGE_FAILED:
        vt_ui_rect((int)cx - 1, (int)cy - 6, 3, 8, VT_COLOR_DANGER);
        vt_ui_rect((int)cx - 1, (int)cy + 4, 3, 3, VT_COLOR_DANGER);
        break;
    default:
        break;
    }
}

/* --- snackbar ------------------------------------------------------------------ */

#define SNACK_US 2400000ULL
static char g_snack[128];
static uint64_t g_snack_until;
static int g_snack_bad;

static void snackbar(const char *text, int bad) {
    snprintf(g_snack, sizeof g_snack, "%s", text);
    g_snack_bad = bad;
    g_snack_until = sceKernelGetProcessTimeWide() + SNACK_US;
}

void downloads_snackbar_draw(int bottom) {
    if (!g_snack_until) return;
    if (sceKernelGetProcessTimeWide() >= g_snack_until) {
        g_snack_until = 0;
        return;
    }
    int tw = vt_ui_text_width(1.0f, g_snack);
    int w = tw + 48;
    if (w > VT_SCREEN_W - 48) w = VT_SCREEN_W - 48;
    int x = (VT_SCREEN_W - w) / 2, y = bottom - 54;
    vt_ui_rect(x - 1, y - 1, w + 2, 42, VT_COLOR_BORDER);
    vt_ui_rect(x, y, w, 40, VT_COLOR_RAISED);
    vt_ui_text_clipped(x + 24, y + 9, g_snack_bad ? VT_COLOR_DANGER
                                                  : VT_COLOR_TEXT,
                       1.0f, w - 48, g_snack);
}

/* --- scrolling ------------------------------------------------------------------
 *
 * list_screen.c's list_scroll_step() and the helpers around it, for one list
 * whose band is not LIST_Y..LIST_BOTTOM (the storage line sits above it). The
 * reasoning in the comments there applies unchanged and is not repeated:
 * pixel scroll, taps hit-tested against the offset the finger came down on,
 * a cursor that touch scrolling leaves alone until the list is at rest, and a
 * press of a direction after a fling that first picks a row on screen. */

static int band_h(void) {
    return g_band_bottom - g_band_top;
}

static void full_rows(int *first, int *last) {
    int off = vt_kinetic_draw_offset(&g_scroll.kinetic);
    int f = off > 0 ? (off + ROW_H - 1) / ROW_H : 0;
    int l = (off + band_h()) / ROW_H - 1;
    if (l > g_count - 1) l = g_count - 1;
    if (f > l) f = l;
    if (f < 0) f = 0;
    *first = f;
    *last = l < f ? f : l;
}

static int cursor_off_screen(int cursor) {
    int off = vt_kinetic_draw_offset(&g_scroll.kinetic);
    return (cursor + 1) * ROW_H <= off || cursor * ROW_H >= off + band_h();
}

static int cursor_onto_screen(int *cursor) {
    if (!cursor_off_screen(*cursor)) return 0;
    int first, last;
    full_rows(&first, &last);
    *cursor = *cursor < first ? first : last;
    return 1;
}

static int cursor_into_view(int *cursor) {
    if (g_scroll.kinetic.mode == VT_KINETIC_EASING) return 0;
    return cursor_onto_screen(cursor);
}

/* Returns the row a tap landed on, or -1. */
static int scroll_step(unsigned touch_flags, const UiTouchEvent *touch) {
    VtKineticScroll *k = &g_scroll.kinetic;
    uint64_t now = sceKernelGetProcessTimeWide();
    vt_kinetic_set_bounds(k, (float)band_h(), (float)(g_count * ROW_H));

    int tapped = -1, tap_y = 0;
    if (vt_kinetic_touch(k, touch_flags, touch->x, touch->y - g_band_top, now,
                         &tap_y) &&
        tap_y >= 0 && tap_y / ROW_H < g_count)
        tapped = tap_y / ROW_H;
    if (vt_kinetic_take_settled(k)) cursor_onto_screen(&g_cursor);

    if (g_cursor != g_scroll.synced_cursor ||
        (k->mode == VT_KINETIC_IDLE && cursor_off_screen(g_cursor)))
        vt_kinetic_ensure_visible(k, (float)(g_cursor * ROW_H),
                                  (float)((g_cursor + 1) * ROW_H));
    vt_kinetic_update(k, now);
    if (vt_kinetic_take_settled(k)) cursor_onto_screen(&g_cursor);
    g_scroll.synced_cursor = g_cursor;
    return tapped;
}

/* --- drawing the tab ----------------------------------------------------------- */

static void draw_header(int top) {
    vita2d_set_clip_rectangle(0, top, VT_SCREEN_W, top + HEADER_H);
    if (g_storage_ok) {
        char free_s[VT_FMT_SIZE_MAX], total_s[VT_FMT_SIZE_MAX],
            dl_s[VT_FMT_SIZE_MAX], text[160];
        size_text(free_s, g_storage.free_bytes);
        size_text(total_s, g_storage.total_bytes);
        size_text(dl_s, g_storage.downloads_bytes);
        if (g_storage.low)
            snprintf(text, sizeof text, vt_str(S_DL_STORAGE_LOW), free_s, dl_s);
        else
            snprintf(text, sizeof text, vt_str(S_DL_STORAGE), free_s, total_s,
                     dl_s);
        unsigned color = g_storage.low ? VT_COLOR_DANGER : VT_COLOR_MUTED;
        vt_ui_text(24, top + 7, color, 0.85f, text);

        /* How full the card is, at a glance: used / capacity. The same red
         * when it is below the reserve, so the strip says "full" without
         * being read. */
        int mw = 140, mx = VT_SCREEN_W - 24 - mw, my = top + 12;
        vt_ui_rect(mx, my, mw, 6, VT_COLOR_RAISED);
        if (g_storage.total_bytes > 0) {
            int64_t used = g_storage.total_bytes - g_storage.free_bytes;
            if (used < 0) used = 0;
            if (used > g_storage.total_bytes) used = g_storage.total_bytes;
            int fill = (int)((double)used / (double)g_storage.total_bytes * mw);
            if (fill < 1 && used > 0) fill = 1;
            if (fill > 0)
                vt_ui_rect(mx, my, fill, 6, g_storage.low ? VT_COLOR_DANGER
                                                          : VT_COLOR_FAINT);
        }
    }
    vt_ui_rect(0, top + HEADER_H - 1, VT_SCREEN_W, 1, VT_COLOR_BORDER);
}

static void draw_empty(int top) {
    const char *title = vt_str(S_DL_EMPTY_TITLE);
    int w = vt_ui_text_width(1.2f, title);
    vt_ui_text((VT_SCREEN_W - w) / 2, top + 70, VT_COLOR_TEXT, 1.2f, title);
    const char *lines[2] = { vt_str(S_DL_EMPTY_LINE1),
                             vt_str(S_DL_EMPTY_LINE2) };
    for (int i = 0; i < 2; i++) {
        int lw = vt_ui_text_width(1.0f, lines[i]);
        vt_ui_text((VT_SCREEN_W - lw) / 2, top + 116 + i * 32, VT_COLOR_MUTED,
                   1.0f, lines[i]);
    }
}

/* Row text is clipped to its column and, vertically, to the whole list band
 * — see begin_column() in rows_view.c: vt_ui_text_clipped() would switch
 * clipping off for the rest of the frame, and a row half under the tab bar
 * would then paint over it. The band is also what keeps a Thai mark that
 * rises above its line from being cut. */
static void begin_column(int x, int w) {
    vita2d_set_clip_rectangle(x, g_band_top, x + w, g_band_bottom);
}

static void end_column(void) {
    vita2d_set_clip_rectangle(0, g_band_top, VT_SCREEN_W, g_band_bottom);
}

static void draw_progress(const DlItem *it, int x, int y, int w) {
    unsigned fill_color;
    switch (it->state) {
    case DL_DOWNLOADING:
    case DL_VERIFYING:
    case DL_RESOLVING:
        fill_color = VT_COLOR_ACCENT;
        break;
    case DL_FAILED:
        fill_color = VT_COLOR_DANGER;
        break;
    default:
        fill_color = VT_COLOR_FAINT;
        break;
    }
    vt_ui_rect(x, y, w, 4, VT_COLOR_RAISED);
    if (it->bytes_total > 0) {
        double f = (double)it->bytes_done / (double)it->bytes_total;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int fw = (int)(f * w);
        if (fw < 1 && it->bytes_done > 0) fw = 1;
        if (fw > 0) vt_ui_rect(x, y, fw, 4, fill_color);
    } else if (it->state == DL_DOWNLOADING || it->state == DL_RESOLVING) {
        /* The server has not said how big it is: a sweep says "working"
         * without promising a percentage. */
        int seg = 56;
        unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
        int pos = (int)((ms % 1400) * (unsigned)(w - seg) / 1400);
        vt_ui_rect(x + pos, y, seg, 4, fill_color);
    }
}

static void draw_row(const DlItem *it, int y, int selected) {
    if (selected) {
        vt_ui_rect(8, y, VT_SCREEN_W - 16, ROW_H - 4, VT_COLOR_FOCUS);
        vt_ui_rect(8, y, 4, ROW_H - 4, VT_COLOR_ACCENT);
    }
    int tx = 24, ty = y + (ROW_H - 4 - THUMB_H) / 2;
    vita2d_texture *thumb = row_thumb(it);
    if (thumb) {
        float sx = (float)THUMB_W / vita2d_texture_get_width(thumb);
        float sy = (float)THUMB_H / vita2d_texture_get_height(thumb);
        vita2d_draw_texture_scale(thumb, (float)tx, (float)ty, sx, sy);
    } else {
        vt_ui_rect(tx, ty, THUMB_W, THUMB_H, VT_COLOR_RAISED);
    }
    /* What cannot be played yet is dimmed, so the ready ones stand out. */
    if (it->state != DL_DONE)
        vt_ui_rect(tx, ty, THUMB_W, THUMB_H, RGBA8(0, 0, 0, 120));
    if (it->duration[0]) {
        int dw = vt_ui_text_width(0.75f, it->duration);
        vt_ui_rect(tx + THUMB_W - dw - 10, ty + THUMB_H - 22, dw + 8, 20,
                   RGBA8(0, 0, 0, 200));
        vt_ui_text(tx + THUMB_W - dw - 6, ty + THUMB_H - 23, VT_COLOR_TEXT,
                   0.75f, it->duration);
    }
    DlBadge badge = it->state == DL_DONE ? DL_BADGE_DONE
                    : it->state == DL_PAUSED ? DL_BADGE_PAUSED
                    : it->state == DL_FAILED ? DL_BADGE_FAILED
                                             : DL_BADGE_ACTIVE;
    downloads_draw_badge(tx + 6, ty + 6, badge);

    int text_x = tx + THUMB_W + 16;
    int text_w = VT_SCREEN_W - text_x - 24;
    char status[200];
    unsigned status_color;
    status_line(it, status, sizeof status, &status_color);
    begin_column(text_x, text_w);
    vt_ui_text(text_x, y + 8, VT_COLOR_TEXT, 1.0f, it->title);
    vt_ui_text(text_x, y + 33, VT_COLOR_MUTED, 0.85f, it->channel);
    vt_ui_text(text_x, y + 53, status_color, 0.85f, status);
    end_column();
    if (it->state != DL_DONE) draw_progress(it, text_x, y + 77, text_w);
}

void downloads_screen_draw(int list_top, int list_bottom) {
    g_band_top = list_top + HEADER_H;
    g_band_bottom = list_bottom;
    sync_items();
    sync_storage(0);
    draw_header(list_top);
    vita2d_set_clip_rectangle(0, g_band_top, VT_SCREEN_W, g_band_bottom);
    if (g_count == 0) {
        draw_empty(g_band_top);
        return;
    }
    int off = vt_kinetic_draw_offset(&g_scroll.kinetic);
    for (int i = off > 0 ? off / ROW_H : 0; i < g_count; i++) {
        int y = g_band_top + i * ROW_H - off;
        if (y >= g_band_bottom) break;
        draw_row(item_at(i), y, i == g_cursor);
    }
}

/* --- actions -------------------------------------------------------------------- */

typedef enum {
    ACT_NONE = 0,
    ACT_DOWNLOAD,
    ACT_OPEN,
    ACT_PLAY,
    ACT_PAUSE,
    ACT_RESUME,
    ACT_RETRY,
    ACT_RETRY_NOW,
    ACT_PAUSE_ALL,
    ACT_RESUME_ALL,
    ACT_DELETE,
    ACT_DELETE_YES,
    ACT_KEEP
} Act;

static const char *act_label(Act act) {
    switch (act) {
    case ACT_DOWNLOAD:   return vt_str(S_DLM_DOWNLOAD);
    case ACT_OPEN:       return vt_str(S_DLM_OPEN);
    case ACT_PLAY:       return vt_str(S_DLM_PLAY);
    case ACT_PAUSE:      return vt_str(S_DLM_PAUSE);
    case ACT_RESUME:     return vt_str(S_DLM_RESUME);
    case ACT_RETRY:      return vt_str(S_DLM_RETRY);
    case ACT_RETRY_NOW:  return vt_str(S_DLM_RETRY_NOW);
    case ACT_PAUSE_ALL:  return vt_str(S_DLM_PAUSE_ALL);
    case ACT_RESUME_ALL: return vt_str(S_DLM_RESUME_ALL);
    case ACT_DELETE:     return vt_str(S_DLM_DELETE);
    case ACT_DELETE_YES: return vt_str(S_DLM_YES_DELETE);
    case ACT_KEEP:       return vt_str(S_DLM_KEEP);
    default:             return "";
    }
}

/* What ✕ / a second tap does for a row: the one thing it is most likely
 * there for. ACT_NONE (a download being verified, a matter of seconds) opens
 * the menu instead. */
static Act primary_action(const DlItem *it) {
    switch (it->state) {
    case DL_DONE:        return ACT_PLAY;
    case DL_QUEUED:
    case DL_RESOLVING:
    case DL_DOWNLOADING: return ACT_PAUSE;
    case DL_WAITING:     return ACT_RETRY_NOW;
    case DL_PAUSED:      return ACT_RESUME;
    case DL_FAILED:      return ACT_RETRY;
    default:             return ACT_NONE;
    }
}

/* Plays a finished download. The player prefers the local file when there is
 * one (it asks dl_local_path itself) and deals with the file having gone
 * missing; all it needs from here is the video. */
static void play(const DlItem *it) {
    yt_video_t v;
    memset(&v, 0, sizeof v);
    snprintf(v.video_id, sizeof v.video_id, "%s", it->video_id);
    /* The player files what it played under History, as this struct: with no
     * thumbnail URL the History row would show a grey box, and since a video
     * watched again replaces its old entry, a copy that had a picture lost it
     * by being played from here. The same URL the rows of this tab fall back
     * to, and the engine saved the file from. */
    snprintf(v.thumb_url, sizeof v.thumb_url,
             "https://i.ytimg.com/vi/%s/mqdefault.jpg", it->video_id);
    snprintf(v.title, sizeof v.title, "%s", it->title);
    snprintf(v.channel, sizeof v.channel, "%s", it->channel);
    snprintf(v.duration, sizeof v.duration, "%s", it->duration);
    v.age_min = -1;
    player_screen_run(&v);
}

/* The action a row or the menu chose, for the download with this id. Only
 * ACT_PLAY blocks. */
static DlMenuResult execute(Act act, const char *video_id) {
    switch (act) {
    case ACT_PLAY: {
        const DlItem *it = find_item(video_id);
        if (!it) return DLM_NOTHING;
        play(it);
        return DLM_RESYNC;
    }
    case ACT_PAUSE:
        dl_pause(video_id);
        break;
    case ACT_RESUME:
    case ACT_RETRY:
    case ACT_RETRY_NOW:
        dl_resume(video_id);
        break;
    case ACT_PAUSE_ALL:
        dl_pause_all();
        break;
    case ACT_RESUME_ALL:
        dl_resume_all();
        break;
    default:
        break;
    }
    return DLM_NOTHING;
}

/* --- the menu --------------------------------------------------------------------- */

typedef enum { MODAL_NONE = 0, MODAL_MENU, MODAL_CONFIRM } ModalKind;
typedef enum { CTX_VIDEO = 0, CTX_TAB } ModalContext;

static struct {
    ModalKind kind;
    ModalContext context;
    char video_id[YT_VIDEO_ID_LEN]; /* what every choice acts on */
    yt_video_t video;               /* CTX_VIDEO: what "Download" queues */
    Act cursor;                     /* the highlighted entry, by what it IS: the
                                     * list of entries is rebuilt from the live
                                     * state, and an index would drift */
} g_modal;

#define MENU_MAX 8
typedef struct {
    Act act;
    const char *label;
} Entry;

static void entry_add(Entry *e, int *n, Act act) {
    if (*n >= MENU_MAX) return;
    e[*n].act = act;
    e[*n].label = act_label(act);
    (*n)++;
}

/* The entries for what the download is doing right now. Rebuilt every frame
 * and on every press, so a state that changes under an open menu changes the
 * menu — and a choice is always made from the list the user could see. */
static int build_entries(Entry *e) {
    int n = 0;
    if (g_modal.kind == MODAL_CONFIRM) {
        entry_add(e, &n, ACT_DELETE_YES);
        entry_add(e, &n, ACT_KEEP);
        return n;
    }
    const DlItem *it = find_item(g_modal.video_id);
    if (g_modal.context == CTX_VIDEO) {
        if (!it) {
            entry_add(e, &n, ACT_DOWNLOAD);
        } else {
            entry_add(e, &n, ACT_OPEN);
            entry_add(e, &n, ACT_DELETE);
        }
        return n;
    }
    if (!it) return 0;
    Act own = primary_action(it);
    if (own != ACT_NONE) entry_add(e, &n, own);
    if (it->state == DL_WAITING) entry_add(e, &n, ACT_PAUSE);
    /* The global ones only when they would do something this row's own entry
     * does not: with one running download, "Pause all" beside "Pause" is the
     * same button twice. */
    int own_pausable = own == ACT_PAUSE || it->state == DL_WAITING;
    if (g_pausable > (own_pausable ? 1 : 0)) entry_add(e, &n, ACT_PAUSE_ALL);
    if (g_paused > (it->state == DL_PAUSED ? 1 : 0))
        entry_add(e, &n, ACT_RESUME_ALL);
    entry_add(e, &n, ACT_DELETE);
    return n;
}

static int cursor_entry(const Entry *e, int n) {
    for (int i = 0; i < n; i++)
        if (e[i].act == g_modal.cursor) return i;
    /* A confirmation opens on the safe answer, and falls back to it. */
    return g_modal.kind == MODAL_CONFIRM ? n - 1 : 0;
}

static char g_title[96];

/* The title also answers "is there room?" before anything is started: the
 * card's free space for a video that is not downloaded yet. */
static const char *menu_title(void) {
    if (g_modal.kind == MODAL_CONFIRM) return vt_str(S_DLM_CONFIRM_DELETE);
    if (g_modal.context == CTX_VIDEO && !find_item(g_modal.video_id) &&
        g_storage_ok) {
        char free_s[VT_FMT_SIZE_MAX];
        size_text(free_s, g_storage.free_bytes);
        snprintf(g_title, sizeof g_title,
                 vt_str(g_storage.low ? S_DL_FULL_FREE : S_DLM_TITLE_FREE),
                 free_s);
        return g_title;
    }
    return vt_str(S_DLM_TITLE);
}

static VtMenuItem g_menu_items[MENU_MAX];

static void menu_items(const Entry *e, int n) {
    for (int i = 0; i < n; i++) {
        g_menu_items[i].label = e[i].label;
        g_menu_items[i].value = NULL;
        g_menu_items[i].marked = 0;
    }
}

static void close_modal(void) {
    memset(&g_modal, 0, sizeof g_modal);
    /* A finger still down — or the tap that chose an entry — must not go on
     * to act on the list underneath. */
    ui_touch_reset();
}

int downloads_menu_active(void) {
    return g_modal.kind != MODAL_NONE;
}

void downloads_menu_open_video(const yt_video_t *video) {
    sync_items();
    sync_storage(1);
    memset(&g_modal, 0, sizeof g_modal);
    g_modal.kind = MODAL_MENU;
    g_modal.context = CTX_VIDEO;
    snprintf(g_modal.video_id, sizeof g_modal.video_id, "%s", video->video_id);
    g_modal.video = *video;
    ui_touch_reset();
}

void downloads_menu_open_selected(void) {
    sync_items();
    if (g_count == 0) return;
    memset(&g_modal, 0, sizeof g_modal);
    g_modal.kind = MODAL_MENU;
    g_modal.context = CTX_TAB;
    snprintf(g_modal.video_id, sizeof g_modal.video_id, "%s",
             item_at(g_cursor)->video_id);
    ui_touch_reset();
}

static void toast_enqueue_result(DlEnqueue result) {
    switch (result) {
    case DL_ENQ_OK:
        snackbar(vt_str(S_DLT_QUEUED), 0);
        break;
    case DL_ENQ_EXISTS:
        snackbar(vt_str(S_DLT_EXISTS), 0);
        break;
    case DL_ENQ_FULL: {
        /* The number is read now, not from the copy the title used: the card
         * is exactly what changed. */
        DlStorage st;
        char text[128];
        /* The engine also answers FULL when its list is at capacity (128
         * entries), with the card half empty: "memory card is full, only
         * 20 GB free" would be a sentence that contradicts itself. The same
         * test as the engine's own, free space under the reserve, tells the
         * two apart. */
        int have_card = dl_storage(&st) == 0;
        if (have_card && !st.low) {
            snackbar(vt_str(S_DLT_LIST_FULL), 1);
            break;
        }
        if (have_card) {
            char free_s[VT_FMT_SIZE_MAX];
            size_text(free_s, st.free_bytes);
            snprintf(text, sizeof text, vt_str(S_DL_FULL_FREE), free_s);
        } else {
            snprintf(text, sizeof text, "%s", vt_str(S_DL_ERR_DISK_FULL));
        }
        snackbar(text, 1);
        break;
    }
    default:
        snackbar(vt_str(S_DLT_BAD), 1);
        break;
    }
}

static DlMenuResult choose(Act act) {
    /* The id survives close_modal(), which clears the struct it is in. */
    char id[YT_VIDEO_ID_LEN];
    snprintf(id, sizeof id, "%s", g_modal.video_id);
    switch (act) {
    case ACT_DOWNLOAD: {
        yt_video_t video = g_modal.video;
        close_modal();
        toast_enqueue_result(dl_enqueue(&video));
        return DLM_NOTHING;
    }
    case ACT_OPEN:
        close_modal();
        downloads_screen_select(id);
        return DLM_OPEN_DOWNLOADS;
    case ACT_DELETE:
        g_modal.kind = MODAL_CONFIRM;
        g_modal.cursor = ACT_KEEP;
        return DLM_NOTHING;
    case ACT_DELETE_YES:
        close_modal();
        dl_delete(id);
        snackbar(vt_str(S_DLT_DELETED), 0);
        return DLM_NOTHING;
    case ACT_KEEP:
        close_modal();
        return DLM_NOTHING;
    default: {
        close_modal();
        return execute(act, id);
    }
    }
}

DlMenuResult downloads_menu_input(unsigned pressed, unsigned direction,
                                  unsigned touch_flags,
                                  const UiTouchEvent *touch) {
    if (g_modal.kind == MODAL_NONE) return DLM_NOTHING;
    sync_items();
    sync_storage(0);
    Entry e[MENU_MAX];
    int n = build_entries(e);
    /* The download this menu was about is gone (deleted from elsewhere, or
     * dropped by the engine): there is nothing left to choose. */
    if (n == 0) {
        close_modal();
        return DLM_NOTHING;
    }
    int idx = cursor_entry(e, n);
    if ((direction & SCE_CTRL_UP) && idx > 0) idx--;
    if ((direction & SCE_CTRL_DOWN) && idx < n - 1) idx++;

    if (pressed & SCE_CTRL_CIRCLE) {
        close_modal();
        return DLM_NOTHING;
    }
    int chosen = (pressed & SCE_CTRL_CROSS) ? idx : -1;
    if (touch_flags & UI_TOUCH_EVENT_TAP) {
        menu_items(e, n);
        int hit = vt_ui_menu_hit(menu_title(), g_menu_items, n, touch->x,
                                 touch->y);
        if (hit >= 0) {
            chosen = hit;
            idx = hit;
        } else if (hit == VT_MENU_HIT_OUTSIDE) {
            close_modal();
            return DLM_NOTHING;
        }
    }
    g_modal.cursor = e[idx].act;
    if (chosen < 0) return DLM_NOTHING;
    return choose(e[chosen].act);
}

void downloads_menu_draw(void) {
    if (g_modal.kind == MODAL_NONE) return;
    Entry e[MENU_MAX];
    int n = build_entries(e);
    if (n == 0) return; /* closed by the next input call */
    menu_items(e, n);
    vt_ui_menu_draw(menu_title(), g_menu_items, n, cursor_entry(e, n));
}

/* --- the tab's input ------------------------------------------------------------------ */

int downloads_long_press_row(const VtKineticScroll *k, uint64_t now_us,
                             int row_h, int count) {
    /* PRESSED: down and still inside the tap slop. A finger that caught a
     * coasting list is no row's, and one that moved is scrolling. */
    if (k->mode != VT_KINETIC_PRESSED || k->caught) return -1;
    if (now_us - k->down_us < DLS_LONG_PRESS_US) return -1;
    if (k->down_y < 0) return -1;
    int row = (k->down_y + k->down_draw_offset) / row_h;
    return row < count ? row : -1;
}

void downloads_screen_enter(void) {
    g_synced = 0;
    sync_items();
    sync_storage(1);
}

int downloads_screen_count(void) {
    sync_items();
    return g_count;
}

void downloads_screen_select(const char *video_id) {
    sync_items();
    int at = display_index_of(video_id);
    if (at < 0) return;
    g_cursor = at;
    remember_cursor();
    /* Not in step with the scroll code on purpose: the next scroll step reads
     * the difference as "the selection moved, show it". */
    g_scroll.synced_cursor = -1;
}

int downloads_screen_input(unsigned pressed, unsigned direction,
                           unsigned touch_flags, const UiTouchEvent *touch,
                           int list_top, int list_bottom) {
    g_band_top = list_top + HEADER_H;
    g_band_bottom = list_bottom;
    sync_items();
    if (g_count == 0) {
        /* Nothing to scroll; drop whatever a finger had started on the rows
         * that just went away. */
        memset(&g_scroll, 0, sizeof g_scroll);
        g_cursor = 0;
        g_cursor_id[0] = '\0';
        return 0;
    }

    if (direction && cursor_into_view(&g_cursor)) direction = 0;
    if ((direction & SCE_CTRL_UP) && g_cursor > 0) g_cursor--;
    if ((direction & SCE_CTRL_DOWN) && g_cursor < g_count - 1) g_cursor++;
    if (direction & SCE_CTRL_RIGHT) {
        g_cursor += VISIBLE_ROWS;
        if (g_cursor > g_count - 1) g_cursor = g_count - 1;
    }
    if (direction & SCE_CTRL_LEFT) {
        g_cursor -= VISIBLE_ROWS;
        if (g_cursor < 0) g_cursor = 0;
    }

    int row = scroll_step(touch_flags, touch);
    int act_row = -1;
    int held = downloads_long_press_row(&g_scroll.kinetic,
                                        sceKernelGetProcessTimeWide(), ROW_H,
                                        g_count);
    if (held >= 0) {
        g_cursor = held;
        remember_cursor();
        downloads_menu_open_selected();
        return 0;
    }
    if (row >= 0) {
        if (row == g_cursor) act_row = row;
        else g_cursor = row;
    }
    /* Cross and SELECT with the selection carried off screen by a drag or a
     * fling pick a row on screen first, as a direction does (list_screen.c):
     * neither plays nor opens a menu for a row the user cannot see. */
    if ((pressed & (SCE_CTRL_CROSS | SCE_CTRL_SELECT)) &&
        !cursor_into_view(&g_cursor)) {
        if (pressed & SCE_CTRL_CROSS) act_row = g_cursor;
        else if (act_row < 0) {
            remember_cursor();
            downloads_menu_open_selected();
            return 0;
        }
    }
    remember_cursor();
    if (act_row < 0) return 0;

    const DlItem *it = item_at(act_row);
    Act act = primary_action(it);
    if (act == ACT_NONE) {
        downloads_menu_open_selected();
        return 0;
    }
    char id[YT_VIDEO_ID_LEN];
    snprintf(id, sizeof id, "%s", it->video_id);
    return execute(act, id) == DLM_RESYNC;
}

const char *downloads_screen_hint(char *buf, size_t cap) {
    sync_items();
    if (g_count == 0) return vt_str(S_HINT_SETTINGS_PLAIN);
    Act act = primary_action(item_at(g_cursor));
    if (act == ACT_NONE) return vt_str(S_HINT_DL_ROW_PLAIN);
    snprintf(buf, cap, vt_str(S_HINT_DL_ROW), act_label(act));
    return buf;
}
