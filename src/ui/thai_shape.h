/* ViTube — Thai mark placement for a renderer without a shaping engine.
 *
 * vita2d draws one glyph per code point and never applies a font's GSUB/GPOS
 * tables, and Thai depends on them: a tone mark sits higher when an upper
 * vowel is already on the consonant (ที่), marks move left on ป ฝ ฟ, and ญ ฐ
 * drop their tail above a lower vowel. Drawn glyph by glyph, those marks
 * collide.
 *
 * tools/make-thai-font.py shapes every consonant + vowel + tone (+ sara am)
 * cluster with HarfBuzz ahead of time, and stores each result that differs
 * from the plain sequence as a single positioned glyph in the private use
 * area of assets/thai.ttf. These functions rewrite text to those code points;
 * the glyphs themselves need no further positioning. */
#ifndef VITUBE_UI_THAI_SHAPE_H
#define VITUBE_UI_THAI_SHAPE_H

#include <stddef.h>

/* Nonzero if `s` contains any Thai letter, i.e. vt_thai_shape() could change
 * it. Cheap: a byte scan, so callers can skip the copy for everything else. */
int vt_thai_present(const char *s);

/* Nonzero for the private-use code points vt_thai_shape() writes; only
 * assets/thai.ttf has glyphs for them. */
int vt_thai_private_use(unsigned int cp);

/* Writes the shaped form of `src` into `dst` and returns its length. The
 * result is never longer than `src` (every substitution replaces one or more
 * three-byte Thai code points with as many or fewer three-byte PUA ones), so
 * a buffer of strlen(src) + 1 always suffices. A smaller `cap` truncates
 * the output; it is NUL-terminated either way. */
size_t vt_thai_shape(char *dst, size_t cap, const char *src);

#endif
