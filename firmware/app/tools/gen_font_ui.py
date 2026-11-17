#!/usr/bin/env python3
"""Generate an LCD interface font table (ak_font_ui12 / ak_font_ui16).

Each UI font size is plain data behind one symbol, so trying another font is
only a regeneration of its file; no drawing code changes. Needs Pillow + numpy
(python3.12 on the build host). The 16 px table (the defaults):

  python3.12 tools/gen_font_ui.py --font unifont-18.0.01.otf

The 12 px table (Fusion Pixel, which also carries GB2312 level 1):

  python3.12 tools/gen_font_ui.py --font fusion-pixel-12px-monospaced-zh_hans.ttf \
      --px 12 --baseline 10 --name "Fusion Pixel 12px 1bpp" --symbol ak_font_ui12 \
      --out common/src/ak_font_ui12_data.c \
      --charset tongyong_level1.txt --charset gb2312_level1.txt --charset punct.txt

Glyph set: ASCII 0x20..0x7E, the --charset lists in tools/charset/ (default
tongyong_level1.txt, Tongyong Guifan level 1, 3500 characters, and punct.txt,
CJK punctuation) and every non-ASCII character quoted in
common/src/ak_i18n.c, so all interface strings are always covered. Characters the font does not have are skipped and
reported; the engine draws them (and anything else outside the set, e.g. a
rare character in a chat reply) as the 0x7F box.

Defaults match GNU Unifont 16px: 8x16 ASCII, baseline 14. Non-ASCII glyphs keep
the font's own advance, rounded to a half (8) or full (16) cell, so halfwidth
symbols such as "…" or "·" are not padded to 16 px.
"""
import argparse
import os
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..")
OUT = os.path.join(APP, "common", "src", "ak_font_ui16_data.c")
I18N = os.path.join(APP, "common", "src", "ak_i18n.c")
CHARSETS = os.path.join(HERE, "charset")
CHARSET_FILES = ["tongyong_level1.txt", "punct.txt"]
MISSING = 0x7F


def pack(levels):
    bits = levels.reshape(-1).astype(np.uint8)
    bits = np.concatenate([bits, np.zeros((-len(bits)) % 8, np.uint8)])
    return np.packbits(bits).tobytes()


def render(font, ch, w, h, base):
    im = Image.new("L", (w + h, h + h), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    d.text((0, base), ch, font=font, fill=255, anchor="ls")
    return (np.asarray(im.crop((0, 0, w, h))) >= 128).astype(np.uint8)


def missing_box(w, h):
    a = np.zeros((h, w), np.uint8)
    a[1, 1:w - 1] = a[h - 2, 1:w - 1] = 1
    a[1:h - 1, 1] = a[1:h - 1, w - 2] = 1
    return a


def i18n_chars():
    """Non-ASCII characters inside C string literals of ak_i18n.c (UTF-8 source)."""
    text = open(I18N, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return {c for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', text) for c in lit if ord(c) > 0x7F}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="pixel TTF/OTF")
    ap.add_argument("--px", type=int, default=16, help="cell height; full-width cells are px wide")
    ap.add_argument("--baseline", type=int, default=14, help="baseline row inside the cell")
    ap.add_argument("--ascii-w", type=int, default=None, help="ASCII cell width (default px/2)")
    ap.add_argument("--name", default="GNU Unifont 16px 1bpp")
    ap.add_argument("--symbol", default="ak_font_ui16", help="C symbol of the table (ak_font_ui.h)")
    ap.add_argument("--out", default=OUT, help="output C file")
    ap.add_argument("--charset", action="append", help="file in tools/charset/ (repeatable)")
    a = ap.parse_args()

    px, base = a.px, a.baseline
    ascii_w = a.ascii_w or px // 2
    font = ImageFont.truetype(a.font, px, layout_engine=ImageFont.Layout.BASIC)

    wide = set()
    for name in a.charset or CHARSET_FILES:
        wide |= {c for c in open(os.path.join(CHARSETS, name), encoding="utf-8").read()
                 if ord(c) > 0x7F and not c.isspace()}
    ui = i18n_chars()
    wide |= ui
    wide = {c for c in wide if ord(c) <= 0xFFFF}

    notdef = render(font, "\ue000", px, px, base)  # private use: the font's .notdef
    entries, skipped = {}, []
    for cp in range(0x20, 0x7F):
        entries[cp] = (ascii_w, pack(render(font, chr(cp), ascii_w, px, base)))
    entries[MISSING] = (ascii_w, pack(missing_box(ascii_w, px)))
    for ch in sorted(wide):
        # Unifont is duospaced: keep a halfwidth symbol at half a cell.
        w = px if font.getlength(ch) > px * 3 / 4 else ascii_w
        levels = render(font, ch, w, px, base)
        if not levels.any() or np.array_equal(levels, notdef[:, :w]):
            skipped.append(ch)
            continue
        entries[ord(ch)] = (w, pack(levels))
    lost_ui = sorted(c for c in skipped if c in ui)
    if lost_ui:
        raise SystemExit("font lacks interface characters: " + "".join(lost_ui))

    codes = sorted(entries)
    data, offs, adv = bytearray(), [], []
    for cp in codes:
        offs.append(len(data))
        adv.append(entries[cp][0])
        data += entries[cp][1]

    def rows(vals, fmt, per):
        return ",\n".join("    " + ", ".join(fmt % v for v in vals[i:i + per]) for i in range(0, len(vals), per))

    o = ["/* GENERATED by tools/gen_font_ui.py -- do not edit by hand.\n"
         " * LCD interface font: ASCII + the tools/charset lists + ak_i18n.c strings.\n"
         " * Source: %s (see firmware/app/third_party/fonts/THIRD_PARTY.md).\n"
         " * %d glyphs, %d bitmap bytes. */\n" % (os.path.basename(a.font), len(codes), len(data)),
         '#include "ak_font_ui.h"\n',
         "static const uint16_t s_codes[%d] = {\n%s\n};" % (len(codes), rows(codes, "0x%04X", 8)),
         "static const uint8_t s_adv[%d] = {\n%s\n};" % (len(adv), rows(adv, "%d", 24)),
         "static const uint32_t s_offs[%d] = {\n%s\n};" % (len(offs), rows(offs, "%d", 10)),
         "static const uint8_t s_data[%d] = {\n%s\n};" % (len(data), rows(list(data), "0x%02X", 16)),
         "const ak_font_t %s = {\n    \"%s\", %d, 1, %d, %d,\n    s_codes, s_adv, s_offs, s_data,\n};\n"
         % (a.symbol, a.name, px, px + 2, len(codes))]
    open(a.out, "w", encoding="utf-8", newline="\n").write("\n".join(o))
    flash = len(codes) * (2 + 1 + 4) + len(data)
    print("glyphs %d (CJK/punct %d), skipped %d: %s" % (len(codes), len(codes) - 96, len(skipped), "".join(skipped)))
    print("bitmap %d B, tables+bitmap %d B (%.1f KB)" % (len(data), flash, flash / 1024))


if __name__ == "__main__":
    main()
