#!/usr/bin/env python3
"""Builds assets/thai.ttf and src/ui/thai_shape_table.h from Noto Sans Thai.

vita2d draws one glyph per code point at the pen position and never consults
GSUB/GPOS, but Thai needs both: a tone mark sits directly on its consonant
unless an upper vowel is already there (then it moves up and shrinks: ที่),
marks on the tall consonants ป ฝ ฟ shift left, and ญ ฐ lose their tail when a
vowel hangs below them. Drawn glyph by glyph, Noto Sans Thai stacks every
tone mark straight onto the vowel underneath it.

So the shaping happens here, once, at build time. HarfBuzz shapes every
cluster the runtime recognises -- consonant, optional above/below vowel,
optional tone mark, optional sara am -- and wherever its result differs from
the plain glyph-by-glyph layout, the font gets the result as one composite
glyph at a private-use code point. The generated table tells
src/ui/thai_shape.c which code points to substitute; nothing is positioned at
runtime.

Source font (OFL-1.1, no reserved font name):
  https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSansThai/hinted/ttf/NotoSansThai-Regular.ttf
  sha256 61cf814eec46b294d6ea4401ac295d0cecd5207bd2331dcc5a15e7301d30ee44

Usage: python3 tools/make-thai-font.py NotoSansThai-Regular.ttf
Needs: pip install fonttools uharfbuzz
"""
import hashlib
import os
import sys

import uharfbuzz as hb
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent

SOURCE_SHA256 = "61cf814eec46b294d6ea4401ac295d0cecd5207bd2331dcc5a15e7301d30ee44"

# Must match src/ui/thai_shape.c: the index of each mark in these lists is
# part of the table key. Index 0 is "none".
CONSONANTS = range(0x0E01, 0x0E2F)
VOWELS = [0, 0x0E31, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E47, 0x0E4D,
          0x0E38, 0x0E39, 0x0E3A]
TONES = [0, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C]
SARA_AM = 0x0E33

PUA_BASE = 0xF000  # clear of emoji.ttf's remap at U+E000-EAFF
PUA_LIMIT = 0xF8FF
# Offsets closer than this to the plain layout are not worth a glyph: 20 units
# of a 1000-unit em is 0.4 px at the 18 px body size.
TOLERANCE = 20
# Composite offsets are rounded to this grid so near-identical clusters
# share a glyph.
GRID = 5


def key_of(c, v, t, am):
    return ((c - 0x0E01) << 8) | (v << 4) | (t << 1) | am


def main():
    src_path = sys.argv[1]
    with open(src_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != SOURCE_SHA256:
        sys.exit(f"unexpected source font sha256 {digest}")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    font = TTFont(src_path)
    glyf = font["glyf"]
    hmtx = font["hmtx"]
    cmap = font.getBestCmap()
    order = font.getGlyphOrder()

    hb_font = hb.Font(hb.Face(hb.Blob.from_file_path(src_path)))

    def shape(cps):
        buf = hb.Buffer()
        buf.add_codepoints(cps)
        buf.guess_segment_properties()
        hb.shape(hb_font, buf, {})
        out, pen = [], 0
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            out.append((order[info.codepoint], pen + pos.x_offset, pos.y_offset,
                        pos.x_advance))
            pen += pos.x_advance
        return out

    def flatten(name, x, y, acc):
        g = glyf[name]
        if g.isComposite():
            for comp in g.components:
                flatten(comp.glyphName, x + comp.x, y + comp.y, acc)
        elif g.numberOfContours:
            acc.append((name, x, y))
        return acc

    def same_picture(a, b):
        fa = sorted(p for n, x, y, _ in a for p in flatten(n, x, y, []))
        fb = sorted(p for n, x, y, _ in b for p in flatten(n, x, y, []))
        return len(fa) == len(fb) and all(
            na == nb and abs(xa - xb) <= TOLERANCE and abs(ya - yb) <= TOLERANCE
            for (na, xa, ya), (nb, xb, yb) in zip(fa, fb))

    def plain(cps):
        out, pen = [], 0
        for cp in cps:
            name = cmap[cp]
            adv = hmtx[name][0]
            out.append((name, pen, 0, adv))
            pen += adv
        return out

    next_pua = [PUA_BASE]
    alias_of = {}      # existing glyph name -> PUA code point
    composite_of = {}  # component signature -> PUA code point
    new_cmap = {}

    def alias(name):
        if name not in alias_of:
            alias_of[name] = next_pua[0]
            new_cmap[next_pua[0]] = name
            next_pua[0] += 1
        return alias_of[name]

    def composite(parts, advance):
        sig = (advance,) + tuple(
            (n, GRID * round(x / GRID), GRID * round(y / GRID)) for n, x, y in parts)
        if sig not in composite_of:
            name = f"thai.cluster.{len(composite_of)}"
            g = Glyph()
            g.numberOfContours = -1
            g.components = []
            for n, x, y in sig[1:]:
                comp = GlyphComponent()
                comp.glyphName, comp.x, comp.y, comp.flags = n, x, y, 0
                g.components.append(comp)
            glyf[name] = g
            g.recalcBounds(glyf)
            hmtx[name] = (advance, g.xMin)
            composite_of[sig] = next_pua[0]
            new_cmap[next_pua[0]] = name
            next_pua[0] += 1
        return composite_of[sig]

    entries = []
    for c in CONSONANTS:
        for v in range(len(VOWELS)):
            for t in range(len(TONES)):
                for am in (0, 1):
                    if not (v or t or am) or (am and v):
                        continue
                    cps = [c] + [x for x in (VOWELS[v], TONES[t]) if x]
                    if am:
                        cps.append(SARA_AM)
                    got = shape(cps)
                    if same_picture(got, plain(cps)):
                        continue
                    base_name, bx, by, base_adv = got[0]
                    assert bx == 0 and by == 0, (cps, got)
                    base_out = c if base_name == cmap[c] else alias(base_name)
                    tail = got[1:]
                    if same_picture(tail, [(n, x + base_adv, y, a)
                                           for n, x, y, a in plain(cps[1:])]):
                        tail_out = 0  # only the consonant changed
                    else:
                        tail_out = composite(
                            [(n, x - base_adv, y) for n, x, y, _ in tail],
                            sum(a for *_, a in tail))
                    entries.append((key_of(c, v, t, am), base_out, tail_out))
    if next_pua[0] > PUA_LIMIT:
        sys.exit("private use area exhausted")

    font.setGlyphOrder(glyf.glyphOrder)
    for table in font["cmap"].tables:
        if table.isUnicode():
            table.cmap.update(new_cmap)
    # vita2d never reads the layout tables, and the new glyphs are not in them.
    for tag in ("GSUB", "GPOS", "GDEF"):
        del font[tag]
    font.save(os.path.join(root, "assets", "thai.ttf"))

    with open(os.path.join(root, "src", "ui", "thai_shape_table.h"), "w",
              newline="\n") as f:
        f.write(
            "/* Generated by tools/make-thai-font.py -- do not edit. Every code point\n"
            " * from THAI_PUA_LO up exists only in the assets/thai.ttf that the same run\n"
            " * wrote: regenerate both together. See src/ui/thai_shape.c. */\n"
            f"#define THAI_PUA_LO 0x{PUA_BASE:04x}u\n"
            f"#define THAI_PUA_HI 0x{next_pua[0] - 1:04x}u\n\n"
            "static const struct { unsigned short key, base, tail; }\n"
            "g_thai_shapes[] = {\n")
        for k, b, t in sorted(entries):
            f.write(f"    {{ 0x{k:04x}, 0x{b:04x}, 0x{t:04x} }},\n")
        f.write("};\n")
    print(f"{len(entries)} clusters, {len(alias_of)} aliases, "
          f"{len(composite_of)} composites, PUA up to U+{next_pua[0] - 1:04X}")


if __name__ == "__main__":
    main()
