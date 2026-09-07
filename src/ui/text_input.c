/* ViTube — raw sceIme text input.
 * Adapted for ViTube from VitaWave (c) spyro-98, GPL-3.0-only.
 * https://github.com/spyro-98/VitaWave
 *
 * sceIme (unlike SceImeDialog) only shows the keyboard and leaves the text
 * field rendering to the app — SceImeDialog/CommonDialog init breaks the
 * h264_vita application profile on real hardware. All state pointed to by
 * SceImeParam must stay valid until sceImeClose(): static buffers only. */
#include "ui/text_input.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/libime.h>
#include <psp2/sysmodule.h>
#include <psp2/types.h>
#include <vita2d.h>

#include "app.h"
#include "ui/strings.h"
#include "ui/ui.h"

#define TEXT_INPUT_MAX_CHARS 255
#define IME_TIMEOUT_FRAMES (60 * 120)
#define IME_LIVE_UTF8_CAPACITY \
    ((TEXT_INPUT_MAX_CHARS + SCE_IME_MAX_PREEDIT_LENGTH) * 3 + 1)

static int g_ime_module_loaded = 0;
static SceUInt32 g_ime_work[SCE_IME_WORK_BUFFER_SIZE / sizeof(SceUInt32)]
    __attribute__((aligned(64)));
static SceWChar16 g_ime_initial[TEXT_INPUT_MAX_CHARS + 1];
static SceWChar16 g_ime_input[TEXT_INPUT_MAX_CHARS + SCE_IME_MAX_PREEDIT_LENGTH + 1];
static volatile int g_ime_entered = 0;
static volatile int g_ime_cancelled = 0;
static volatile SceUInt32 g_ime_caret_index = 0;

static size_t utf8_decode_one(const char *in, uint32_t *codepoint) {
    const unsigned char *s = (const unsigned char *)in;
    uint32_t cp = 0xFFFDU;
    size_t length = 1;
    if (!s || !s[0]) return 0;
    if (s[0] < 0x80U) {
        cp = s[0];
    } else if (s[0] >= 0xC2U && s[0] <= 0xDFU && (s[1] & 0xC0U) == 0x80U) {
        cp = ((uint32_t)(s[0] & 0x1FU) << 6) | (uint32_t)(s[1] & 0x3FU);
        length = 2;
    } else if (s[0] >= 0xE0U && s[0] <= 0xEFU && s[1] && s[2] &&
               (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U) {
        uint32_t decoded = ((uint32_t)(s[0] & 0x0FU) << 12) |
                           ((uint32_t)(s[1] & 0x3FU) << 6) |
                           (uint32_t)(s[2] & 0x3FU);
        if (decoded >= 0x800U && !(decoded >= 0xD800U && decoded <= 0xDFFFU)) {
            cp = decoded;
            length = 3;
        }
    } else if (s[0] >= 0xF0U && s[0] <= 0xF4U && s[1] && s[2] && s[3] &&
               (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U &&
               (s[3] & 0xC0U) == 0x80U) {
        uint32_t decoded = ((uint32_t)(s[0] & 0x07U) << 18) |
                           ((uint32_t)(s[1] & 0x3FU) << 12) |
                           ((uint32_t)(s[2] & 0x3FU) << 6) |
                           (uint32_t)(s[3] & 0x3FU);
        if (decoded >= 0x10000U && decoded <= 0x10FFFFU) {
            cp = decoded;
            length = 4;
        }
    }
    if (codepoint) *codepoint = cp;
    return length;
}

static void utf8_to_utf16(const char *in, SceWChar16 *out, size_t out_count) {
    size_t i = 0, o = 0;
    while (in && in[i] && o + 1 < out_count) {
        uint32_t cp;
        size_t bytes = utf8_decode_one(in + i, &cp);
        if (!bytes) break;
        i += bytes;
        if (cp <= 0xFFFFU) {
            out[o++] = (SceWChar16)cp;
        } else {
            if (o + 2 >= out_count) break;
            cp -= 0x10000U;
            out[o++] = (SceWChar16)(0xD800U + (cp >> 10));
            out[o++] = (SceWChar16)(0xDC00U + (cp & 0x3FFU));
        }
    }
    out[o] = 0;
}

static int utf16_to_utf8(const SceWChar16 *in, char *out, size_t out_max) {
    size_t o = 0;
    if (!out || out_max == 0) return -1;
    for (size_t i = 0; in[i] != 0; i++) {
        uint32_t c = in[i];
        if (c >= 0xD800U && c <= 0xDBFFU &&
            in[i + 1] >= 0xDC00U && in[i + 1] <= 0xDFFFU) {
            c = 0x10000U + ((c - 0xD800U) << 10) + (in[++i] - 0xDC00U);
        } else if (c >= 0xD800U && c <= 0xDFFFU) {
            c = 0xFFFDU;
        }
        size_t need = c < 0x80U ? 1 : (c < 0x800U ? 2 : (c < 0x10000U ? 3 : 4));
        if (o + need >= out_max) {
            out[o] = '\0';
            return -1;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000U) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
    return 0;
}

static void ime_event_handler(void *arg, const SceImeEventData *event) {
    (void)arg;
    if (!event) return;
    if (event->id == SCE_IME_EVENT_UPDATE_TEXT) {
        g_ime_caret_index = event->param.text.caretIndex;
    } else if (event->id == SCE_IME_EVENT_UPDATE_CARET) {
        g_ime_caret_index = event->param.caretIndex;
    } else if (event->id == SCE_IME_EVENT_PRESS_ENTER) {
        g_ime_entered = 1;
    } else if (event->id == SCE_IME_EVENT_PRESS_CLOSE) {
        g_ime_cancelled = 1;
    }
}

static size_t utf16_caret_to_utf8_byte(const SceWChar16 *text,
                                       SceUInt32 caret_index) {
    size_t bytes = 0;
    for (SceUInt32 i = 0; text && text[i] && i < caret_index; i++) {
        uint32_t c = text[i];
        if (c >= 0xD800U && c <= 0xDBFFU && i + 1 < caret_index &&
            text[i + 1] >= 0xDC00U && text[i + 1] <= 0xDFFFU) {
            bytes += 4;
            i++;
        } else {
            bytes += c < 0x80U ? 1 : (c < 0x800U ? 2 : 3);
        }
    }
    return bytes;
}

static void draw_editor(const char *live_text, size_t caret_byte,
                        int caret_visible, const char *title,
                        const char *hint) {
    vt_ui_rect(0, 0, VT_SCREEN_W, VT_SCREEN_H, VT_COLOR_BG);
    vt_ui_text(60, 60, VT_COLOR_MUTED, 1.0f, title);
    /* text field */
    vt_ui_rect(56, 100, VT_SCREEN_W - 112, 52, VT_COLOR_SURFACE);
    vt_ui_rect(56, 150, VT_SCREEN_W - 112, 2, VT_COLOR_ACCENT);
    vt_ui_text_clipped(70, 114, VT_COLOR_TEXT, 1.0f, VT_SCREEN_W - 140,
                       live_text);
    if (caret_visible) {
        char head[IME_LIVE_UTF8_CAPACITY];
        size_t n = caret_byte < sizeof head - 1 ? caret_byte : sizeof head - 1;
        memcpy(head, live_text, n);
        head[n] = '\0';
        int caret_x = 70 + vt_ui_text_width(1.0f, head);
        if (caret_x < VT_SCREEN_W - 70)
            vt_ui_rect(caret_x, 112, 2, 28, VT_COLOR_TEXT);
    }
    vt_ui_text(60, 168, VT_COLOR_FAINT, 0.85f, hint);
}

int ui_text_input(const char *title, const char *initial,
                  char *out, size_t out_max) {
    if (!out || out_max < 2) return -1;
    if (!title) title = "Text eingeben";

    utf8_to_utf16(initial ? initial : "", g_ime_initial,
                  sizeof(g_ime_initial) / sizeof(g_ime_initial[0]));
    memset(g_ime_input, 0, sizeof(g_ime_input));
    memcpy(g_ime_input, g_ime_initial, sizeof(g_ime_initial));
    out[0] = '\0';
    g_ime_entered = 0;
    g_ime_cancelled = 0;
    g_ime_caret_index = 0;
    while (g_ime_initial[g_ime_caret_index] != 0) g_ime_caret_index++;

    if (!g_ime_module_loaded) {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_IME) < 0) return -1;
        g_ime_module_loaded = 1;
    }

    SceImeParam param;
    sceImeParamInit(&param);
    param.supportedLanguages = 0; /* 0 = all installed languages */
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    param.work = g_ime_work;
    param.arg = NULL;
    param.handler = ime_event_handler;
    param.initialText = g_ime_initial;
    param.inputTextBuffer = g_ime_input;
    param.maxTextLength = out_max - 1 < TEXT_INPUT_MAX_CHARS
                        ? (SceUInt32)(out_max - 1) : TEXT_INPUT_MAX_CHARS;
    param.enterLabel = SCE_IME_ENTER_LABEL_DEFAULT;

    if (sceImeOpen(&param) < 0) return -1;
    int ime_open = 1;

    char live_text[IME_LIVE_UTF8_CAPACITY];
    static char previous_text[IME_LIVE_UTF8_CAPACITY];
    previous_text[0] = '\0';
    SceUInt32 previous_caret = g_ime_caret_index;
    int result = -1;
    int input_too_long = 0;
    /* idle timeout: any typing or caret movement resets the counter */
    for (int idle = 0; idle < IME_TIMEOUT_FRAMES; idle++) {
        utf16_to_utf8(g_ime_input, live_text, sizeof(live_text));
        size_t caret_byte = utf16_caret_to_utf8_byte(g_ime_input,
                                                     g_ime_caret_index);
        if (strcmp(live_text, previous_text) != 0 ||
            g_ime_caret_index != previous_caret) {
            snprintf(previous_text, sizeof previous_text, "%s", live_text);
            previous_caret = g_ime_caret_index;
            idle = 0;
        }
        int caret_visible =
            ((sceKernelGetProcessTimeWide() / 500000ULL) & 1) == 0;

        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_editor(live_text, caret_byte, caret_visible, title,
                    vt_str(input_too_long ? S_INPUT_TOO_LONG
                                          : S_INPUT_CONFIRM));
        vita2d_end_drawing();
        vita2d_wait_rendering_done();

        /* IME updates outside the GXM scene, immediately before the swap. */
        int update_ret = sceImeUpdate();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();

        if (g_ime_entered) {
            if (utf16_to_utf8(g_ime_input, out, out_max) < 0) {
                out[0] = '\0';
                g_ime_entered = 0;
                input_too_long = 1;
                continue;
            }
            sceImeClose();
            ime_open = 0;
            result = out[0] ? 1 : 0;
            break;
        }
        if (g_ime_cancelled) {
            sceImeClose();
            ime_open = 0;
            result = 0;
            break;
        }
        if (update_ret < 0) {
            sceImeClose();
            ime_open = 0;
            result = -1;
            break;
        }
    }
    if (ime_open) {
        /* Idle timeout. Handing the draft back as "confirmed" (1) made the
         * caller run a search for a half-typed string and store it in the
         * recent searches; 0 is the documented cancelled/empty result. */
        sceImeClose();
        out[0] = '\0';
        result = 0;
    }
    return result;
}
