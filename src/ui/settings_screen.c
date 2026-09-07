/* ViTube — the "Mehr" tab. See settings_screen.h. */
#include "ui/settings_screen.h"

#include <stdio.h>

#include <psp2/ctrl.h>

#include "app.h"
#include "net/http.h"
#include "ui/log_screen.h"
#include "store.h"
#include "ui/login_screen.h"
#include "ui/strings.h"
#include "ui/ui.h"
#include "yt/hd_helper.h"
#include "yt/oauth.h"
#include "yt/region.h"

/* Seven rows at 56 px fit the list area without scrolling today; the top
 * offset below keeps that from being a constraint on the eighth. */
#define ROW_H 56

typedef enum {
    ROW_ACCOUNT = 0,
    ROW_HD,
    ROW_REGION,
    ROW_LANGUAGE,
    ROW_SORT,
    ROW_LOG,
    ROW_CACHE,
    ROW_NETWORK,
    ROW_VERSION,
    ROW_COUNT
} SettingsRow;

/* Modals are a mode of this screen rather than their own loops with their own
 * frames: the shell keeps drawing the top bar, the tab row and the settings
 * rows, and this puts a menu on top of them. */
typedef enum {
    MODAL_NONE = 0,
    MODAL_QUALITY,
    MODAL_REGION,
    MODAL_LANGUAGE,
    MODAL_SORT,
    MODAL_CACHE
} SettingsModal;

#define QUALITY_HD 0
#define QUALITY_STANDARD 1
#define CACHE_CLEAR 0
#define CACHE_CANCEL 1

static int g_cursor;
static int g_top;
static SettingsModal g_modal;
static int g_modal_cursor;

/* Read fresh every frame rather than cached: the login state changes behind
 * this screen's back (a refresh that finds the token revoked logs out on its
 * own), and the whole tab is a handful of rows of text. */
static const char *row_label(int row) {
    switch (row) {
    case ROW_ACCOUNT:  return vt_str(S_ROW_ACCOUNT);
    case ROW_HD:       return vt_str(S_ROW_QUALITY);
    case ROW_REGION:   return vt_str(S_ROW_REGION);
    case ROW_LANGUAGE: return vt_str(S_ROW_LANGUAGE);
    case ROW_SORT:     return vt_str(S_ROW_SORT);
    case ROW_LOG:      return vt_str(S_ROW_LOG);
    case ROW_CACHE:    return vt_str(S_ROW_CACHE);
    case ROW_NETWORK:  return vt_str(S_ROW_NETWORK);
    default:           return vt_str(S_ROW_VERSION);
    }
}

static const char *row_value(int row) {
    switch (row) {
    case ROW_ACCOUNT:  return vt_str(yt_oauth_logged_in()
                                     ? S_SIGNED_IN_VALUE
                                     : S_NOT_SIGNED_IN_VALUE);
    case ROW_HD:       return yt_hd_pref_get() ? "720p HD"
                                               : vt_str(S_QUALITY_STANDARD);
    case ROW_REGION:   return yt_region_at(yt_region_index())->label;
    case ROW_LANGUAGE: return vt_lang_label(vt_lang_choice());
    case ROW_SORT:     return vt_str(
        vt_store_channel_sort() == VT_CHANNEL_SORT_ALPHA ? S_SORT_ALPHA
                                                         : S_SORT_RECENT);
    case ROW_NETWORK:  return vt_str(http_connected() ? S_CONNECTED
                                                      : S_DISCONNECTED);
    case ROW_VERSION:  return VITUBE_VERSION;
    default:           return "";
    }
}

/* Network and version are readouts, not settings — dimmed and skipped by
 * Cross, so pressing it there does not feel like a dead button. */
static int row_actionable(int row) {
    return row != ROW_NETWORK && row != ROW_VERSION;
}

int settings_screen_modal(void) {
    return g_modal != MODAL_NONE;
}

static int modal_entries(void) {
    if (g_modal == MODAL_REGION) return yt_region_count();
    if (g_modal == MODAL_LANGUAGE) return VT_LANG_CHOICE_COUNT;
    if (g_modal == MODAL_SORT) return 2;
    return 2;
}

/* Applies the choice at g_modal_cursor and closes. */
static SettingsResult modal_commit(void) {
    SettingsModal was = g_modal;
    int choice = g_modal_cursor;
    g_modal = MODAL_NONE;
    if (was == MODAL_QUALITY) {
        /* The only writer of the system-wide default. The player's Triangle
         * menu changes the video in front of you and nothing else. */
        yt_hd_pref_set(choice == QUALITY_HD);
        return SETTINGS_NOTHING;
    }
    if (was == MODAL_REGION) {
        if (choice == yt_region_index()) return SETTINGS_NOTHING;
        yt_region_set_index(choice);
        /* Every list on hand was fetched with the old hl/gl: titles, relative
         * dates and the results themselves are all in the wrong language. */
        return SETTINGS_RELOAD_ALL;
    }
    if (was == MODAL_LANGUAGE) {
        /* Only the app's own text changes — what YouTube sends is the
         * region's business — so nothing fetched needs dropping. */
        vt_lang_set_choice((VtLangChoice)choice);
        return SETTINGS_NOTHING;
    }
    if (was == MODAL_SORT) {
        vt_store_set_channel_sort(choice);
        return SETTINGS_NOTHING;
    }
    if (was == MODAL_CACHE && choice == CACHE_CLEAR) {
        vt_store_clear_personal();
        return SETTINGS_RELOAD_ALL;
    }
    return SETTINGS_NOTHING;
}

SettingsResult settings_screen_input(unsigned pressed, unsigned direction) {
    if (g_modal != MODAL_NONE) {
        int last = modal_entries() - 1;
        if ((direction & SCE_CTRL_UP) && g_modal_cursor > 0) g_modal_cursor--;
        if ((direction & SCE_CTRL_DOWN) && g_modal_cursor < last)
            g_modal_cursor++;
        if (pressed & SCE_CTRL_CIRCLE) g_modal = MODAL_NONE;
        else if (pressed & SCE_CTRL_CROSS) return modal_commit();
        return SETTINGS_NOTHING;
    }

    if ((direction & SCE_CTRL_UP) && g_cursor > 0) g_cursor--;
    if ((direction & SCE_CTRL_DOWN) && g_cursor < ROW_COUNT - 1) g_cursor++;
    if (!(pressed & SCE_CTRL_CROSS)) return SETTINGS_NOTHING;

    switch (g_cursor) {
    case ROW_ACCOUNT:
        /* login_screen_run() covers both directions: the device-code flow
         * when signed out, the account screen with its two-step logout when
         * signed in. */
        if (!login_screen_run()) return SETTINGS_NOTHING;
        return yt_oauth_logged_in() ? SETTINGS_SIGNED_IN : SETTINGS_SIGNED_OUT;
    case ROW_HD:
        /* A choice, not a switch that flips under the press: both options
         * named, the current one marked, readable before committing to it.
         *
         * Named after the resolution rather than a trade-off — the HLS detour
         * costs an extra resolve on paper, but on hardware 720p does not
         * start any slower, so calling the other option "fast" would be a
         * promise the app does not keep. Standard is for manifests that do
         * not resolve at all, and for a connection that cannot feed 720p. */
        g_modal = MODAL_QUALITY;
        g_modal_cursor = yt_hd_pref_get() ? QUALITY_HD : QUALITY_STANDARD;
        return SETTINGS_NOTHING;
    case ROW_REGION:
        g_modal = MODAL_REGION;
        g_modal_cursor = yt_region_index();
        return SETTINGS_NOTHING;
    case ROW_LANGUAGE:
        g_modal = MODAL_LANGUAGE;
        g_modal_cursor = (int)vt_lang_choice();
        return SETTINGS_NOTHING;
    case ROW_SORT:
        g_modal = MODAL_SORT;
        g_modal_cursor = vt_store_channel_sort();
        return SETTINGS_NOTHING;
    case ROW_LOG:
        log_screen_run();
        return SETTINGS_NOTHING;
    case ROW_CACHE:
        /* Confirmed rather than done on the press. Not because it is
         * dangerous — it only forces a refetch — but because the screen has
         * no way to show that anything happened, and a button that visibly
         * does nothing reads as broken. */
        g_modal = MODAL_CACHE;
        g_modal_cursor = CACHE_CANCEL;
        return SETTINGS_NOTHING;
    default:
        return SETTINGS_NOTHING;
    }
}

static void draw_modal(void) {
    if (g_modal == MODAL_QUALITY) {
        int hd = yt_hd_pref_get();
        VtMenuItem items[2] = {
            { "720p HD", NULL, hd },
            { vt_str(S_QUALITY_STANDARD), NULL, !hd },
        };
        vt_ui_menu_draw(vt_str(S_ROW_QUALITY), items, 2, g_modal_cursor);
    } else if (g_modal == MODAL_REGION) {
        VtMenuItem items[16];
        int n = yt_region_count();
        if (n > (int)(sizeof items / sizeof *items))
            n = (int)(sizeof items / sizeof *items);
        int current = yt_region_index();
        for (int i = 0; i < n; i++) {
            const yt_region_t *r = yt_region_at(i);
            items[i].label = r->label;
            items[i].value = r->gl;
            items[i].marked = (i == current);
        }
        vt_ui_menu_draw(vt_str(S_ROW_REGION), items, n, g_modal_cursor);
    } else if (g_modal == MODAL_LANGUAGE) {
        VtLangChoice current = vt_lang_choice();
        VtMenuItem items[VT_LANG_CHOICE_COUNT];
        for (int i = 0; i < VT_LANG_CHOICE_COUNT; i++) {
            items[i].label = vt_lang_label((VtLangChoice)i);
            /* Automatic shows what it currently resolves to, so the row is
             * not a promise the user has to go and verify. */
            items[i].value = (i == VT_LANG_AUTO)
                                 ? vt_lang_effective_label() : NULL;
            items[i].marked = (i == (int)current);
        }
        vt_ui_menu_draw(vt_str(S_ROW_LANGUAGE), items, VT_LANG_CHOICE_COUNT,
                        g_modal_cursor);
    } else if (g_modal == MODAL_SORT) {
        int current = vt_store_channel_sort();
        VtMenuItem items[2] = {
            { vt_str(S_SORT_RECENT), NULL, current == VT_CHANNEL_SORT_RECENT },
            { vt_str(S_SORT_ALPHA),  NULL, current == VT_CHANNEL_SORT_ALPHA },
        };
        vt_ui_menu_draw(vt_str(S_ROW_SORT), items, 2, g_modal_cursor);
    } else if (g_modal == MODAL_CACHE) {
        VtMenuItem items[2] = {
            { vt_str(S_CACHE_CLEAR), NULL, 0 },
            { vt_str(S_CACHE_KEEP),  NULL, 0 },
        };
        vt_ui_menu_draw(vt_str(S_CACHE_CONFIRM_TITLE), items, 2,
                        g_modal_cursor);
    }
}

void settings_screen_draw(int list_top_y) {
    int visible = (VT_SCREEN_H - list_top_y - 36) / ROW_H;
    if (visible < 1) visible = 1;
    if (g_cursor < g_top) g_top = g_cursor;
    if (g_cursor >= g_top + visible) g_top = g_cursor - visible + 1;

    for (int i = g_top; i < ROW_COUNT && i < g_top + visible; i++) {
        int y = list_top_y + (i - g_top) * ROW_H;
        int actionable = row_actionable(i);
        if (i == g_cursor) {
            vt_ui_rect(8, y, VT_SCREEN_W - 16, ROW_H - 4, VT_COLOR_FOCUS);
            vt_ui_rect(8, y, 4, ROW_H - 4, VT_COLOR_ACCENT);
        }
        vt_ui_text(28, y + 14, actionable ? VT_COLOR_TEXT : VT_COLOR_MUTED,
                   1.0f, row_label(i));
        const char *value = row_value(i);
        if (value[0]) {
            int w = vt_ui_text_width(1.0f, value);
            vt_ui_text(VT_SCREEN_W - 32 - w, y + 14,
                       actionable ? VT_COLOR_MUTED : VT_COLOR_FAINT, 1.0f,
                       value);
        }
    }
    draw_modal();
}

const char *settings_screen_hint(void) {
    if (g_modal != MODAL_NONE)
        return vt_str(S_HINT_SETTINGS_MODAL);
    switch (g_cursor) {
    case ROW_ACCOUNT:
        return vt_str(yt_oauth_logged_in() ? S_HINT_SETTINGS_ACCOUNT_IN
                                          : S_HINT_SETTINGS_ACCOUNT_OUT);
    case ROW_HD:
    case ROW_REGION:
    case ROW_LANGUAGE:
    case ROW_SORT:
        return vt_str(S_HINT_SETTINGS_CHANGE);
    case ROW_LOG:
        return vt_str(S_HINT_SETTINGS_OPEN);
    case ROW_CACHE:
        return vt_str(S_HINT_SETTINGS_CLEAR);
    default:
        return vt_str(S_HINT_SETTINGS_PLAIN);
    }
}
