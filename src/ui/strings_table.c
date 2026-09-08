/* ViTube — the catalogue's index. One table per language, each in its own
 * file (strings_de.c ... strings_fr.c) and each indexed by VtStringId through
 * designated initialisers; this file only maps a language to its table.
 *
 * Deliberately free of psp2 and of the language setting, so that
 * tests/test_strings.c can compile it on the build machine together with the
 * tables and check that every language has every entry and that the printf
 * conversions match the German original. A translated format string with a
 * different conversion than its original is the classic way localisation
 * crashes, and it is invisible until the one code path that uses it runs on
 * the device. */
#include "ui/strings.h"

#include <stddef.h>

extern const char *const vt_strings_de[VT_STR_COUNT];
extern const char *const vt_strings_en[VT_STR_COUNT];
extern const char *const vt_strings_es[VT_STR_COUNT];
extern const char *const vt_strings_pt[VT_STR_COUNT];
extern const char *const vt_strings_it[VT_STR_COUNT];
extern const char *const vt_strings_fr[VT_STR_COUNT];

static const char *const *const g_tables[VT_LANG_CHOICE_COUNT] = {
    [VT_LANG_DE] = vt_strings_de,
    [VT_LANG_EN] = vt_strings_en,
    [VT_LANG_ES] = vt_strings_es,
    [VT_LANG_PT] = vt_strings_pt,
    [VT_LANG_IT] = vt_strings_it,
    [VT_LANG_FR] = vt_strings_fr,
};

static const char *const g_codes[VT_LANG_CHOICE_COUNT] = {
    [VT_LANG_AUTO] = "auto",
    [VT_LANG_DE] = "de", [VT_LANG_EN] = "en", [VT_LANG_ES] = "es",
    [VT_LANG_PT] = "pt", [VT_LANG_IT] = "it", [VT_LANG_FR] = "fr",
};

const char *vt_str_in(VtLangChoice lang, VtStringId id) {
    if (lang <= VT_LANG_AUTO || lang >= VT_LANG_CHOICE_COUNT) return NULL;
    if (id < 0 || id >= VT_STR_COUNT) return NULL;
    return g_tables[lang][id];
}

const char *vt_lang_code(VtLangChoice lang) {
    if (lang < 0 || lang >= VT_LANG_CHOICE_COUNT) return "?";
    return g_codes[lang];
}
