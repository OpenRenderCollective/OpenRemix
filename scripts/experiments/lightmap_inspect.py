#!/usr/bin/env python3
"""Preview a Forward+ publication dump without changing its cache or textures.

Requires numpy and Pillow. Input is REMIX_DEBUG_BAKE_CAPTURE_PREFIX output:
<prefix>.surfaces-published.bin and <prefix>.surface-charts.bin. The layout
mirrors forward/lightmap/data.h (20 RGBA16F layers per page).
"""
import argparse
import colorsys
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def font(size):
    for name in ("C:/Windows/Fonts/segoeui.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def irradiance_preview(values, exposure):
    linear = np.maximum(values.astype(np.float32), 0) * exposure
    linear = linear / (1 + linear)
    srgb = np.where(linear <= .0031308, linear * 12.92,
                    1.055 * np.power(linear, 1 / 2.4) - .055)
    return Image.fromarray(np.rint(np.clip(srgb, 0, 1) * 255).astype(np.uint8))


def probe_summary(prefix, metres_per_unit):
    path = Path(str(prefix) + ".irradiance-published.bin")
    if not path.exists():
        return None
    if path.stat().st_size < 48 or (path.stat().st_size - 48) % 256:
        raise ValueError("probe dump does not contain a complete header and 256-byte SH records")
    header = np.fromfile(path, dtype="<f4", count=12)
    dimensions = np.fromfile(path, dtype="<u4", count=12)[4:8]
    count = (path.stat().st_size - 48) // 256
    if not count or int(np.prod(dimensions[:3].astype(np.uint64))) != count or dimensions[3] != count:
        raise ValueError("probe dimensions disagree with the published record count")
    data = np.memmap(path, dtype="<f4", mode="r", offset=48, shape=(count, 16, 4))
    spacing = float(header[3])
    if not np.isfinite(spacing) or spacing <= 0:
        raise ValueError("probe spacing must be finite and positive")
    valid = int(np.count_nonzero(data[:, 15, 3] >= .5))
    relocated = int(np.count_nonzero(np.any(data[:, :3, 3] != 0, axis=1))) if header[10] > .5 else 0
    return dict(source=str(path), dimensions=dimensions[:3].tolist(), allocated_probes=count,
                valid_probes=valid, invalid_or_untraced_probes=count - valid,
                relocated_probes=relocated,
                spacing_world_units=spacing,
                spacing_metres=spacing * metres_per_unit if metres_per_unit else None,
                limitations="Uniform grid spacing/validity alone does not measure surface/probe shading error")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--page-size", type=int, default=1024)
    parser.add_argument("--metres-per-unit", type=float)
    args = parser.parse_args()
    if args.page_size <= 0:
        parser.error("page size must be positive")
    if args.metres_per_unit is not None and (not np.isfinite(args.metres_per_unit) or args.metres_per_unit <= 0):
        parser.error("metres per unit must be finite and positive")
    pixels = Path(str(args.prefix) + ".surfaces-published.bin")
    chart_file = Path(str(args.prefix) + ".surface-charts.bin")
    page_bytes = args.page_size ** 2 * 20 * 4 * 2
    if not pixels.stat().st_size or pixels.stat().st_size % page_bytes:
        parser.error("surface dump does not contain complete 20-layer RGBA16F pages")
    if chart_file.stat().st_size % 20:
        parser.error("chart dump does not contain complete five-uint allocations")
    count = pixels.stat().st_size // page_bytes
    atlas = np.memmap(pixels, dtype="<f2", mode="r",
                      shape=(count, 20, args.page_size, args.page_size, 4))
    charts = np.fromfile(chart_file, dtype="<u4").reshape(-1, 5)
    bounds = charts.astype(np.uint64)
    if np.any(charts[:, 0] >= count) or np.any(bounds[:, 1:3] + bounds[:, 3:5] > args.page_size):
        parser.error("chart allocation falls outside the atlas")
    # The constant SH term is orientation-independent, not the complete
    # irradiance evaluated at each receiver normal. Do not add unit-light SH
    # or shadow channels to it as though those channels contained RGB light.
    l0 = np.asarray(atlas[:, 0, :, :, :3], dtype=np.float32) * .2820947918
    if not np.all(np.isfinite(l0)):
        parser.error("non-finite indirect coefficient")
    positive = l0[l0 > 0]
    white = float(np.percentile(positive, 99)) if positive.size else 1
    exposure = 1 / max(white, 1e-8)
    probes = probe_summary(args.prefix, args.metres_per_unit)
    args.output.mkdir(parents=True, exist_ok=False)
    tile, gap, top, label = 300, 14, 126, 30
    sheet = Image.new("RGB", (gap + count * (tile + gap), top + 3 * (tile + label + gap)), "#17202a")
    draw = ImageDraw.Draw(sheet)
    draw.text((gap, 10), "OpenRemix / actual packed lightmap pages", font=font(25), fill="white")
    draw.text((gap, 47), f"{count} pages, {args.page_size} x {args.page_size}, {len(charts)} chart rectangles",
              font=font(17), fill="#cad6df")
    draw.text((gap, 74), "Indirect row: L0 SH term with shared exposure; not the final combined lighting.",
              font=font(15), fill="#cad6df")
    draw.text((gap, 96), "Rectangles include padding. Sun mask: white = clear, black = occluded/unallocated.",
              font=font(15), fill="#cad6df")
    for page in range(count):
        chart_image = Image.new("RGB", (args.page_size, args.page_size))
        boxes = ImageDraw.Draw(chart_image)
        for index, (owner, x, y, width, height) in enumerate(charts.tolist()):
            if owner == page and width and height:
                hue = (index * .61803398875) % 1
                color = tuple(round(c * 255) for c in colorsys.hsv_to_rgb(hue, .65, .9))
                boxes.rectangle((x, y, x + width - 1, y + height - 1), fill=color)
        indirect = irradiance_preview(l0[page], exposure)
        sun = Image.fromarray(np.rint(np.clip(atlas[page, 19, :, :, 0].astype(np.float32), 0, 1) * 255)
                               .astype(np.uint8)).convert("RGB")
        views = (("chart-rectangles", chart_image), ("indirect-l0", indirect), ("sun-visibility", sun))
        for row, (name, image) in enumerate(views):
            image.save(args.output / f"page-{page}-{name}.png")
            x, y = gap + page * (tile + gap), top + row * (tile + label + gap)
            draw.text((x, y), f"Page {page} / {name}", font=font(15), fill="white")
            sheet.paste(image.resize((tile, tile), Image.Resampling.NEAREST), (x, y + label))
    sheet.save(args.output / "overview.png")
    metadata = dict(source=str(pixels), source_bytes=pixels.stat().st_size,
                    page_size=args.page_size, pages=count, charts=len(charts),
                    bytes_per_surface_texel=160, indirect_l0_exposure=exposure,
                    limitations=["L0 is not the full directional irradiance or combined direct+indirect light",
                                 "Chart rectangles include padding; they are not an occupancy measurement",
                                 "Twenty layers include signed SH, unit-light responses and visibility"])
    metadata["diffuse_probes"] = probes
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(args.output / "overview.png")


if __name__ == "__main__":
    main()
