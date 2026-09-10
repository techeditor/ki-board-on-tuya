#!/usr/bin/env python3
"""Kiro-ghost mascot sprite pipeline (scaffold).

Adapted from CodeTank's tools/mascot_pipeline.py, whose grid slicing, union
crop and RGB565 RLE output are themselves adapted from the MIT-licensed
clawd-tank tools by Marcio Granzotto Rodrigues.

Differences vs the CodeTank original (this is a SCAFFOLD — source art is authored
by hand later, see docs/kiro-mascot-authoring.md):
  * DEFAULT_MANIFEST / DEFAULT_OUTPUT_DIR point at this probe (lcd_showcase).
  * Pillow is imported lazily, so ``--help`` works with no dependency installed.
  * A missing ``source/<action>.png`` prints a clear "author it first" hint and
    exits non-zero, instead of raising a traceback.
  * The "must be exactly 200 frames" rule is a WARNING, not a hard error, so the
    scaffold does not wedge before any art exists.

Output headers are byte-compatible with the RLE runtime in
``firmware/feature-probes/lcd_showcase/include/app_rle_sprite.h`` and the
registry shape in ``sprite_mascot_registry.h`` (enums from ak_scene.h), so a
freshly packed set drops straight into ak_scene.c.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from pathlib import Path
from typing import Iterable, Sequence


HERE = Path(__file__).resolve().parent
DEFAULT_MANIFEST = HERE / "manifest.json"
DEFAULT_BUILD_DIR = HERE / "build" / "mascot"
# lcd_showcase/tools/kiro_mascot/ -> up 2 -> lcd_showcase/ ; then include/assets/generated
DEFAULT_OUTPUT_DIR = HERE.parents[1] / "include" / "assets" / "generated"


class PipelineError(Exception):
    """A user-actionable pipeline error (printed without a traceback)."""


def _require_pil():
    """Import Pillow lazily so --help / validate work without it installed."""
    try:
        from PIL import Image, ImageDraw  # noqa: F401
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise PipelineError(
            "Pillow is required for slicing/packing. Install it with:\n"
            "    python3 -m pip install Pillow"
        ) from exc
    return Image, ImageDraw


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def validate_manifest(manifest: dict, manifest_path: Path) -> None:
    required = {"schemaVersion", "workCanvas", "baseline", "transparentKey", "palette", "actions"}
    missing = required.difference(manifest)
    if missing:
        raise PipelineError(f"manifest missing fields: {sorted(missing)}")
    if manifest["schemaVersion"] != 1:
        raise PipelineError("unsupported manifest schemaVersion")
    if len(manifest["workCanvas"]) != 2 or min(manifest["workCanvas"]) <= 0:
        raise PipelineError("workCanvas must contain two positive integers")
    names: set[str] = set()
    enums: set[str] = set()
    frame_total = 0
    missing_sources: list[str] = []
    for action in manifest["actions"]:
        for field in ("name", "enum", "frameCount", "fps", "loop", "maxBox"):
            if field not in action:
                raise PipelineError(f"action missing {field}: {action}")
        if action["name"] in names or action["enum"] in enums:
            raise PipelineError(f"duplicate action: {action['name']}")
        names.add(action["name"])
        enums.add(action["enum"])
        frame_total += int(action["frameCount"])
        if "source" in action:
            if "grid" not in action or math.prod(action["grid"]) != action["frameCount"]:
                raise PipelineError(f"grid/frame mismatch for {action['name']}")
            source = manifest_path.parent / action["source"]
            if not source.is_file():
                missing_sources.append(action["source"])
        elif action.get("deriveFrom") not in names:
            raise PipelineError(f"deriveFrom must reference an earlier action: {action['name']}")

    # Soft check: CodeTank targets 200 total frames. This is a scaffold — warn only.
    if frame_total != 200:
        print(f"[warn] total frame count is {frame_total} (CodeTank reference used 200)", file=sys.stderr)

    if missing_sources:
        listing = "\n".join(f"    - {name}" for name in missing_sources)
        raise PipelineError(
            "missing source sprite sheet(s) — author them by hand first, then re-run.\n"
            "See docs/kiro-mascot-authoring.md for the step-by-step drawing guide.\n"
            f"Expected under {manifest_path.parent}/source/ :\n{listing}"
        )


def grid_bounds(length: int, parts: int) -> list[tuple[int, int]]:
    if length <= 0 or parts <= 0:
        raise PipelineError("length and parts must be positive")
    return [(round(i * length / parts), round((i + 1) * length / parts)) for i in range(parts)]


def _content_projection(image, vertical: bool) -> list[int]:
    alpha = image.getchannel("A")
    if vertical:
        return [sum(bool(alpha.getpixel((x, y))) for y in range(image.height)) for x in range(image.width)]
    return [sum(bool(alpha.getpixel((x, y))) for x in range(image.width)) for y in range(image.height)]


def _adaptive_grid_bounds(projection: Sequence[int], parts: int) -> list[tuple[int, int]]:
    """Find background valleys near nominal grid boundaries."""
    length = len(projection)
    cell = length / parts
    separators = [0]
    for index in range(1, parts):
        nominal = index * cell
        start = max(separators[-1] + 1, round(nominal - cell * 0.35))
        end = min(length - 1, round(nominal + cell * 0.35))
        minimum = min(projection[start:end + 1])
        candidates = [pos for pos in range(start, end + 1) if projection[pos] == minimum]
        groups: list[list[int]] = []
        for position in candidates:
            if not groups or position != groups[-1][-1] + 1:
                groups.append([position])
            else:
                groups[-1].append(position)
        group = min(groups, key=lambda item: (abs((item[0] + item[-1]) / 2 - nominal), -len(item)))
        separators.append(round((group[0] + group[-1]) / 2))
    separators.append(length)
    return list(zip(separators[:-1], separators[1:]))


def chroma_to_alpha(image):
    """Remove green-dominant chroma pixels, including dark green edge blends."""
    rgba = image.convert("RGBA")
    output = []
    for red, green, blue, _alpha in rgba.getdata():
        is_key = green >= 40 and green >= red + 25 and green >= blue + 25
        output.append((0, 0, 0, 0) if is_key else (red, green, blue, 255))
    rgba.putdata(output)
    return rgba


def split_foreground_grid(sheet, columns: int, rows: int):
    keyed = chroma_to_alpha(sheet)
    xs = _adaptive_grid_bounds(_content_projection(keyed, vertical=True), columns)
    ys = _adaptive_grid_bounds(_content_projection(keyed, vertical=False), rows)
    return [keyed.crop((x0, y0, x1, y1)) for y0, y1 in ys for x0, x1 in xs]


def alpha_bbox(image):
    return image.getchannel("A").getbbox()


def union_bbox(images):
    boxes = [alpha_bbox(image) for image in images]
    if any(box is None for box in boxes):
        raise PipelineError("animation contains an empty frame")
    typed = [box for box in boxes if box is not None]
    return (
        min(box[0] for box in typed), min(box[1] for box in typed),
        max(box[2] for box in typed), max(box[3] for box in typed),
    )


def load_palette(path: Path) -> list[tuple[int, int, int]]:
    data = load_json(path)
    colors = data.get("colors", [])
    if len(colors) != 32:
        raise PipelineError(f"shared palette must have exactly 32 colors, got {len(colors)}")
    parsed = []
    for value in colors:
        if not isinstance(value, str) or len(value) != 7 or not value.startswith("#"):
            raise PipelineError(f"invalid palette color: {value!r}")
        parsed.append(tuple(int(value[index:index + 2], 16) for index in (1, 3, 5)))
    return parsed


def apply_palette(image, palette):
    rgba = image.convert("RGBA")
    cache: dict = {}
    output = []
    for red, green, blue, alpha in rgba.getdata():
        if alpha == 0:
            output.append((0, 0, 0, 0))
            continue
        source = (red, green, blue)
        target = cache.get(source)
        if target is None:
            target = min(palette, key=lambda color: sum((color[i] - source[i]) ** 2 for i in range(3)))
            cache[source] = target
        output.append((*target, 255))
    rgba.putdata(output)
    return rgba


def normalize_action(frames, action: dict, manifest: dict, palette):
    Image, _ = _require_pil()
    source_boxes = [alpha_bbox(frame) for frame in frames]
    if any(box is None for box in source_boxes):
        raise PipelineError(f"{action['name']}: animation contains an empty frame")
    typed_boxes = [box for box in source_boxes if box is not None]
    source_width = max(box[2] - box[0] for box in typed_boxes)
    source_height = max(box[3] - box[1] for box in typed_boxes)
    max_width, max_height = action["maxBox"]
    scale = min(max_width / source_width, max_height / source_height)
    canvas_width, canvas_height = manifest["workCanvas"]
    baseline = int(manifest["baseline"])
    normalized = []
    for frame, source_box in zip(frames, typed_boxes):
        cropped = frame.crop(source_box)
        target_width = max(1, round(cropped.width * scale))
        target_height = max(1, round(cropped.height * scale))
        resized = cropped.resize((target_width, target_height), Image.Resampling.NEAREST)
        canvas = Image.new("RGBA", (canvas_width, canvas_height), (0, 0, 0, 0))
        x = (canvas_width - target_width) // 2
        y = baseline - target_height + 1
        canvas.alpha_composite(resized, (x, y))
        normalized.append(apply_palette(canvas, palette))

    final_box = union_bbox(normalized)
    padding = 2
    final_box = (
        max(0, final_box[0] - padding), max(0, final_box[1] - padding),
        min(canvas_width, final_box[2] + padding), min(canvas_height, final_box[3] + padding),
    )
    cropped_frames = [frame.crop(final_box) for frame in normalized]
    content_bottom = final_box[3] - 1
    y_offset = int(manifest["groundOffset"]) - (content_bottom - baseline)
    return cropped_frames, y_offset


def derive_mini(frames, action: dict, palette):
    Image, _ = _require_pil()
    width, height = action["maxBox"]
    result = []
    for frame in frames[:action["frameCount"]]:
        box = alpha_bbox(frame)
        if box is None:
            raise PipelineError("cannot derive mini mascot from an empty frame")
        crop = frame.crop(box)
        crop.thumbnail((width, height), Image.Resampling.NEAREST)
        canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        canvas.alpha_composite(crop, ((width - crop.width) // 2, height - crop.height))
        result.append(apply_palette(canvas, palette))
    return result, int(action.get("yOffset", -5))


def process(manifest: dict, manifest_path: Path, build_dir: Path) -> dict:
    Image, _ = _require_pil()
    palette = load_palette(manifest_path.parent / manifest["palette"])
    frames_root = build_dir / "frames"
    if frames_root.exists():
        shutil.rmtree(frames_root)
    frames_root.mkdir(parents=True)
    metadata = {"schemaVersion": 1, "actions": []}
    processed: dict = {}

    for action in manifest["actions"]:
        name = action["name"]
        if "source" in action:
            source = Image.open(manifest_path.parent / action["source"]).convert("RGB")
            columns, rows = action["grid"]
            cells = split_foreground_grid(source, columns, rows)
            if len(cells) != action["frameCount"]:
                raise PipelineError(f"unexpected frame count for {name}")
            frames, y_offset = normalize_action(cells, action, manifest, palette)
        else:
            frames, y_offset = derive_mini(processed[action["deriveFrom"]], action, palette)

        action_dir = frames_root / name
        action_dir.mkdir()
        for index, frame in enumerate(frames):
            frame.save(action_dir / f"frame_{index:03d}.png", optimize=True)
        processed[name] = frames
        colors = {pixel[:3] for frame in frames for pixel in frame.getdata() if pixel[3]}
        if len(colors) > 32:
            raise PipelineError(f"{name}: palette expanded to {len(colors)} colors")
        metadata["actions"].append({
            "name": name, "width": frames[0].width, "height": frames[0].height,
            "frameCount": len(frames), "yOffset": y_offset, "colors": len(colors),
        })
        print(f"processed {name:14s} {len(frames):2d} frames {frames[0].width:3d}x{frames[0].height:<3d} colors={len(colors):2d}")

    metadata_path = build_dir / "processed.json"
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def load_processed_frames(build_dir: Path, action: dict):
    Image, _ = _require_pil()
    paths = sorted((build_dir / "frames" / action["name"]).glob("frame_*.png"))
    if len(paths) != action["frameCount"]:
        raise PipelineError(f"run 'process' first: {action['name']} has {len(paths)} processed frames")
    return [Image.open(path).convert("RGBA") for path in paths]


def composite_preview(frame, scale: int = 3):
    Image, _ = _require_pil()
    background = Image.new("RGBA", frame.size, (16, 16, 24, 255))
    background.alpha_composite(frame)
    return background.convert("RGB").resize((frame.width * scale, frame.height * scale), Image.Resampling.NEAREST)


def preview(manifest: dict, build_dir: Path) -> None:
    Image, ImageDraw = _require_pil()
    preview_dir = build_dir / "previews"
    preview_dir.mkdir(parents=True, exist_ok=True)
    gallery_items = []
    for action in manifest["actions"]:
        frames = load_processed_frames(build_dir, action)
        scaled = [composite_preview(frame) for frame in frames]
        scaled[0].save(preview_dir / f"{action['name']}.gif", save_all=True,
                       append_images=scaled[1:], duration=1000 // action["fps"], loop=0)
        gallery_items.append((action["name"], scaled[0]))

    tile_width = max(image.width for _, image in gallery_items) + 16
    tile_height = max(image.height for _, image in gallery_items) + 32
    columns = 4
    rows = math.ceil(len(gallery_items) / columns)
    gallery = Image.new("RGB", (columns * tile_width, rows * tile_height), (16, 16, 24))
    draw = ImageDraw.Draw(gallery)
    for index, (name, image) in enumerate(gallery_items):
        x = (index % columns) * tile_width + (tile_width - image.width) // 2
        y = (index // columns) * tile_height + 18
        gallery.paste(image, (x, y))
        draw.text(((index % columns) * tile_width + 4, (index // columns) * tile_height + 3), name, fill=(200, 180, 255))
    gallery.save(preview_dir / "gallery.png", optimize=True)
    print(f"wrote previews to {preview_dir}")


def rgb888_to_rgb565(red: int, green: int, blue: int) -> int:
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


def frame_to_words(frame, transparent_key: int) -> list[int]:
    words = []
    for red, green, blue, alpha in frame.convert("RGBA").getdata():
        value = transparent_key if alpha == 0 else rgb888_to_rgb565(red, green, blue)
        if alpha and value == transparent_key:
            value ^= 0x0001
        words.append(value)
    return words


def rle_encode(words: Sequence[int]) -> list[int]:
    if not words:
        return []
    encoded = []
    current = words[0]
    count = 1
    for value in words[1:]:
        if value == current and count < 0xFFFF:
            count += 1
        else:
            encoded.extend((current, count))
            current = value
            count = 1
    encoded.extend((current, count))
    return encoded


def rle_decode(encoded: Sequence[int], pixel_count: int) -> list[int]:
    if len(encoded) % 2:
        raise PipelineError("RLE word stream must contain value/count pairs")
    output = []
    for index in range(0, len(encoded), 2):
        output.extend([encoded[index]] * encoded[index + 1])
    if len(output) != pixel_count:
        raise PipelineError(f"RLE decoded {len(output)} pixels, expected {pixel_count}")
    return output


def c_array(values: Iterable[int], formatter, per_line: int = 12) -> str:
    items = [formatter(value) for value in values]
    return "\n".join("    " + ", ".join(items[index:index + per_line]) + "," for index in range(0, len(items), per_line))


def action_asset_stem(action_name: str) -> str:
    return "mini_mascot" if action_name == "mini_mascot" else f"mascot_{action_name}"


def write_action_header(output_dir: Path, action: dict, frames, y_offset: int, transparent_key: int):
    name = action["name"]
    symbol = action_asset_stem(name)
    macro = f"MASCOT_{name.upper()}"
    offsets = [0]
    encoded_all = []
    for frame in frames:
        raw = frame_to_words(frame, transparent_key)
        encoded = rle_encode(raw)
        if rle_decode(encoded, frame.width * frame.height) != raw:
            raise PipelineError(f"RLE round-trip failed for {name}")
        encoded_all.extend(encoded)
        offsets.append(len(encoded_all))
    content = f"""#pragma once

#include <stdint.h>

#define {macro}_WIDTH {frames[0].width}
#define {macro}_HEIGHT {frames[0].height}
#define {macro}_FRAME_COUNT {len(frames)}
#define {macro}_Y_OFFSET {y_offset}

static const uint32_t {symbol}_frame_offsets[{len(offsets)}] = {{
{c_array(offsets, str, 10)}
}};

static const uint16_t {symbol}_rle_data[{len(encoded_all)}] = {{
{c_array(encoded_all, lambda value: f'0x{value:04X}', 10)}
}};
"""
    (output_dir / f"sprite_{symbol}.h").write_text(content, encoding="utf-8")
    return frames[0].width, frames[0].height, len(encoded_all)


def pack(manifest: dict, manifest_path: Path, build_dir: Path, output_dir: Path) -> None:
    metadata = load_json(build_dir / "processed.json")
    meta_by_name = {entry["name"]: entry for entry in metadata["actions"]}
    transparent_key = int(manifest["transparentKey"], 0)
    output_dir.mkdir(parents=True, exist_ok=True)
    for old in output_dir.glob("sprite_mascot_*.h"):
        old.unlink()
    (output_dir / "sprite_mini_mascot.h").unlink(missing_ok=True)
    total_words = 0
    includes = []
    entries = []
    for action in manifest["actions"]:
        frames = load_processed_frames(build_dir, action)
        meta = meta_by_name[action["name"]]
        width, height, words = write_action_header(output_dir, action, frames, meta["yOffset"], transparent_key)
        total_words += words
        symbol = action_asset_stem(action["name"])
        includes.append(f'#include "sprite_{symbol}.h"')
        entries.append(f"""    [{action['enum']}] = {{
        .rle_data = {symbol}_rle_data,
        .frame_offsets = {symbol}_frame_offsets,
        .frame_count = {action['frameCount']},
        .frame_ms = {1000 // action['fps']},
        .looping = {'true' if action['loop'] else 'false'},
        .width = {width},
        .height = {height},
        .y_offset = {meta['yOffset']},
    }},""")
    registry = f"""#pragma once

#include <stdbool.h>
#include <stdint.h>

{chr(10).join(includes)}

typedef struct {{
    const uint16_t *rle_data;
    const uint32_t *frame_offsets;
    int frame_count;
    int frame_ms;
    bool looping;
    int width;
    int height;
    int y_offset;
}} mascot_anim_def_t;

static const mascot_anim_def_t mascot_anim_defs[MASCOT_ANIM_COUNT] = {{
{chr(10).join(entries)}
}};
"""
    (output_dir / "sprite_mascot_registry.h").write_text(registry, encoding="utf-8")
    print(f"packed {sum(action['frameCount'] for action in manifest['actions'])} frames, {total_words * 2} RLE bytes")
    print(f"output -> {output_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Kiro-ghost mascot sprite pipeline (scaffold).")
    parser.add_argument("command", choices=("validate", "process", "preview", "pack", "all"),
                        help="validate: check manifest+sources only; all: process+preview+pack")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    args = parser.parse_args()

    try:
        manifest_path = args.manifest.resolve()
        manifest = load_json(manifest_path)
        validate_manifest(manifest, manifest_path)
        if args.command == "validate":
            print("manifest + sources OK")
            return 0
        if args.command in ("process", "all"):
            process(manifest, manifest_path, args.build_dir)
        if args.command in ("preview", "all"):
            preview(manifest, args.build_dir)
        if args.command in ("pack", "all"):
            pack(manifest, manifest_path, args.build_dir, args.output_dir)
    except PipelineError as exc:
        print(f"\n[error] {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
