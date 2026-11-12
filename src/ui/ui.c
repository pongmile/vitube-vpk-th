/* ViTube — vita2d runtime, font and small draw helpers. */
#include "ui/strings.h"
#include "ui/ui.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "app.h"
#include "log.h"
#include "ui/thai_shape.h"

/* Text is drawn with FreeType (vita2d_font), not the system PGF font, and
 * with ONE font instance per size. Why both halves matter:
 *
 * PGF (and PVF alike) rasterise every glyph once into a shared atlas at a
 * fixed size; their "scale" argument then only stretches that quad, and the
 * atlas is sampled with a POINT minification filter. Every scale below 1.0 —
 * this app used 0.7/0.75/0.85/0.9/0.95 — therefore dropped whole pixel rows
 * out of each glyph, which is why the small text was mushy on hardware while
 * the 1.0 text was crisp (reported 2026-08-28).
 *
 * The FreeType path does rasterise at the requested pixel size, but its atlas
 * is keyed by glyph index alone: within one instance the FIRST size a glyph
 * is drawn at wins, and every other size is resampled from it — the same bug
 * again. A separate instance per size is what makes each draw texel-exact.
 *
 * Sizes are em pixels and deliberately close to the old PGF metrics so the
 * existing layouts still hold; they are the knob to turn if anything looks
 * too large or too small on the screen. */
typedef enum {
    VT_FONT_SMALL, VT_FONT_BODY, VT_FONT_LARGE, VT_FONT_HUGE, VT_FONT_COUNT
} VtFontSlot;
static const int g_font_px[VT_FONT_COUNT] = { 14, 18, 21, 36 };
static vita2d_font *g_fonts[VT_FONT_COUNT];
/* Every family by glyph_source()'s answer. Main is g_fonts above, loaded up
 * front; the others load on demand — see fallback_font() below. */
#define SRC_MAIN 0
#define SRC_EMOJI 1
#define SRC_THAI 2
#define SRC_COUNT 3
static const char *const g_font_path[SRC_COUNT] = {
    "app0:font.ttf", "app0:emoji.ttf", "app0:thai.ttf",
};
static vita2d_font *g_fallback[SRC_COUNT][VT_FONT_COUNT];
static char g_fallback_failed[SRC_COUNT][VT_FONT_COUNT];

/* FreeType faces used only to ask which code points a font can draw;
 * vita2d keeps its own faces for the actual rendering. See glyph_source(). */
static FT_Library g_ft;
static FT_Face g_face[SRC_COUNT];

static void open_coverage_faces(void) {
    if (FT_Init_FreeType(&g_ft) != 0) { g_ft = NULL; return; }
    for (int i = 0; i < SRC_COUNT; i++)
        if (FT_New_Face(g_ft, g_font_path[i], 0, &g_face[i]) != 0)
            g_face[i] = NULL;
    vt_log("ui: coverage faces main=%d emoji=%d thai=%d",
           g_face[SRC_MAIN] != NULL, g_face[SRC_EMOJI] != NULL,
           g_face[SRC_THAI] != NULL);
}

/* Kept only as a fallback: if the bundled TTF cannot be loaded the app still
 * runs with the system font rather than showing no text at all. */
static vita2d_pgf *g_pgf;

/* DejaVu Sans' ascender is 1901/2048 em, so the baseline for a line whose
 * top is y sits ~0.93 em below it. vita2d_font draws from the baseline. */
#define FONT_BASELINE(px) (((px) * 93) / 100)
#define FONT_LINE_H(px) (((px) * 7) / 6)

static VtFontSlot slot_for(float scale) {
    if (scale <= 0.87f) return VT_FONT_SMALL;
    if (scale <= 1.05f) return VT_FONT_BODY;
    if (scale <= 1.5f) return VT_FONT_LARGE;
    return VT_FONT_HUGE;
}

int vt_ui_init(void) {
    int ret = vita2d_init();
    /* libvita2d can return 1 even when internal GXM calls failed */
    if (ret <= 0 || vita2d_get_context() == NULL ||
        vita2d_get_current_fb() == NULL) return -1;
    vita2d_set_clear_color(VT_COLOR_BG);
    int loaded = 0;
    for (int i = 0; i < VT_FONT_COUNT; i++) {
        g_fonts[i] = vita2d_load_font_file("app0:font.ttf");
        if (g_fonts[i]) loaded++;
    }
    if (loaded != VT_FONT_COUNT) {
        vt_log("ui: font.ttf loaded %d/%d instances, using system font",
               loaded, VT_FONT_COUNT);
        for (int i = 0; i < VT_FONT_COUNT; i++) {
            if (g_fonts[i]) vita2d_free_font(g_fonts[i]);
            g_fonts[i] = NULL;
        }
        g_pgf = vita2d_load_default_pgf();
        if (!g_pgf) return -1;
    } else {
        vt_log("ui: font.ttf loaded, %d sizes", VT_FONT_COUNT);
    }
    open_coverage_faces();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    return 0;
}

void vt_ui_term(void) {
    vita2d_fini();
    for (int i = 0; i < VT_FONT_COUNT; i++) {
        if (g_fonts[i]) vita2d_free_font(g_fonts[i]);
        g_fonts[i] = NULL;
        for (int f = 0; f < SRC_COUNT; f++) {
            if (g_fallback[f][i]) vita2d_free_font(g_fallback[f][i]);
            g_fallback[f][i] = NULL;
        }
    }
    if (g_pgf) vita2d_free_pgf(g_pgf);
    g_pgf = NULL;
    for (int f = 0; f < SRC_COUNT; f++) {
        if (g_face[f]) FT_Done_Face(g_face[f]);
        g_face[f] = NULL;
    }
    if (g_ft) FT_Done_FreeType(g_ft);
    g_ft = NULL;
}

/* DejaVu Sans covers Latin, Greek and Cyrillic but no emoji, and YouTube
 * titles are full of them — they came out as .notdef boxes on hardware
 * ("OK, ich lag falsch! □□□□□□□", 2026-08-28). Emoji therefore get a second
 * font, and each run of text is drawn with whichever font can render it.
 *
 * Monochrome Noto Emoji on purpose: the colour build stores its glyphs as
 * CBDT bitmaps, and vita2d's text path rasterises 8-bit coverage and tints
 * it, so colour glyphs cannot come through. Emoji render as pictographs in
 * the surrounding text colour.
 *
 * Thai is a third family for the same reason: DejaVu has no Thai letters.
 * assets/thai.ttf is Noto Sans Thai with its mark positioning baked into
 * extra glyphs — see thai_shape.h.
 *
 * Loaded lazily per size — the emoji file is ~2 MB and each instance keeps
 * its own face, so sizes that never meet an emoji never pay for one. */
static vita2d_font *fallback_font(int src, VtFontSlot s) {
    if (!g_fallback[src][s] && !g_fallback_failed[src][s]) {
        g_fallback[src][s] = vita2d_load_font_file(g_font_path[src]);
        if (!g_fallback[src][s]) {
            g_fallback_failed[src][s] = 1;
            vt_log("ui: %s could NOT be loaded", g_font_path[src]);
        }
    }
    return g_fallback[src][s];
}

/* Decodes one UTF-8 sequence. Returns its byte length (1 for an invalid
 * lead byte, so the caller always advances) and writes the code point. */
static int utf8_next(const unsigned char *p, unsigned int *cp) {
    int len = *p >= 0xf0 ? 4 : *p >= 0xe0 ? 3 : *p >= 0xc0 ? 2 : 1;
    for (int i = 1; i < len; i++)
        if ((p[i] & 0xc0) != 0x80) { len = 1; break; } /* truncated/invalid */
    switch (len) {
    case 4: *cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) |
                  ((p[2] & 0x3fu) << 6) | (p[3] & 0x3fu); break;
    case 3: *cp = ((p[0] & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) |
                  (p[2] & 0x3fu); break;
    case 2: *cp = ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu); break;
    default: *cp = p[0]; break;
    }
    return len;
}

/* Which font draws a given code point — asked of the fonts, not guessed by
 * range. An earlier version decided by range ("everything from U+1F000 up is
 * emoji") and got it wrong twice over: plenty of emoji (⚡ U+26A1, ⭐ U+2B50,
 * ✅ U+2705) sit below that line, and DejaVu really does carry outlines for a
 * few (😀 U+1F600, ❤ U+2764), so whichever font was asked first won.
 *
 * Now the blocks below only decide the ORDER of the question; coverage is
 * verified either way, so a wrong guess can never lose a character. Inside
 * the emoji blocks the emoji font is preferred because its shapes are the
 * better ones there. */
static int in_emoji_block(unsigned int cp) {
    return (cp >= 0x1f000 && cp <= 0x1faff) ||
           (cp >= 0x2600 && cp <= 0x27bf) ||
           (cp >= 0x2b00 && cp <= 0x2bff);
}

/* Thai, and the private-use glyphs vt_thai_shape() rewrites it to. */
static int in_thai_block(unsigned int cp) {
    return (cp >= 0x0e00 && cp <= 0x0e7f) || vt_thai_private_use(cp);
}

/* See the block comment above text_runs(): the emoji font's astral glyphs are
 * reachable through a private-use remap; nothing else astral is drawable. */
#define EMOJI_ASTRAL_LO 0x1f000u
#define EMOJI_ASTRAL_HI 0x1faffu
#define EMOJI_PUA_BASE 0xe000u

static int glyph_source(unsigned int cp) {
    if (cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f)) return -1;
    if (cp < 0x80) return 0; /* ASCII: never worth a lookup */
    int have[SRC_COUNT];
    for (int i = 0; i < SRC_COUNT; i++)
        have[i] = g_face[i] && FT_Get_Char_Index(g_face[i], cp);
    int src;
    if (in_thai_block(cp))
        src = have[SRC_THAI] ? SRC_THAI : have[SRC_MAIN] ? SRC_MAIN : -1;
    else if (in_emoji_block(cp))
        src = have[SRC_EMOJI] ? SRC_EMOJI : have[SRC_MAIN] ? SRC_MAIN : -1;
    else
        src = have[SRC_MAIN] ? SRC_MAIN : have[SRC_EMOJI] ? SRC_EMOJI
            : have[SRC_THAI] ? SRC_THAI : -1;
    /* Without coverage faces, fall back to a plain range guess. */
    if (!g_face[SRC_MAIN] && !g_face[SRC_EMOJI] && !g_face[SRC_THAI])
        src = cp >= 0x1f000 ? SRC_EMOJI : SRC_MAIN;
    /* vita2d's UTF-8 reader stops at three bytes: an astral code point it
     * is handed comes out as four .notdef boxes. Only the emoji block has a
     * PUA remap (text_runs), so every other astral code point — the 471
     * mathematical letters and the like DejaVu covers, emoji tag characters
     * — is dropped instead, which is also the right rendering for a
     * monochrome subdivision-flag fallback. */
    if (cp > 0xffff &&
        !(src == SRC_EMOJI && cp >= EMOJI_ASTRAL_LO && cp <= EMOJI_ASTRAL_HI))
        src = -1;
    /* A code point neither font can draw is worth one line — it is the only
     * way a missing glyph shows up other than a box on screen. */
    if (src < 0) {
        static unsigned int seen[24];
        static int seen_n;
        for (int i = 0; i < seen_n; i++)
            if (seen[i] == cp) return src;
        if (seen_n < (int)(sizeof seen / sizeof *seen)) {
            seen[seen_n++] = cp;
            vt_log("ui: no glyph for U+%04X in any font", cp);
        }
    }
    return src;
}

/* vita2d decodes text with utf8_to_ucs2() — UCS-2, sixteen bits — so it
 * cannot represent anything above U+FFFF at all. That is where most emoji
 * live (😲 U+1F632, 💀 U+1F480, 👉 U+1F449), and it is why they came out as
 * boxes while ❌ U+274C, which is inside the BMP, drew correctly: the font
 * was never the problem, the library's character type was.
 *
 * assets/emoji.ttf therefore carries a second cmap entry for each of those
 * glyphs in the private use area, at PUA + (cp - LO). Translating here puts
 * the code point back inside the sixteen bits vita2d can carry. Both halves
 * of this must move together: regenerate the font's PUA entries if these
 * constants ever change. */
static int utf8_encode(char *dst, unsigned int cp) {
    if (cp < 0x80) { dst[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        dst[0] = (char)(0xc0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xe0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        dst[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    dst[0] = (char)(0xf0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    dst[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* What text_runs() actually draws for a code point. Styled Unicode letters
 * (Mathematical Alphanumerics, common in titles) fold to their plain ASCII
 * letter: no font has them, and 'NEW' beats a row of boxes. Anything else
 * no font covers becomes U+25A1 (a box) instead of vanishing — a CJK title
 * used to draw as an empty string, with no sign anything was missing. Only
 * the joiners/selectors that are meant to be invisible are still dropped
 * (src -1). */
#define PLACEHOLDER_CP 0x25a1u
static unsigned int drawable_cp(unsigned int cp, int *src) {
    if (cp >= 0x1d400 && cp <= 0x1d6a3) {
        /* Thirteen alphabets of 52 letters (A-Z, a-z), back to back. */
        unsigned int i = (cp - 0x1d400) % 52;
        cp = i < 26 ? 'A' + i : 'a' + (i - 26);
    } else if (cp >= 0x1d7ce && cp <= 0x1d7ff) {
        cp = '0' + (cp - 0x1d7ce) % 10;
    }
    *src = glyph_source(cp);
    if (*src < 0 && cp != 0x200d && !(cp >= 0xfe00 && cp <= 0xfe0f)) {
        cp = PLACEHOLDER_CP;
        *src = glyph_source(cp);
    }
    return cp;
}

/* Single path behind both drawing and measuring, so a centred string can
 * never be measured with different metrics than it is drawn with. */
static int text_runs(int x, int y, unsigned int color, VtFontSlot s,
                     const char *text, int draw) {
    /* Thai goes through the cluster substitution first (thai_shape.h), but
     * only when thai.ttf is there to draw the private-use code points it
     * produces. The output is never longer than the input. */
    char shaped[512];
    char *heap = NULL;
    if (g_face[SRC_THAI] && vt_thai_present(text)) {
        size_t cap = strlen(text) + 1;
        char *buf = cap <= sizeof shaped ? shaped : (heap = malloc(cap));
        if (buf) {
            vt_thai_shape(buf, cap, text);
            text = buf;
        }
    }
    int px = g_font_px[s];
    int baseline = y + FONT_BASELINE(px);
    int cursor = x;
    const unsigned char *p = (const unsigned char *)text;
    char run[320];
    while (*p) {
        unsigned int cp = 0;
        int len = utf8_next(p, &cp);
        int src;
        cp = drawable_cp(cp, &src);
        if (src < 0) { p += len; continue; }
        /* Collect the longest stretch that one font can draw. */
        size_t n = 0;
        while (*p && n + 4 < sizeof run) {
            unsigned int next_cp = 0;
            int next_len = utf8_next(p, &next_cp);
            int next_src;
            next_cp = drawable_cp(next_cp, &next_src);
            if (next_src < 0) { p += next_len; continue; } /* drop, keep run */
            if (next_src != src) break;
            unsigned int draw_cp = next_cp;
            if (src == SRC_EMOJI && next_cp >= EMOJI_ASTRAL_LO &&
                next_cp <= EMOJI_ASTRAL_HI)
                draw_cp = EMOJI_PUA_BASE + (next_cp - EMOJI_ASTRAL_LO);
            n += (size_t)utf8_encode(run + n, draw_cp);
            p += next_len;
        }
        run[n] = '\0';
        if (!n) continue;
        vita2d_font *f = src == SRC_MAIN ? g_fonts[s] : fallback_font(src, s);
        if (!f) f = g_fonts[s]; /* no fallback font: the box is still better
                                 * than silently losing the character */
        if (draw) vita2d_font_draw_text(f, cursor, baseline, color, px, run);
        cursor += vita2d_font_text_width(f, px, run);
    }
    free(heap);
    return cursor - x;
}

void vt_ui_text(int x, int y, unsigned int color, float scale,
                const char *text) {
    VtFontSlot s = slot_for(scale);
    if (!g_fonts[s]) { /* system-font fallback: the old, blurrier path */
        vita2d_pgf_draw_text(g_pgf, x, y + (int)(17.0f * scale), color, scale,
                             text);
        return;
    }
    text_runs(x, y, color, s, text, 1);
}

int vt_ui_text_width(float scale, const char *text) {
    VtFontSlot s = slot_for(scale);
    if (!g_fonts[s]) return vita2d_pgf_text_width(g_pgf, scale, (char *)text);
    return text_runs(0, 0, 0, s, text, 0);
}

void vt_ui_text_clipped(int x, int y, unsigned int color, float scale,
                        int max_w, const char *text) {
    VtFontSlot s = slot_for(scale);
    int line_h = g_fonts[s] ? FONT_LINE_H(g_font_px[s]) : (int)(26 * scale);
    /* The clip is there for the width. Vertically it leaves a fifth of an em
     * of slack on both sides: Thai stacks a tone mark over an upper vowel up
     * to 1.08 em above the baseline, where the line box ends at 0.93, and
     * hangs vowels under ฎ ฏ down to 0.43 em below it. */
    int pad = g_fonts[s] ? g_font_px[s] / 5 : 0;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(x, y - pad, x + max_w, y + line_h + pad);
    vt_ui_text(x, y, color, scale, text);
    vita2d_disable_clipping();
}

void vt_ui_rect(int x, int y, int w, int h, unsigned int color) {
    vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

void vt_ui_top_bar(const char *subtitle, int logged_in) {
    vt_ui_rect(0, 0, VT_SCREEN_W, 44, VT_COLOR_SURFACE);
    vt_ui_rect(0, 44, VT_SCREEN_W, 1, VT_COLOR_BORDER);
    vt_ui_rect(16, 12, 34, 22, VT_COLOR_ACCENT);
    /* small play triangle in the logo block */
    vita2d_draw_line(28.0f, 17.0f, 28.0f, 29.0f, VT_COLOR_TEXT);
    vt_ui_text(58, 10, VT_COLOR_TEXT, 1.0f, "ViTube");
    if (subtitle && subtitle[0])
        vt_ui_text_clipped(190, 10, VT_COLOR_MUTED, 1.0f, 480, subtitle);

    char right[64];
    SceDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    int battery = scePowerGetBatteryLifePercent();
    snprintf(right, sizeof right, "%s  %02d:%02d  %d%%",
             logged_in ? "\xe2\x97\x8f" : "", now.hour, now.minute,
             battery < 0 ? 0 : battery);
    int w = vt_ui_text_width(1.0f, right);
    vt_ui_text(VT_SCREEN_W - 16 - w, 10, logged_in ? VT_COLOR_OK : VT_COLOR_MUTED,
               1.0f, right);
}

/* --- PlayStation face buttons ------------------------------------------- */

/* Hint strings still carry the four UTF-8 shapes (U+2715 ✕, U+25CB ○,
 * U+25A1 □, U+25B3 △) so the literals stay readable at the call sites — but
 * they are no longer looked up in the font. The Vita's system font has no
 * glyph for U+2715 at all: on hardware it came out as "_" (2026-08-28), and
 * the other three rendered as generic geometry rather than buttons. Drawn as
 * vectors they are always present, sharp at any size, and carry the console's
 * own button colours so they are recognisable at a glance. */
#define BTN_CROSS 0
#define BTN_CIRCLE 1
#define BTN_SQUARE 2
#define BTN_TRIANGLE 3

static const unsigned int g_btn_color[4] = {
    RGBA8(122, 167, 255, 255), /* ✕ blue    */
    RGBA8(255, 122, 122, 255), /* ○ red     */
    RGBA8(242, 140, 216, 255), /* □ magenta */
    RGBA8(111, 215, 168, 255), /* △ green   */
};

/* vita2d_draw_line is one pixel wide; stroke by stacking offset lines. */
static void stroke_line(float x0, float y0, float x1, float y1, int t,
                        unsigned int color) {
    for (int i = 0; i < t; i++)
        vita2d_draw_line(x0, y0 + i, x1, y1 + i, color);
}

/* Returns the marker's UTF-8 length (0 if `s` is not one) and the button id. */
static int face_button_at(const char *s, int *which) {
    /* s[1] is checked before s[2] is read: a truncated 0xe2 at the very end
     * of the string would otherwise read one byte past the terminator. */
    if ((unsigned char)s[0] != 0xe2 || !s[1]) return 0;
    unsigned char b = (unsigned char)s[1], c = (unsigned char)s[2];
    if (b == 0x9c && c == 0x95) { *which = BTN_CROSS; return 3; }
    if (b == 0x97 && c == 0x8b) { *which = BTN_CIRCLE; return 3; }
    if (b == 0x96 && c == 0xa1) { *which = BTN_SQUARE; return 3; }
    if (b == 0x96 && c == 0xb3) { *which = BTN_TRIANGLE; return 3; }
    return 0;
}

/* Draws one button with its symbol box `d` px tall, top-left at (x, y).
 * `bg` is the colour behind it — needed to hollow out the circle. Returns
 * the advance width including the gap to the following text. */
static int draw_face_button(int x, int y, int d, int which, unsigned int bg) {
    unsigned int col = g_btn_color[which];
    float r = d / 2.0f;
    float cx = x + r, cy = y + r;
    int t = d >= 16 ? 2 : 1;
    switch (which) {
    case BTN_CROSS:
        stroke_line(cx - r * 0.6f, cy - r * 0.6f, cx + r * 0.6f, cy + r * 0.6f,
                    t, col);
        stroke_line(cx + r * 0.6f, cy - r * 0.6f, cx - r * 0.6f, cy + r * 0.6f,
                    t, col);
        break;
    case BTN_CIRCLE:
        vita2d_draw_fill_circle(cx, cy, r * 0.72f, col);
        vita2d_draw_fill_circle(cx, cy, r * 0.72f - t, bg);
        break;
    case BTN_SQUARE: {
        float a = r * 0.62f;
        vt_ui_rect((int)(cx - a), (int)(cy - a), (int)(a * 2), t, col);
        vt_ui_rect((int)(cx - a), (int)(cy + a - t), (int)(a * 2), t, col);
        vt_ui_rect((int)(cx - a), (int)(cy - a), t, (int)(a * 2), col);
        vt_ui_rect((int)(cx + a - t), (int)(cy - a), t, (int)(a * 2), col);
        break;
    }
    case BTN_TRIANGLE: {
        float a = r * 0.72f;
        stroke_line(cx, cy - a, cx - a, cy + a * 0.7f, t, col);
        stroke_line(cx, cy - a, cx + a, cy + a * 0.7f, t, col);
        stroke_line(cx - a, cy + a * 0.7f, cx + a, cy + a * 0.7f, t, col);
        break;
    }
    default: break;
    }
    return d + 3;
}

/* Draws `text` at (x, y), substituting a drawn button for each face-button
 * marker. Pass draw = 0 to only measure. Returns the total width. */
static int text_with_buttons(int x, int y, unsigned int color, float scale,
                             const char *text, unsigned int bg, int draw) {
    int d = 16; /* symbol box, close to the hint bar's cap height */
    int cursor = x;
    const char *run = text;
    char chunk[256];
    for (const char *p = text;; p++) {
        int which = 0;
        int marker = *p ? face_button_at(p, &which) : 0;
        if (*p && !marker) continue;
        if (p > run) { /* flush the plain-text run before this marker */
            size_t n = (size_t)(p - run);
            if (n >= sizeof chunk) n = sizeof chunk - 1;
            memcpy(chunk, run, n);
            chunk[n] = '\0';
            if (draw) vt_ui_text(cursor, y, color, scale, chunk);
            cursor += vt_ui_text_width(scale, chunk);
        }
        if (!*p) break;
        if (draw) draw_face_button(cursor, y + 1, d, which, bg);
        cursor += d + 3;
        p += marker - 1; /* the loop's p++ consumes the last marker byte */
        run = p + 1;
    }
    return cursor - x;
}

void vt_ui_hint_bar(const char *hint) {
    vt_ui_rect(0, VT_SCREEN_H - 36, VT_SCREEN_W, 36, VT_COLOR_SURFACE);
    vt_ui_rect(0, VT_SCREEN_H - 37, VT_SCREEN_W, 1, VT_COLOR_BORDER);
    text_with_buttons(16, VT_SCREEN_H - 30, VT_COLOR_MUTED, 0.9f, hint,
                      VT_COLOR_SURFACE, 1);
}

#define TAB_BAR_H 34
#define TAB_BAR_Y 44 /* directly below the 44px top bar */

#define MENU_ROW_H 44

void vt_ui_menu_draw(const char *title, const VtMenuItem *items, int count,
                     int cursor) {
    /* A modal is full-screen by definition; the caller may still have the
     * list clip enabled (the settings tab draws inside it), which cut the
     * top of the Region picker and left the header undimmed. */
    vita2d_disable_clipping();
    vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, RGBA8(0, 0, 0, 160));
    int content_w = vt_ui_text_width(0.9f, title);
    for (int i = 0; i < count; i++) {
        int w = vt_ui_text_width(1.0f, items[i].label);
        if (items[i].value) w += 24 + vt_ui_text_width(1.0f, items[i].value);
        if (items[i].marked) w += 28;
        if (w > content_w) content_w = w;
    }
    int box_w = content_w + 80;
    if (box_w < 300) box_w = 300;
    if (box_w > VT_SCREEN_W - 80) box_w = VT_SCREEN_W - 80;
    int box_h = 46 + count * MENU_ROW_H;
    int box_x = (VT_SCREEN_W - box_w) / 2;
    int box_y = (VT_SCREEN_H - box_h) / 2;
    vt_ui_rect(box_x, box_y, box_w, box_h, VT_COLOR_SURFACE);
    vt_ui_text(box_x + 20, box_y + 12, VT_COLOR_MUTED, 0.9f, title);
    for (int i = 0; i < count; i++) {
        int y = box_y + 44 + i * MENU_ROW_H;
        if (i == cursor)
            vt_ui_rect(box_x + 8, y, box_w - 16, MENU_ROW_H - 6,
                       VT_COLOR_FOCUS);
        vt_ui_text(box_x + 24, y + 8, VT_COLOR_TEXT, 1.0f, items[i].label);
        int right = box_x + box_w - 24;
        if (items[i].marked) {
            int dw = vt_ui_text_width(1.0f, "\xe2\x97\x8f");
            vt_ui_text(right - dw, y + 8, VT_COLOR_ACCENT, 1.0f,
                       "\xe2\x97\x8f");
            right -= dw + 12;
        }
        if (items[i].value) {
            int vw = vt_ui_text_width(1.0f, items[i].value);
            vt_ui_text(right - vw, y + 8, VT_COLOR_MUTED, 1.0f,
                       items[i].value);
        }
    }
}

int vt_ui_tab_bar(const char *const *tabs, int count, int active) {
    vt_ui_rect(0, TAB_BAR_Y, VT_SCREEN_W, TAB_BAR_H, VT_COLOR_BG);
    vt_ui_rect(0, TAB_BAR_Y + TAB_BAR_H - 1, VT_SCREEN_W, 1, VT_COLOR_BORDER);
    int x = 16;
    for (int i = 0; i < count; i++) {
        int w = vt_ui_text_width(0.95f, tabs[i]) + 28;
        if (i == active) {
            vt_ui_rect(x, TAB_BAR_Y + 4, w, TAB_BAR_H - 8, VT_COLOR_RAISED);
            vt_ui_rect(x, TAB_BAR_Y + TAB_BAR_H - 3, w, 3, VT_COLOR_ACCENT);
        }
        vt_ui_text(x + 14, TAB_BAR_Y + 7, i == active ? VT_COLOR_TEXT
                                                       : VT_COLOR_MUTED,
                   0.95f, tabs[i]);
        x += w + 8;
    }
    const char *arrows = "L1 \xe2\x97\x84 \xe2\x96\xba R1";
    int aw = vt_ui_text_width(0.85f, arrows);
    vt_ui_text(VT_SCREEN_W - 16 - aw, TAB_BAR_Y + 9, VT_COLOR_FAINT, 0.85f,
              arrows);
    return TAB_BAR_H;
}

unsigned int vt_nav_repeat(VtNavRepeat *nav, unsigned int pressed,
                           unsigned int held, unsigned char lx,
                           unsigned char ly) {
    unsigned int direction = held & (SCE_CTRL_UP | SCE_CTRL_DOWN |
                                     SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
    if (!direction) {
        /* left analog stick */
        if (ly < 64) direction = SCE_CTRL_UP;
        else if (ly > 191) direction = SCE_CTRL_DOWN;
        else if (lx < 64) direction = SCE_CTRL_LEFT;
        else if (lx > 191) direction = SCE_CTRL_RIGHT;
    }
    uint64_t now = sceKernelGetProcessTimeWide();
    if (!direction) {
        nav->direction = 0;
        return 0;
    }
    if (pressed & direction) {
        nav->direction = direction;
        nav->next_time_us = now + 280000;
        return direction;
    }
    if (direction != nav->direction) {
        nav->direction = direction;
        nav->next_time_us = now + 280000;
        return direction;
    }
    if (now >= nav->next_time_us) {
        nav->next_time_us = now + 95000;
        return direction;
    }
    return 0;
}

typedef struct {
    VtTaskFn fn;
    void *arg;
    volatile int done;
    int result;
} TaskState;

static void *task_main(void *opaque) {
    TaskState *task = opaque;
    task->result = task->fn(task->arg);
    __sync_synchronize();
    task->done = 1;
    return NULL;
}

int vt_ui_run_task(const char *label, VtTaskFn fn, void *arg,
                   volatile int *cancel_flag) {
    TaskState task = { fn, arg, 0, -999 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, task_main, &task) != 0) return -999;

    SceCtrlData ctrl, previous = {0};
    sceCtrlPeekBufferPositive(0, &previous, 1);
    int cancelled = 0;
    unsigned frame = 0;
    while (!task.done) {
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        unsigned pressed = ctrl.buttons & ~previous.buttons;
        previous = ctrl;
        if ((pressed & SCE_CTRL_CIRCLE) && cancel_flag && !cancelled) {
            *cancel_flag = 1;
            cancelled = 1;
        }
        vt_ui_busy_frame(cancelled ? vt_str(S_CANCELLING) : label, NULL,
                         frame, !cancelled && cancel_flag);
        frame++;
    }
    pthread_join(thread, NULL);
    return task.result;
}

void vt_ui_busy_frame(const char *label, const char *detail, unsigned frame,
                      int cancel_hint) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    int w = vt_ui_text_width(1.1f, label);
    vt_ui_text((VT_SCREEN_W - w) / 2, 240, VT_COLOR_TEXT, 1.1f, label);
    /* simple animated dots bar */
    int cx = VT_SCREEN_W / 2 - 60 + (int)(frame % 60) * 2;
    vt_ui_rect(VT_SCREEN_W / 2 - 60, 290, 120, 4, VT_COLOR_RAISED);
    vt_ui_rect(cx, 290, 16, 4, VT_COLOR_ACCENT);
    if (detail && detail[0]) {
        int dw = vt_ui_text_width(0.9f, detail);
        vt_ui_text((VT_SCREEN_W - dw) / 2, 312, VT_COLOR_MUTED, 0.9f, detail);
    }
    if (cancel_hint) vt_ui_hint_bar(vt_str(S_CANCEL_HINT));
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
    vita2d_swap_buffers();
}

void vt_ui_toast(const char *line1, const char *line2, int frames) {
    for (int i = 0; i < frames; i++) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        int w1 = vt_ui_text_width(1.1f, line1);
        vt_ui_text((VT_SCREEN_W - w1) / 2, 230, VT_COLOR_TEXT, 1.1f, line1);
        if (line2 && line2[0]) {
            int w2 = vt_ui_text_width(0.9f, line2);
            vt_ui_text((VT_SCREEN_W - w2) / 2, 270, VT_COLOR_MUTED, 0.9f, line2);
        }
        vita2d_end_drawing();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
    }
}
