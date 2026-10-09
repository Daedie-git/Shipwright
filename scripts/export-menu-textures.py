#!/usr/bin/env python3
"""Export PAL 1.0 menu textures from oot.o2r. Requires Pillow and PyYAML."""
import argparse
from pathlib import Path
import struct
import zipfile

from PIL import Image, ImageDraw
import yaml

ROOT = Path(__file__).resolve().parents[1]


def decode(data, fmt, width, height):
    pixels = []
    if fmt == "RGBA32":
        return Image.frombytes("RGBA", (width, height), data)
    if fmt in ("I4", "IA4"):
        values = [n for b in data for n in (b >> 4, b & 15)]
    elif fmt == "IA16":
        values = zip(data[::2], data[1::2])
    else:
        values = data
    for value in values:
        if fmt == "IA16":
            intensity, alpha = value
        elif fmt == "IA8":
            intensity, alpha = (value >> 4) * 17, (value & 15) * 17
        elif fmt == "IA4":
            intensity, alpha = (value >> 1) * 255 // 7, (value & 1) * 255
        elif fmt == "I4":
            intensity = alpha = value * 17
        elif fmt == "I8":
            intensity = alpha = value
        else:
            raise ValueError(f"Unsupported texture format: {fmt}")
        pixels.append((intensity, intensity, intensity, alpha))
    assert len(pixels) == width * height
    image = Image.new("RGBA", (width, height))
    image.putdata(pixels)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=ROOT / "build-cmake/soh/oot.o2r")
    parser.add_argument("--output", type=Path, default=ROOT / "build-cmake/menu-textures")
    args = parser.parse_args()
    previews = []
    count = 0
    with zipfile.ZipFile(args.archive) as archive:
        for group in ("objects/object_mag", "textures/title_static"):
            definitions = yaml.safe_load((ROOT / f"soh/assets/yml/pal_1-0/{group}.yml").read_text())
            for name, spec in definitions.items():
                if spec.get("type") != "TEXTURE":
                    continue
                resource = archive.read(f"{group}/{name}")
                # Torch's 64-byte resource header precedes format, width, height, size.
                _, width, height, size = struct.unpack_from("<4I", resource, 64)
                assert (width, height) == (spec["width"], spec["height"])
                data = resource[80:]
                assert len(data) == size
                image = decode(data, spec["format"], width, height)
                destination = args.output / group / f"{name}.png"
                destination.parent.mkdir(parents=True, exist_ok=True)
                image.save(destination)
                count += 1
                if not any(tag in name for tag in ("FRATex", "GERTex", "JPNTex", "EffectMask")):
                    previews.append((name, image))
    # Checkerboard makes transparency visible without altering exported PNGs.
    columns, cell_width, cell_height = 4, 340, 210
    sheet = Image.new("RGB", (columns * cell_width, ((len(previews) + columns - 1) // columns) * cell_height))
    draw = ImageDraw.Draw(sheet)
    for index, (name, image) in enumerate(previews):
        x, y = index % columns * cell_width, index // columns * cell_height
        for dy in range(0, cell_height, 12):
            for dx in range(0, cell_width, 12):
                color = (55, 55, 55) if (dx // 12 + dy // 12) % 2 else (80, 80, 80)
                draw.rectangle((x + dx, y + dy, x + dx + 11, y + dy + 11), fill=color)
        draw.text((x + 8, y + 8), name, fill="white")
        scale = min(2, (cell_width - 16) / image.width, (cell_height - 36) / image.height)
        preview = image.resize((int(image.width * scale), int(image.height * scale)), Image.Resampling.NEAREST)
        sheet.paste(preview, (x + (cell_width - preview.width) // 2, y + 30), preview)
    sheet.save(args.output / "menu-preview.png")
    print(f"Exported {count} textures to {args.output}")
    print(f"Preview: {args.output / 'menu-preview.png'}")


if __name__ == "__main__":
    main()
