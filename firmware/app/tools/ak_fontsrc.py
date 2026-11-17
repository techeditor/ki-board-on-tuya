"""Host-side font sources for the Ki-board LCD: BDF / pixel TTF / Noto -> level bitmaps.

A Face maps a BMP code point to (advance, levels) where levels is an
h x advance uint8 array (0..2^bpp-1) and h is the face's cell height. That is
exactly what common/src/ak_font.c draws, so gen_font_data.py and the review
previews share one loader and cannot disagree about a glyph.

Needs Pillow and numpy; TTF/OTF sources also need fontTools (for the cmap).
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont


class Face:
    def __init__(self, name, h, bpp=1):
        self.name, self.h, self.bpp = name, h, bpp
        self.g = {}

    def has(self, cp):
        return cp in self.g

    def glyph(self, cp):
        """(adv, levels) or None."""
        return self.g.get(cp)


def load_bdf(path, ascent, height, name, gb2312=False):
    """BDF bitmap font. Each glyph is placed in a height-row cell whose baseline
    sits `ascent` rows from the top; ink outside the cell or the advance is clipped.
    gb2312=True decodes GL (94x94) encodings, as in the X11 ISAS fonts."""
    face = Face(name, height)
    enc = dw = None
    bbx = (0, 0, 0, 0)
    rows = None
    for line in open(path, encoding="latin-1"):
        k = line.split()
        if not k:
            continue
        if rows is not None and k[0] != "ENDCHAR":
            rows.append(k[0])
            continue
        if k[0] == "ENCODING":
            enc = int(k[1])
        elif k[0] == "DWIDTH":
            dw = int(k[1])
        elif k[0] == "BBX":
            bbx = tuple(int(x) for x in k[1:5])
        elif k[0] == "BITMAP":
            rows = []
        elif k[0] == "ENDCHAR":
            cp = enc
            if gb2312 and enc is not None:
                try:
                    cp = ord(bytes([(enc >> 8) | 0x80, (enc & 0xFF) | 0x80]).decode("gb2312"))
                except (UnicodeDecodeError, TypeError, ValueError):
                    cp = -1
            if cp is not None and 0x20 <= cp <= 0xFFFF and dw:
                bw, bh, bx, by = bbx
                a = np.zeros((height, dw), np.uint8)
                top = ascent - (by + bh)
                for i, hx in enumerate(rows):
                    v, nb = int(hx, 16), len(hx) * 4
                    r = top + i
                    if not 0 <= r < height:
                        continue
                    for c in range(bw):
                        x = bx + c
                        if 0 <= x < dw and (v >> (nb - 1 - c)) & 1:
                            a[r, x] = 1
                face.g[cp] = (dw, a)
            rows = None
            enc = dw = None
    return face


class TtfFace(Face):
    """Outline or pixel-outline TTF/OTF rendered by FreeType at one size.
    bpp=1 uses FreeType's monochrome rasterizer; 2/4 quantise the AA coverage.
    cjk_adv forces the advance of non-ASCII glyphs (fixed-cell CJK)."""

    def __init__(self, path, size, ascent, height, name, bpp=1, cjk_adv=None):
        super().__init__(name, height, bpp)
        from fontTools.ttLib import TTFont

        self.cmap = set(TTFont(path, fontNumber=0).getBestCmap())
        self.font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
        try:
            self.font.set_variation_by_name("Regular")
        except Exception:
            pass
        self.size, self.ascent, self.cjk_adv = size, ascent, cjk_adv

    def has(self, cp):
        return cp in self.cmap

    def glyph(self, cp):
        if cp in self.g:
            return self.g[cp]
        if cp not in self.cmap:
            return None
        ch = chr(cp)
        adv = self.cjk_adv if (self.cjk_adv and cp >= 0x80) else max(1, round(self.font.getlength(ch)))
        im = Image.new("L", (adv + self.size * 2, self.h + self.size), 0)
        d = ImageDraw.Draw(im)
        d.fontmode = "1" if self.bpp == 1 else "L"
        d.text((0, self.ascent), ch, font=self.font, fill=255, anchor="ls")
        a = np.asarray(im.crop((0, 0, adv, self.h)), dtype=np.float32)
        if self.bpp == 1:
            lv = (a >= 128).astype(np.uint8)
        else:
            lv = np.round(a / 255 * ((1 << self.bpp) - 1)).astype(np.uint8)
        self.g[cp] = (adv, lv)
        return self.g[cp]


class Merged(Face):
    """ASCII (< 0x80) from `latin`, everything else from `cjk`, same cell."""

    def __init__(self, name, cjk, latin):
        assert cjk.h == latin.h and cjk.bpp == latin.bpp, (cjk.name, latin.name)
        super().__init__(name, cjk.h, cjk.bpp)
        self.cjk, self.latin = cjk, latin

    def _src(self, cp):
        return self.latin if cp < 0x80 else self.cjk

    def has(self, cp):
        return self._src(cp).has(cp)

    def glyph(self, cp):
        return self._src(cp).glyph(cp)


class Scaled(Face):
    """Integer nearest-neighbour scale of another face (k x k pixels per dot)."""

    def __init__(self, base, k, name):
        super().__init__(name, base.h * k, base.bpp)
        self.base, self.k = base, k

    def has(self, cp):
        return self.base.has(cp)

    def glyph(self, cp):
        g = self.base.glyph(cp)
        if g is None:
            return None
        return g[0] * self.k, np.kron(g[1], np.ones((self.k, self.k), np.uint8))


def missing_box(w, h, level=1):
    a = np.zeros((h, w), np.uint8)
    a[1, 1:w - 1] = a[h - 2, 1:w - 1] = level
    a[1:h - 1, 1] = a[1:h - 1, w - 2] = level
    return a


def glyph_or_box(face, cp, box_w):
    g = face.glyph(cp)
    if g is not None:
        return g
    return box_w, missing_box(box_w, face.h, (1 << face.bpp) - 1)


def packed_size(adv, h, bpp):
    """Bytes of one glyph in ak_font_data.c (whole bytes per glyph)."""
    return (adv * h * bpp + 7) // 8


# ak_font_t tables per glyph: codes uint16 + adv uint8 + offs uint32
TABLE_BYTES_PER_GLYPH = 7
