# Third-party fonts

`common/src/ak_font_data.c` embeds small 1bpp bitmap subsets (ASCII plus the
characters in `tools/font_sample.txt`, about 13 KB of glyph data in total) taken
from the fonts below. The font files themselves are not in this repository.

| Font | Version / source | Copyright | Licence | Licence text |
|---|---|---|---|---|
| Fusion Pixel Font (12px monospaced, zh_hans) | 2026.11.10, <https://github.com/TakWolf/fusion-pixel-font/releases> | (c) 2022 TakWolf. Built from Ark Pixel, Cubic-11 and Galmuri, whose licences are bundled too. | SIL OFL 1.1 | `fusion-pixel/OFL.txt`, `fusion-pixel/ark-pixel/OFL.txt`, `fusion-pixel/cubic-11/OFL.txt`, `fusion-pixel/galmuri/LICENSE.txt` |
| GNU Unifont (16px) | 18.0.01, <https://ftp.gnu.org/gnu/unifont/unifont-18.0.01/> | (c) 1998-2026 Roman Czyborra, Paul Hardy, Qianqian Fang, et al. | Dual: SIL OFL 1.1, or GPLv2+ with the font embedding exception. **We use it under the OFL.** | `unifont/LICENSE.txt` |
| Chill Bitmap 16px (寒蝉点阵体) | v2.502, <https://github.com/Warren2060/ChillBitmap/releases> | Unifont authors (glyph base); (c) 2024 ChillType / Warren2060 (revised Latin and punctuation) | OFL 1.1 with Reserved Font Name "ChillBitmap"; the bundled licence file also carries Unifont's dual licence | `chill-bitmap/OFL.txt` (repo `License`), `chill-bitmap/LICENSE.txt` (release zip) |
| ISAS 16x16 Song (`gb16st.bdf`) | X.Org font-isas-misc 1.0.4, <https://xorg.freedesktop.org/releases/individual/font/> | (c) 1988 The Institute of Software, Academia Sinica | Permissive (use, copy, modify, distribute for any purpose, keep the notice, no endorsement) | `isas/COPYING` |

The ISAS font has no ASCII; its test entry draws ASCII with the Unifont glyphs.

OFL notes: the OFL allows embedding and redistribution, including commercially,
as long as the copyright notice and licence text travel with it, and the fonts are
not sold on their own. A subset is a Modified Version, so it must not use a
Reserved Font Name. Chill Bitmap declares "ChillBitmap" as one, so its subset is
labelled "Unifont 16px alt Latin" on screen. Fusion Pixel and Unifont declare
none (Unifont's licence file leaves the RFN template unfilled).

Regenerate the data (needs Pillow, numpy, fontTools and the four font files):

```
python3 tools/gen_font_data.py --fusion fusion-pixel-12px-monospaced-zh_hans.ttf \
    --unifont unifont-18.0.01.otf --chill ChillBitmap_16px.ttf --isas gb16st.bdf
```

Also in the firmware: `common/include/ak_font8x8.h` (font8x8 by Daniel Hepper, public domain).

## Interface font (`common/src/ak_font_ui_data.c`)

The LCD interface font for Chinese / mixed text (FR-01) is Fusion Pixel Font
12px monospaced zh_hans (same file, licence and rules as above), 1bpp, about
4000 glyphs: ASCII, the characters in `tools/charset/` (Tongyong Guifan Hanzi
Biao level 1, GB2312 level 1, CJK punctuation; character lists, not font data)
and every character used in `common/src/ak_i18n.c`. About 97 KB of flash.
Regenerate it, or swap in another pixel font, with:

```
python3.12 tools/gen_font_ui.py --font fusion-pixel-12px-monospaced-zh_hans.ttf
```

Not used on purpose: Zpix (commercial use is a paid licence) and WenQuanYi bitmap
fonts (GPLv2 with a font embedding exception; unclear for firmware images).
The comparisons are in the LCD font evaluation reports (2026-11-16, and the
14/16 px round of 2026-11-17).
