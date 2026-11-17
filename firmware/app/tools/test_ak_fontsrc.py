#!/usr/bin/env python3
"""Self-test for the BDF loader in ak_fontsrc.py: python3 tools/test_ak_fontsrc.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ak_fontsrc import load_bdf  # noqa: E402

# One GB2312 glyph (GL 0x5650 = U+4E2D 中) 4x3 at the baseline, one clipped glyph.
BDF = """STARTFONT 2.1
CHARS 2
STARTCHAR a
ENCODING 22096
DWIDTH 4 0
BBX 4 3 0 0
BITMAP
F0
90
60
ENDCHAR
STARTCHAR b
ENCODING 8481
DWIDTH 2 0
BBX 4 2 0 -2
BITMAP
F0
F0
ENDCHAR
ENDFONT
"""


def main():
    with tempfile.NamedTemporaryFile("w", suffix=".bdf", delete=False) as fp:
        fp.write(BDF)
    try:
        f = load_bdf(fp.name, ascent=4, height=5, name="t", gb2312=True)
    finally:
        os.unlink(fp.name)
    adv, a = f.glyph(0x4E2D)
    assert adv == 4 and a.shape == (5, 4), (adv, a.shape)
    assert a[:, :].tolist() == [[0] * 4, [1, 1, 1, 1], [1, 0, 0, 1], [0, 1, 1, 0], [0] * 4], a.tolist()
    adv, b = f.glyph(0x3000)  # GL 0x2121 = U+3000; ink below the cell and right of the advance is clipped
    assert adv == 2 and b.tolist() == [[0, 0]] * 4 + [[1, 1]], b.tolist()
    print("ak_fontsrc OK")


if __name__ == "__main__":
    main()
