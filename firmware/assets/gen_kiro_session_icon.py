#!/usr/bin/env python3
"""Generate the 32x32 Kiro session icon from the canonical SVG source.

The SVG is rasterized with librsvg, centered on a transparent square canvas,
and converted to the MSB-first 1-bpp layout consumed by ak_gfx_icon().
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

WIDTH = 32
HEIGHT = 32
THRESHOLD = 128


def render_rgba(source: Path, preview: Path) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        fitted = Path(tmp) / "fitted.png"
        padded = Path(tmp) / "padded.png"
        subprocess.run(
            [
                "rsvg-convert",
                f"--width={WIDTH}",
                f"--height={HEIGHT}",
                "--keep-aspect-ratio",
                str(source),
                "--output",
                str(fitted),
            ],
            check=True,
        )
        subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-i",
                str(fitted),
                "-vf",
                f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:color=black@0",
                str(padded),
            ],
            check=True,
        )
        rgba = subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(padded),
                "-f",
                "rawvideo",
                "-pix_fmt",
                "rgba",
                "-",
            ],
            check=True,
            stdout=subprocess.PIPE,
        ).stdout
        preview.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(padded, preview)
    expected = WIDTH * HEIGHT * 4
    if len(rgba) != expected:
        raise RuntimeError(f"expected {expected} RGBA bytes, got {len(rgba)}")
    return rgba


def pack_bitmap(rgba: bytes) -> list[int]:
    packed: list[int] = []
    for y in range(HEIGHT):
        for byte_x in range(WIDTH // 8):
            value = 0
            for bit in range(8):
                x = byte_x * 8 + bit
                offset = (y * WIDTH + x) * 4
                red, green, blue, alpha = rgba[offset : offset + 4]
                luminance = (299 * red + 587 * green + 114 * blue) // 1000
                if alpha >= THRESHOLD and luminance >= THRESHOLD:
                    value |= 0x80 >> bit
            packed.append(value)
    return packed


def write_header(output: Path, bitmap: list[int], source: Path) -> None:
    rows = []
    stride = WIDTH // 8
    for y in range(HEIGHT):
        row = bitmap[y * stride : (y + 1) * stride]
        rows.append("    " + ", ".join(f"0x{value:02X}" for value in row) + ",")
    body = "\n".join(rows)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        f"""/**
 * @file ak_session_icons.h
 * @brief 32x32 1-bpp Kiro session icon generated from {source.name}.
 *
 * Row-major, MSB-first, {stride} bytes per row. Set pixels use the session
 * status foreground color; clear pixels reveal the slot background.
 */

#ifndef AK_SESSION_ICONS_H
#define AK_SESSION_ICONS_H

#include <stdint.h>

#define AK_SESSION_ICON_W {WIDTH}U
#define AK_SESSION_ICON_H {HEIGHT}U
#define AK_SESSION_ICON_BYTES {len(bitmap)}U

static const uint8_t AK_SESSION_ICON_KIRO_GHOST[AK_SESSION_ICON_BYTES] = {{
{body}
}};

#endif /* AK_SESSION_ICONS_H */
"""
    )


def main() -> None:
    assets = Path(__file__).resolve().parent
    repo = assets.parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=assets / "AWS_KIRO_Ghost_Outline.svg")
    parser.add_argument(
        "--header",
        type=Path,
        default=repo / "firmware/app/common/include/ak_session_icons.h",
    )
    parser.add_argument(
        "--preview",
        type=Path,
        default=assets / "png32/AWS_KIRO_Ghost_Outline.png",
    )
    args = parser.parse_args()

    rgba = render_rgba(args.source, args.preview)
    bitmap = pack_bitmap(rgba)
    write_header(args.header, bitmap, args.source)
    print(f"generated {args.header} ({len(bitmap)} bytes) and {args.preview}")


if __name__ == "__main__":
    main()
