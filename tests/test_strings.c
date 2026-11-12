/* Host-side checks on the catalogue (src/ui/strings_table.c + strings_*.c).
 *
 * German is the original; every other language is checked against it. The
 * one that matters is the printf parity: a translated format string whose
 * conversions differ from the original passes the compiler, passes review,
 * and then reads the wrong thing off the stack the first time that one code
 * path runs on the device. The glyph and key-name checks guard the hint
 * bars: a translator dropping a "✕" or a "START" leaves a legend that no
 * longer names the button it describes. Everything else here is bookkeeping.
 *
 *   gcc -Isrc -Wall -o /tmp/ts tests/test_strings.c src/ui/strings_table.c \
 *       src/ui/strings_de.c src/ui/strings_en.c src/ui/strings_es.c \
 *       src/ui/strings_pt.c src/ui/strings_it.c src/ui/strings_fr.c && /tmp/ts
 */
#include "../src/ui/strings.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* The conversions in order, e.g. "Kanäle (%d) von %s" -> "ds". "%%" is a
 * literal percent and consumes no argument. */
static void conversions(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = s; *p && n + 1 < cap; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%' || !*p) continue;
        while (*p && !strchr("diouxXeEfgGaAcspn", *p)) p++;
        if (*p) out[n++] = *p;
        else break;
    }
    out[n] = '\0';
}

static int count(const char *s, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = strstr(s, needle); p; p = strstr(p + len, needle)) n++;
    return n;
}

static int codepoints(const char *s) {
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) n++;
    return n;
}

static size_t leading_spaces(const char *s) { return strspn(s, " "); }
static size_t trailing_spaces(const char *s) {
    size_t n = strlen(s), t = 0;
    while (t < n && s[n - 1 - t] == ' ') t++;
    return t;
}

/* Everything that must survive translation verbatim: the face-button glyphs
 * the hint bar turns into icons, the hardware key names, the arrows, and the
 * three-space separator between hint segments. */
static const char *const invariant[] = {
    "\xe2\x9c\x95" /* ✕ */, "\xe2\x97\x8b" /* ○ */, "\xe2\x96\xb3" /* △ */,
    "\xe2\x96\xa1" /* □ */, "START", "SELECT", "R1", "L1", "Enter",
    "\xe2\x86\x91\xe2\x86\x93" /* ↑↓ */, "\xe2\x86\x90\xe2\x86\x92" /* ←→ */,
    "   ", "YouTube", "Google", "log.txt", "720p", "LIVE",
};

static void check_language(VtLangChoice lang) {
    const char *code = vt_lang_code(lang);
    int translated = 0;
    for (int i = 0; i < VT_STR_COUNT; i++) {
        const char *de = vt_str_in(VT_LANG_DE, (VtStringId)i);
        const char *s = vt_str_in(lang, (VtStringId)i);
        CHECK(s != NULL, "%s: id %d has no text", code, i);
        if (!s || !de) continue;
        CHECK(s[0], "%s: id %d is empty", code, i);

        char cde[16], cs[16];
        conversions(de, cde, sizeof cde);
        conversions(s, cs, sizeof cs);
        CHECK(strcmp(cde, cs) == 0,
              "%s: id %d: format conversions differ, de=\"%s\" (%s) %s=\"%s\" (%s)",
              code, i, de, cde, code, s, cs);

        for (size_t k = 0; k < sizeof invariant / sizeof *invariant; k++) {
            int want = count(de, invariant[k]), have = count(s, invariant[k]);
            CHECK(want == have,
                  "%s: id %d: \"%s\" appears %d time(s) in de, %d in %s: \"%s\"",
                  code, i, invariant[k], want, have, code, s);
        }
        CHECK(leading_spaces(de) == leading_spaces(s) &&
              trailing_spaces(de) == trailing_spaces(s),
              "%s: id %d: surrounding spaces differ: de=\"%s\" %s=\"%s\"",
              code, i, de, code, s);
        /* Generous caps, not a layout model: the screen is 960 px wide, the
         * longest hint bar (Italian) uses 596 of them and the widest tab bar
         * (French) 470. The real widths are measured with the shipped font
         * when a language is added; these only catch a runaway. */
        int cp = codepoints(s);
        int limit = i == S_TAB_HOME || i == S_TAB_SEARCH || i == S_TAB_SUBS ||
                    i == S_TAB_DOWNLOADS || i == S_TAB_MORE ? 14 : 90;
        CHECK(cp <= limit, "%s: id %d: %d code points, limit %d: \"%s\"",
              code, i, cp, limit, s);
        if (strcmp(de, s) != 0) translated++;
    }
    printf("%s: %d Strings, davon %d wirklich uebersetzt\n", code, VT_STR_COUNT,
           translated);
}

int main(void) {
    CHECK(vt_str_in(VT_LANG_AUTO, S_CANCEL_HINT) == NULL,
          "AUTO must not resolve to a table here");
    CHECK(vt_str_in(VT_LANG_CHOICE_COUNT, S_CANCEL_HINT) == NULL,
          "out-of-range language must be NULL");
    CHECK(vt_str_in(VT_LANG_DE, VT_STR_COUNT) == NULL,
          "out-of-range id must be NULL");
    CHECK(strcmp(vt_lang_code(VT_LANG_AUTO), "auto") == 0, "code of AUTO");
    CHECK(strcmp(vt_lang_code(VT_LANG_FR), "fr") == 0, "code of FR");
    for (int i = 0; i < VT_STR_COUNT; i++) {
        const char *de = vt_str_in(VT_LANG_DE, (VtStringId)i);
        CHECK(de && de[0], "de: id %d missing or empty", i);
    }
    for (int lang = VT_LANG_DE + 1; lang < VT_LANG_CHOICE_COUNT; lang++)
        check_language((VtLangChoice)lang);
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL STRING TESTS PASSED\n");
    return 0;
}
