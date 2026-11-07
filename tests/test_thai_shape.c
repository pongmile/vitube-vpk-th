/* Host-side checks on the Thai cluster substitution (src/ui/thai_shape.c).
 *
 * The table itself is generated, so what is worth checking is the code that
 * reads it: that a cluster finds its entry, that text around and between
 * clusters comes through byte for byte, and that the output never grows —
 * ui.c shapes into a buffer sized from the input.
 *
 *   gcc -Isrc -Wall -o /tmp/tt tests/test_thai_shape.c src/ui/thai_shape.c \
 *       && /tmp/tt
 */
#include "../src/ui/thai_shape.h"
#include "../src/ui/thai_shape_table.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

static const unsigned short g_vowels[] = {
    0, 0x0e31, 0x0e34, 0x0e35, 0x0e36, 0x0e37, 0x0e47, 0x0e4d,
    0x0e38, 0x0e39, 0x0e3a,
};
static const unsigned short g_tones[] = {
    0, 0x0e48, 0x0e49, 0x0e4a, 0x0e4b, 0x0e4c,
};

static size_t put3(char *dst, size_t n, unsigned cp) {
    dst[n] = (char)(0xe0 | (cp >> 12));
    dst[n + 1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    dst[n + 2] = (char)(0x80 | (cp & 0x3f));
    return n + 3;
}

static void unchanged(const char *s) {
    char out[256];
    size_t n = vt_thai_shape(out, sizeof out, s);
    CHECK(n == strlen(s) && strcmp(out, s) == 0, "changed: %s -> %s", s, out);
}

int main(void) {
    /* Text without Thai, Thai that needs nothing, and malformed input. */
    unchanged("");
    unchanged("Hello, world! \xc3\xa4\xf0\x9f\x98\x80");
    unchanged("\xe0\xb8\x81\xe0\xb8\xb2");             /* กา */
    unchanged("\xe0\xb8\x81\xe0\xb9\x88");             /* ก่ */
    unchanged("\xe0\xb8\x81\xe0\xb8\xb4");             /* กิ */
    unchanged("\xe0\xb9\x80\xe0\xb8\x81");             /* เก */
    unchanged("\xe0\xb8");                             /* truncated */
    unchanged("\xe0\xb8\x81\xe0\xb8");                 /* ก, then truncated */
    unchanged("\xe0\xb9\x88\xe0\xb9\x88");             /* marks, no consonant */
    CHECK(!vt_thai_present("plain ascii \xc3\xa4"), "no Thai");
    CHECK(vt_thai_present("x \xe0\xb8\x81"), "Thai present");

    /* Every table entry, rebuilt as text and embedded between ASCII. */
    size_t count = sizeof g_thai_shapes / sizeof *g_thai_shapes;
    for (size_t i = 0; i < count; i++) {
        unsigned key = g_thai_shapes[i].key;
        unsigned c = 0x0e01 + (key >> 8), v = (key >> 4) & 15,
                 t = (key >> 1) & 7, am = key & 1;
        CHECK(i == 0 || g_thai_shapes[i - 1].key < key, "table not sorted");
        char in[32], want[32], out[32];
        size_t n = 0, w = 0;
        in[n++] = want[w++] = 'a';
        n = put3(in, n, c);
        if (v) n = put3(in, n, g_vowels[v]);
        if (t) n = put3(in, n, g_tones[t]);
        if (am) n = put3(in, n, 0x0e33);
        in[n++] = 'z';
        in[n] = '\0';
        w = put3(want, w, g_thai_shapes[i].base);
        if (g_thai_shapes[i].tail) {
            w = put3(want, w, g_thai_shapes[i].tail);
        } else {
            memcpy(want + w, in + 4, n - 5);
            w += n - 5;
        }
        want[w++] = 'z';
        want[w] = '\0';
        size_t got = vt_thai_shape(out, n + 1, in);
        CHECK(got == w && strcmp(out, want) == 0, "entry %04x", key);
        CHECK(got <= n, "entry %04x grew", key);
        CHECK(g_thai_shapes[i].tail == 0 ||
              (g_thai_shapes[i].tail >= THAI_PUA_LO &&
               g_thai_shapes[i].tail <= THAI_PUA_HI), "entry %04x tail", key);
    }

    /* ที่ (tone over an upper vowel) must be substituted, and a buffer too
     * small for the input must still come back terminated. */
    char out[16];
    vt_thai_shape(out, sizeof out, "\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88");
    CHECK(strcmp(out, "\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88") != 0, "ที่");
    size_t small = vt_thai_shape(out, 4, "\xe0\xb8\x81\xe0\xb8\x81");
    CHECK(small < 4 && out[small] == '\0', "truncation");

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL THAI SHAPE TESTS PASSED (%zu clusters)\n", count);
    return 0;
}
