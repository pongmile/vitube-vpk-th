#!/usr/bin/env python3
"""Draws the VPK's system artwork: the bubble icon, the LiveArea and pic0.

The LiveArea is the page the Vita shows when the bubble is tapped, before the
app itself starts. Without sce_sys/livearea it is the bare system default;
with it, it carries the same dark palette and red play mark as the app's own
top bar (src/ui/ui.c vt_ui_top_bar, colours from src/app.h). Everything is
drawn here from code -- no downloaded artwork, no third-party logo -- so the
images can be regenerated and restyled by editing this file.

Writes, relative to the repository root:
  sce_sys/icon0.png                     128x128  home-screen bubble
  sce_sys/pic0.png                      960x544  full-screen backdrop
  sce_sys/livearea/contents/bg.png      840x500  LiveArea background
  sce_sys/livearea/contents/startup.png 280x158  the gate ("Start") image
(template.xml next to them is hand-written and not touched here.)

The system only accepts 8-bit indexed PNGs for all of these: a truecolour
file makes the VPK install fail or the image silently not show. Every image
is therefore drawn in RGB at SCALE times its size (Pillow's primitives do not
antialias), downsampled, and quantised to a 256-colour palette with
Floyd-Steinberg dithering (plus a little fixed-seed grain) so the gradients
do not band. The last step re-reads each file's PNG header and checks it is
8-bit indexed, non-interlaced and the right size.

Text uses the fonts the app already ships, so the output is the same on any
machine: assets/font.ttf (DejaVu Sans, Book weight -- the wordmark is
emboldened with a stroke) and assets/thai.ttf (Noto Sans Thai). Only Thai
words without stacked marks are used: Pillow's basic layout places each
glyph at the pen like vita2d does, and a single mark per consonant is
already positioned correctly by the font.

Usage: python3 tools/make-livearea.py
Needs: pip install Pillow
"""
import os
import random
import struct

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "assets", "font.ttf")
THAI_FONT = os.path.join(ROOT, "assets", "thai.ttf")

# Supersampling factor: everything is drawn this many times larger, then
# downsampled with LANCZOS, which gives antialiased edges.
SCALE = 4

# src/app.h -- keep in step with the app's palette.
BG = (12, 12, 14)          # VT_COLOR_BG
SURFACE = (24, 24, 28)     # VT_COLOR_SURFACE
RAISED = (38, 38, 44)      # VT_COLOR_RAISED
BORDER = (52, 52, 60)      # VT_COLOR_BORDER
ACCENT = (255, 0, 0)       # VT_COLOR_ACCENT
TEXT = (240, 240, 240)     # VT_COLOR_TEXT
MUTED = (160, 160, 168)    # VT_COLOR_MUTED
FAINT = (105, 105, 112)    # VT_COLOR_FAINT
WHITE = (255, 255, 255)    # the play triangle and the tag's letters


def font(size, path=FONT):
    # BASIC layout pins the result even where Pillow was built with libraqm.
    return ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(w, h, top, bottom):
    """An RGB image fading from `top` to `bottom`."""
    column = Image.new("RGB", (1, h))
    column.putdata([lerp(top, bottom, y / max(1, h - 1)) for y in range(h)])
    return column.resize((w, h), Image.NEAREST)


def glow(canvas, cx, cy, rx, ry, color, strength):
    """Adds a soft elliptical light of `color` centred on (cx, cy)."""
    w, h = canvas.size
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).ellipse((cx - rx, cy - ry, cx + rx, cy + ry),
                                 fill=round(255 * strength))
    # Blur radius relative to the ellipse, so the falloff is the same at
    # every scale.
    mask = mask.filter(ImageFilter.GaussianBlur(min(rx, ry) * 0.6))
    canvas.paste(Image.new("RGB", (w, h), color), (0, 0), mask)


def draw_mark(canvas, cx, cy, size):
    """The play mark: a red rounded square with a white play triangle.

    The red block with a white triangle is the logo of the app's top bar
    (vt_ui_top_bar), drawn square here with soft corners -- an original mark,
    deliberately not the wide 16:9 rounded rectangle of any video site's
    logo. The triangle's corners are rounded too, and it is nudged right of
    centre so it looks centred: a triangle's visual centre is its centroid,
    not the middle of its bounding box. Returns the block's bounding box."""
    x0, y0 = round(cx - size / 2), round(cy - size / 2)
    draw = ImageDraw.Draw(canvas)
    draw.rounded_rectangle((x0, y0, x0 + size - 1, y0 + size - 1),
                           radius=round(size * 0.28), fill=ACCENT)

    # A rounded triangle is a smaller triangle dilated by r: the core filled,
    # its edges stroked 2r wide, and a disc of radius r on every corner.
    r = size * 0.06
    th = size * 0.46 - 2 * r       # core height (vertical extent)
    tw = th * 0.92                 # core depth (horizontal extent)
    tx = cx - tw / 2 + tw * 0.12
    core = [(tx, cy - th / 2), (tx + tw, cy), (tx, cy + th / 2)]
    draw.polygon(core, fill=WHITE)
    for i, (px, py) in enumerate(core):
        qx, qy = core[(i + 1) % 3]
        draw.line((px, py, qx, qy), fill=WHITE, width=round(2 * r))
        draw.ellipse((px - r, py - r, px + r, py + r), fill=WHITE)
    return x0, y0, x0 + size, y0 + size


def text_box(draw, text, fnt, stroke=0):
    return draw.textbbox((0, 0), text, font=fnt, stroke_width=stroke)


def draw_text(draw, x, y, text, fnt, fill, stroke=0):
    """Draws `text` with its ink box's top-left corner at (x, y)."""
    l, t, _, _ = text_box(draw, text, fnt, stroke)
    draw.text((x - l, y - t), text, font=fnt, fill=fill,
              stroke_width=stroke, stroke_fill=fill)


def draw_tag(canvas, x, y0, height, label):
    """The "TH" tag: a small accent pill with white letters, its top-left
    corner at (x, y0). Returns its right edge."""
    draw = ImageDraw.Draw(canvas)
    fnt = font(round(height * 0.66))
    stroke = max(1, round(height * 0.03))
    l, t, r, b = text_box(draw, label, fnt, stroke)
    pad = height * 0.30
    w = (r - l) + 2 * pad
    draw.rounded_rectangle((x, y0, x + w, y0 + height), radius=height * 0.22,
                           fill=ACCENT)
    draw_text(draw, x + pad, y0 + (height - (b - t)) / 2, label, fnt,
              WHITE, stroke)
    return x + w


def tag_width(height, label):
    """Width draw_tag() will give a tag of this height."""
    draw = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    l, _, r, _ = text_box(draw, label, font(round(height * 0.66)),
                          max(1, round(height * 0.03)))
    return (r - l) + 2 * height * 0.30


def draw_logo(canvas, cx, cy, height, tag=True):
    """Mark + "ViTube" wordmark (+ "TH" tag), centred as a group on (cx, cy).

    `height` is the mark's size; the wordmark's cap height is about 70% of
    it, close to the proportions of the app's top bar."""
    draw = ImageDraw.Draw(canvas)
    word_font = font(round(height * 0.98))
    stroke = max(1, round(height * 0.035))  # DejaVu Book -> semibold
    l, t, r, b = text_box(draw, "ViTube", word_font, stroke)
    word_w, word_h = r - l, b - t

    gap = height * 0.30
    tag_h = height * 0.48
    tag_gap = height * 0.22
    total = height + gap + word_w
    if tag:
        total += tag_gap + tag_width(tag_h, "TH")

    x = cx - total / 2
    draw_mark(canvas, x + height / 2, cy, height)
    x += height + gap
    # "ViTube" has no descenders, so its ink box is centred on the mark.
    top = cy - word_h / 2
    draw_text(draw, x, top, "ViTube", word_font, TEXT, stroke)
    if tag:
        # The tag's bottom sits on the wordmark's baseline.
        draw_tag(canvas, x + word_w + tag_gap, top + word_h - tag_h, tag_h, "TH")


# Colours drawn flat somewhere (text, the mark, the tag). They are given
# palette entries of their own: left to median cut, the gradients take most
# of the palette, the white text lands between two greys and the dither turns
# it to visible grain.
EXACT = [ACCENT, WHITE, TEXT, MUTED, FAINT, BG, SURFACE, RAISED, BORDER]


def finish(canvas, size, name):
    """Downsamples the supersampled canvas, quantises and saves it.

    Pillow's quantize() never dithers when it builds the palette itself, and
    the dark red glows then come out as visible rings. So the palette is
    built first -- median cut for the gradients and antialiased edges, plus
    the EXACT colours -- and the image is then mapped onto it with
    Floyd-Steinberg dithering.

    The dark gradients span only a dozen 8-bit levels, so even before
    quantising they show as steps on the Vita's OLED. A little grain (at most
    two levels either way, from a fixed seed so every run is identical) is
    added to the dark areas only -- the flat text and mark colours stay exact
    -- and breaks the steps up."""
    img = canvas.resize(size, Image.LANCZOS)
    rng = random.Random(size[0] * 10000 + size[1])
    count = size[0] * size[1]
    # Two uniform 0..2 values summed: triangular noise in 0..4, centred on 2.
    grain = ImageChops.add(
        *(Image.frombytes("L", size, rng.randbytes(count))
          .point(lambda v: v * 3 // 256) for _ in range(2)))
    noisy = ImageChops.add(img, Image.merge("RGB", [grain] * 3), offset=-2)
    dark = img.convert("L").point(lambda v: 255 if v < 72 else 0)
    img = Image.composite(noisy, img, dark)

    n = 256 - len(EXACT)
    cut = img.quantize(colors=n, method=Image.Quantize.MEDIANCUT)
    entries = cut.getpalette()[:3 * n]
    entries += [v for color in EXACT for v in color]
    palette = Image.new("P", (1, 1))
    palette.putpalette(entries + [0] * (768 - len(entries)))
    img = img.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    path = os.path.join(ROOT, *name.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, optimize=True)
    return path


def make_icon0():
    """The home-screen bubble. The system crops it to a circle, so the mark
    stays well inside the inscribed circle and the corners are just
    background."""
    s = 128 * SCALE
    c = vertical_gradient(s, s, RAISED, BG)
    glow(c, s / 2, s / 2, s * 0.42, s * 0.30, (90, 0, 0), 0.55)
    draw_mark(c, s / 2, s / 2, round(s * 0.60))
    return finish(c, (128, 128), "sce_sys/icon0.png")


def feed_ghosts(canvas, x0, x1, fade_to):
    """Faint video cards -- a thumbnail and two title lines, like the app's
    feed -- in a column between x0 and x1, fading out towards `fade_to` (the
    centre of the page), so the sides of the LiveArea are not bare."""
    w, h = canvas.size
    shape = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(shape)
    cw = x1 - x0
    th = cw * 9 / 16
    pitch = th + cw * 0.36
    y = h * 0.5 - pitch * 1.5 - th * 0.15   # three cards, roughly centred
    for _ in range(3):
        d.rounded_rectangle((x0, y, x1, y + th), radius=cw * 0.06, fill=255)
        bar = cw * 0.05
        d.rounded_rectangle((x0, y + th + bar * 1.4, x0 + cw * 0.86,
                             y + th + bar * 2.4), radius=bar / 2, fill=255)
        d.rounded_rectangle((x0, y + th + bar * 3.2, x0 + cw * 0.56,
                             y + th + bar * 4.2), radius=bar / 2, fill=255)
        y += pitch
    # Opacity falls off linearly towards the centre, and towards the top and
    # bottom so the cards do not crowd the logo and the tagline.
    ramp = Image.new("L", (w, 1))
    far = max(abs(x0 - fade_to), abs(x1 - fade_to))
    ramp.putdata([round(150 * min(1.0, abs(x - fade_to) / far))
                  for x in range(w)])
    ramp = ramp.resize((w, h), Image.NEAREST)
    vert = Image.new("L", (1, h))
    vert.putdata([round(255 * max(0.0, 1 - abs(y - h / 2) / (h * 0.5)) ** 0.7)
                  for y in range(h)])
    vert = vert.resize((w, h), Image.NEAREST)
    mask = ImageChops.multiply(ImageChops.multiply(shape, ramp), vert)
    canvas.paste(Image.new("RGB", (w, h), RAISED), (0, 0), mask)


def make_bg():
    """LiveArea background. The system draws the 280x158 gate over the
    middle of it, so the logo goes in the band above and the tagline in the
    band below; the centre only carries a soft red glow that frames the
    gate."""
    w, h = 840 * SCALE, 500 * SCALE
    c = vertical_gradient(w, h, SURFACE, BG)
    feed_ghosts(c, 36 * SCALE, 176 * SCALE, w / 2)
    feed_ghosts(c, w - 176 * SCALE, w - 36 * SCALE, w / 2)
    glow(c, w / 2, h * 0.52, w * 0.30, h * 0.26, (110, 0, 0), 0.50)
    draw_logo(c, w / 2, 72 * SCALE, 44 * SCALE)

    draw = ImageDraw.Draw(c)
    # Thin divider under the logo band, faded at both ends.
    line = Image.new("L", (w, 1))
    line.putdata([round(255 * max(0.0, 1 - abs(x - w / 2) / (w * 0.32)))
                  for x in range(w)])
    line = line.resize((w, SCALE), Image.NEAREST)  # 1 px once downsampled
    c.paste(Image.new("RGB", line.size, BORDER), (0, 120 * SCALE), line)

    en = font(20 * SCALE)
    th = font(21 * SCALE, THAI_FONT)
    tagline = "Video client for PS Vita"
    thai = "รองรับภาษาไทย"
    l, t, r, b = text_box(draw, tagline, en)
    draw_text(draw, (w - (r - l)) / 2, 404 * SCALE, tagline, en, MUTED)
    l, t, r, b = text_box(draw, thai, th)
    draw_text(draw, (w - (r - l)) / 2, 440 * SCALE, thai, th, FAINT)
    return finish(c, (840, 500), "sce_sys/livearea/contents/bg.png")


def make_startup():
    """The gate image: the logo on a raised card."""
    w, h = 280 * SCALE, 158 * SCALE
    c = vertical_gradient(w, h, RAISED, SURFACE)
    glow(c, w / 2, h * 0.45, w * 0.45, h * 0.40, (80, 0, 0), 0.45)
    draw_logo(c, w / 2, h * 0.46, 30 * SCALE)
    return finish(c, (280, 158), "sce_sys/livearea/contents/startup.png")


def make_pic0():
    """Full-screen backdrop: big centred logo over the app's background."""
    w, h = 960 * SCALE, 544 * SCALE
    c = vertical_gradient(w, h, SURFACE, BG)
    glow(c, w / 2, h * 0.48, w * 0.34, h * 0.30, (100, 0, 0), 0.50)
    draw_logo(c, w / 2, h * 0.46, 64 * SCALE)
    draw = ImageDraw.Draw(c)
    en = font(22 * SCALE)
    tagline = "Video client for PS Vita"
    l, t, r, b = text_box(draw, tagline, en)
    draw_text(draw, (w - (r - l)) / 2, h * 0.46 + 70 * SCALE, tagline, en, MUTED)
    return finish(c, (960, 544), "sce_sys/pic0.png")


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def check(path, size):
    """Reads the PNG header itself rather than trusting Pillow's save: the
    file must be colour type 3 (indexed) at bit depth 8 -- Pillow's optimize
    would write 1/2/4-bit files for images with few colours -- not interlaced,
    and the expected size."""
    with open(path, "rb") as f:
        head = f.read(33)
    w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", head[16:29])
    ok = (head[:8] == PNG_SIGNATURE and head[12:16] == b"IHDR"
          and (w, h) == size and depth == 8 and ctype == 3 and interlace == 0)
    with Image.open(path) as im:
        ok = ok and im.mode == "P"
    print(f"{'ok ' if ok else 'BAD'} {os.path.relpath(path, ROOT)}: {w}x{h}, "
          f"{depth}-bit, colour type {ctype}, {os.path.getsize(path)} bytes")
    if not ok:
        raise SystemExit(f"{path}: expected an 8-bit indexed {size[0]}x{size[1]} PNG")


def main():
    check(make_icon0(), (128, 128))
    check(make_pic0(), (960, 544))
    check(make_bg(), (840, 500))
    check(make_startup(), (280, 158))


if __name__ == "__main__":
    main()
