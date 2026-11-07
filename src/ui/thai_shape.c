/* ViTube — Thai cluster substitution. See thai_shape.h. */
#include "ui/thai_shape.h"

#include "ui/thai_shape_table.h"

/* The mark lists, in the order tools/make-thai-font.py numbers them: the
 * index is part of the table key, so both lists must stay in step with the
 * script's VOWELS and TONES. Index 0 means "none". */
static const unsigned short g_vowels[] = {
    0, 0x0e31, 0x0e34, 0x0e35, 0x0e36, 0x0e37, 0x0e47, 0x0e4d,
    0x0e38, 0x0e39, 0x0e3a,
};
static const unsigned short g_tones[] = {
    0, 0x0e48, 0x0e49, 0x0e4a, 0x0e4b, 0x0e4c,
};
#define SARA_AM 0x0e33u

/* U+0E00-0E7F is always E0 B8 xx or E0 B9 xx in UTF-8, so Thai can be
 * recognised byte-wise. Returns the code point, or 0 for anything else. The
 * bytes are tested in order, so a string ending early is never read past its
 * terminator. */
static unsigned thai_at(const unsigned char *p) {
    if (p[0] != 0xe0 || (p[1] != 0xb8 && p[1] != 0xb9) ||
        (p[2] & 0xc0) != 0x80)
        return 0;
    return ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
}

static int index_in(const unsigned short *list, int n, unsigned cp) {
    for (int i = 1; i < n; i++)
        if (list[i] == cp) return i;
    return 0;
}

static int find(unsigned key) {
    int lo = 0, hi = (int)(sizeof g_thai_shapes / sizeof *g_thai_shapes) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_thai_shapes[mid].key == key) return mid;
        if (g_thai_shapes[mid].key < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Every code point written here is in the BMP above U+07FF: three bytes. */
static size_t put3(char *dst, size_t n, size_t cap, unsigned cp) {
    if (n + 3 >= cap) return n;
    dst[n] = (char)(0xe0 | (cp >> 12));
    dst[n + 1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    dst[n + 2] = (char)(0x80 | (cp & 0x3f));
    return n + 3;
}

int vt_thai_present(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (thai_at(p)) return 1;
    return 0;
}

int vt_thai_private_use(unsigned int cp) {
    return cp >= THAI_PUA_LO && cp <= THAI_PUA_HI;
}

size_t vt_thai_shape(char *dst, size_t cap, const char *src) {
    const unsigned char *p = (const unsigned char *)src;
    size_t n = 0;
    if (!cap) return 0;
    while (*p && n + 1 < cap) {
        unsigned c = thai_at(p);
        if (c < 0x0e01 || c > 0x0e2e) { /* not a consonant: copy as is */
            dst[n++] = (char)*p++;
            continue;
        }
        /* The one cluster shape the table knows: consonant, then at most one
         * above/below vowel, one tone mark, and a sara am (only without a
         * vowel). Anything after that is copied unchanged. */
        const unsigned char *q = p + 3;
        int v = index_in(g_vowels, sizeof g_vowels / sizeof *g_vowels,
                         thai_at(q));
        if (v) q += 3;
        int t = index_in(g_tones, sizeof g_tones / sizeof *g_tones,
                         thai_at(q));
        if (t) q += 3;
        int am = !v && thai_at(q) == SARA_AM;
        if (am) q += 3;
        int e = (v || t || am)
                    ? find(((c - 0x0e01) << 8) | (unsigned)(v << 4) |
                           (unsigned)(t << 1) | (unsigned)am)
                    : -1;
        if (e < 0) {
            while (p < q && n + 1 < cap) dst[n++] = (char)*p++;
            continue;
        }
        n = put3(dst, n, cap, g_thai_shapes[e].base);
        if (g_thai_shapes[e].tail) {
            n = put3(dst, n, cap, g_thai_shapes[e].tail);
        } else { /* only the consonant changed: the marks stay */
            for (const unsigned char *m = p + 3; m < q && n + 1 < cap; m++)
                dst[n++] = (char)*m;
        }
        p = q;
    }
    dst[n] = '\0';
    return n;
}
