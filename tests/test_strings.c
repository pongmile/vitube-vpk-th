/* Host-side checks on the German/English catalogue (src/ui/strings_table.c).
 *
 * The one that matters is the printf parity: a translated format string whose
 * conversions differ from the original passes the compiler, passes review,
 * and then reads the wrong thing off the stack the first time that one code
 * path runs on the device. Everything else here is bookkeeping.
 *
 *   gcc -Isrc -Wall -o /tmp/ts tests/test_strings.c \
 *       src/ui/strings_table.c && /tmp/ts
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

int main(void) {
    int translated = 0;
    for (int i = 0; i < VT_STR_COUNT; i++) {
        const char *de = vt_str_de((VtStringId)i);
        const char *en = vt_str_en((VtStringId)i);
        CHECK(de != NULL, "id %d has no German text", i);
        CHECK(en != NULL, "id %d has no English text", i);
        if (!de || !en) continue;
        CHECK(de[0], "id %d: German text is empty", i);
        CHECK(en[0], "id %d: English text is empty", i);

        char cde[16], cen[16];
        conversions(de, cde, sizeof cde);
        conversions(en, cen, sizeof cen);
        CHECK(strcmp(cde, cen) == 0,
              "id %d: format conversions differ, de=\"%s\" (%s) en=\"%s\" (%s)",
              i, de, cde, en, cen);
        if (strcmp(de, en) != 0) translated++;
    }
    printf("%d Strings, davon %d wirklich uebersetzt\n", VT_STR_COUNT,
           translated);
    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL STRING TESTS PASSED\n");
    return 0;
}
