#!/usr/bin/env python3
"""Build an optional 4x English file-menu mod. Requires Pillow and PyYAML."""
import argparse
import math
from pathlib import Path
import struct
import zipfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont
import yaml

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
GROUP = "textures/title_static"
BUTTONS = {
    "File1": "File 1", "File2": "File 2", "File3": "File 3", "Copy": "Copy",
    "Erase": "Erase", "Options": "Options", "Quit": "Quit", "Yes": "Yes", "END": "END",
}
LABELS = {
    "AreYouSure2": "Are you sure?", "AreYouSure": "Are you sure?",
    "CheckBrightness": "CHECK BRIGHTNESS", "Controls": "A-Decide • B-Cancel",
    "CopyToWhichFile": "Copy to which file?", "CopyWhichFile": "Copy which file?",
    "EraseWhichFile": "Erase which file?", "FileCopied": "File copied.",
    "FileEmpty": "This is an empty file.", "FileErased": "File erased.",
    "FileInUse": "This file is in use.", "Headset": "Headset", "Hold": "Hold",
    "LTargeting": "Z TARGETING", "LangEnglish": "English", "LangEnglishLarge": "ENGLISH",
    "Language": "LANGUAGE", "Mono": "Mono", "Name": "Name?",
    "NoEmptyFile": "There is no empty file.", "NoFileToCopy": "No file to copy.",
    "NoFileToErase": "No file to erase.", "OpenThisFile": "Open this file?",
    "Options": "Options", "PleaseSelectAFile": "Please select a file.", "SOUND": "SOUND",
    "SelectYourLanguage": "• Select your Language", "Stereo": "Stereo",
    "Surround": "Surround", "Switch": "Switch",
}
BYTES_PER_PIXEL = {1: 4, 2: 2, 5: 0.5, 6: 1, 7: 0.5, 8: 1, 9: 2}


def redraw_button(label, font_path, size=(64, 16)):
    s = 8
    width, height = size
    image = Image.new("RGBA", (width * s, height * s))
    mask = Image.new("L", image.size)
    points = [(0, 5), (5, 0), (width - 3, 0), (width, 3),
              (width, height - 3), (width - 3, height), (5, height), (0, height - 5)]
    points = [(x * s, y * s) for x, y in points]
    ImageDraw.Draw(mask).polygon(points, fill=255)
    draw = ImageDraw.Draw(image)
    for y in range(image.height):
        gray = round(205 - 75 * y / (image.height - 1))
        draw.line((0, y, image.width, y), fill=(gray, gray, gray, 255))
    image.putalpha(mask)
    draw = ImageDraw.Draw(image)
    draw.line(points[:4], fill=(245, 245, 245, 255), width=s)
    draw.line(points[3:] + [points[0]], fill=(65, 65, 65, 255), width=s)
    draw.line([(s, 5 * s), (5 * s, s), ((width - 4) * s, s)], fill=(110, 110, 110, 255), width=s // 2)
    font = ImageFont.truetype(str(font_path), 12 * s)
    bbox = draw.textbbox((0, 0), label, font=font)
    x = (image.width - (bbox[2] - bbox[0])) // 2 - bbox[0]
    y = (image.height - (bbox[3] - bbox[1])) // 2 - bbox[1]
    draw.text((x + s, y + s), label, font=font, fill=(240, 240, 240, 255))
    draw.text((x, y), label, font=font, fill=(0, 0, 0, 255))
    return image.resize((width * SCALE, height * SCALE), Image.Resampling.LANCZOS)


def redraw_label(label, original, font_path):
    # Keep the original occupied rectangle: game geometry and alignment are unchanged.
    bbox = original.getchannel("A").getbbox()
    if bbox is None:
        raise ValueError("Empty label texture")
    s = 8
    font = ImageFont.truetype(str(font_path), 12 * s)
    stroke = 6
    scratch = Image.new("RGBA", (2000, 200))
    draw = ImageDraw.Draw(scratch)
    draw.text((stroke, 0), label, font=font, fill="white", stroke_width=stroke, stroke_fill="black")
    cropped = scratch.crop(scratch.getchannel("A").getbbox())
    text = cropped.resize(((bbox[2] - bbox[0]) * SCALE, (bbox[3] - bbox[1]) * SCALE), Image.Resampling.LANCZOS)
    image = Image.new("RGBA", (original.width * SCALE, original.height * SCALE))
    image.paste(text, (bbox[0] * SCALE, bbox[1] * SCALE))
    return image


def redraw_panel(width, height, outer=True):
    # Rebuild continuous shading and borders before splitting the window into its tiles.
    w, h = width * SCALE, height * SCALE
    image = Image.new("RGBA", (w, h))
    pixels = []
    for y in range(h):
        for x in range(w):
            px, py = x / SCALE, y / SCALE
            sheen = math.exp(-((math.sin((px + 0.65 * py - 28) / 30)) / 0.6) ** 2)
            gray = round(80 + 165 * sheen)
            fade = max(0, min(1, (width - px) / 56)) if outer else 1
            pixels.append((gray, gray, gray, round(255 * fade)))
    image.putdata(pixels)
    draw = ImageDraw.Draw(image)
    if outer:
        mask = Image.new("L", (w, h))
        ImageDraw.Draw(mask).polygon([(0, 8 * SCALE), (8 * SCALE, 0), (w, 0),
                                     (w, h), (8 * SCALE, h), (0, h - 8 * SCALE)], fill=255)
        # Preserve the alpha fade at the right edge while making the left corners clean.
        from PIL import ImageChops
        image.putalpha(ImageChops.multiply(image.getchannel("A"), mask))
        draw = ImageDraw.Draw(image)
        draw.line([(0, 8 * SCALE), (8 * SCALE, 0), (w, 0)], fill=(50, 50, 50, 255), width=2 * SCALE)
        draw.line([(SCALE, 9 * SCALE), (SCALE, h - 9 * SCALE), (8 * SCALE, h - SCALE)],
                  fill=(220, 220, 220, 255), width=SCALE)
        # The inset border spans multiple tiles, so draw it in the assembled image.
        draw.line([(14 * SCALE, (height - 14) * SCALE), (14 * SCALE, 29 * SCALE), (w, 29 * SCALE)],
                  fill=(30, 30, 30, 255), width=SCALE)
        draw.line([(15 * SCALE, (height - 14) * SCALE), (15 * SCALE, 30 * SCALE), (w, 30 * SCALE)],
                  fill=(230, 230, 230, 255), width=SCALE)
        # Borders must fade with the panel, too.
        alpha = image.getchannel("A")
        for x in range((width - 56) * SCALE, w):
            factor = max(0, min(1, (width - x / SCALE) / 56))
            alpha.paste(round(255 * factor), (x, 0, x + 1, h))
        image.putalpha(alpha)
    else:
        draw.rounded_rectangle((SCALE, SCALE, w - SCALE - 1, h - SCALE - 1),
                               radius=2 * SCALE, outline=(235, 235, 235, 255), width=SCALE)
        draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=3 * SCALE,
                               outline=(45, 45, 45, 255), width=SCALE)
    return image


def redraw_highlight(size):
    w, h = (v * SCALE for v in size)
    mask = Image.new("L", (w, h))
    inset = 4 * SCALE
    ImageDraw.Draw(mask).rounded_rectangle((inset, inset, w - inset - 1, h - inset - 1),
                                          radius=4 * SCALE, outline=255, width=SCALE)
    glow = mask.filter(ImageFilter.GaussianBlur(1.5 * SCALE))
    from PIL import ImageChops
    alpha = ImageChops.lighter(mask, glow)
    # Intensity textures originally carry their intensity in both colour and alpha.
    return Image.merge("RGBA", (alpha, alpha, alpha, alpha))


def pack_texture(original, image):
    original_type, width, height, _ = struct.unpack_from("<4I", original, 64)
    assert image.size == (width * SCALE, height * SCALE)
    header = bytearray(original[:64])
    header[1] = 1
    struct.pack_into("<I", header, 8, 1)
    data = image.tobytes()
    metadata = struct.pack("<4I2fI", 1, image.width, image.height, 1,
                           SCALE * 4 / BYTES_PER_PIXEL[original_type], float(SCALE), len(data))
    return bytes(header) + metadata + data


def comparison(images, destination):
    cell_w, cell_h = 700, 145
    sheet = Image.new("RGB", (cell_w * 2, len(images) * cell_h), (45, 45, 45))
    draw = ImageDraw.Draw(sheet)
    for row, (name, original, replacement) in enumerate(images):
        y = row * cell_h
        draw.text((10, y + 8), name + " — original", fill="white")
        draw.text((cell_w + 10, y + 8), "4x replacement", fill="white")
        for x, image in [(10, original.resize(replacement.size, Image.Resampling.NEAREST)), (cell_w + 10, replacement)]:
            sheet.paste(image, (x, y + 35), image)
    sheet.save(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=Path("/usr/share/fonts/liberation/LiberationSans-Bold.ttf"))
    parser.add_argument("--output", type=Path, default=ROOT / "build-cmake/menu-upscale-sample")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    originals = {}
    replacements = {}
    def original(path):
        if path not in originals:
            originals[path] = Image.open(ROOT / f"build-cmake/menu-textures/{path}.png").convert("RGBA")
        return originals[path]
    for key, label in BUTTONS.items():
        path = f"{GROUP}/gFileSel{key}ButtonENGTex"
        replacements[path] = redraw_button(label, args.font, original(path).size)
    for key, label in LABELS.items():
        path = f"{GROUP}/gFileSel{key}ENGTex"
        replacements[path] = redraw_label(label, original(path), args.font)
    panel = redraw_panel(248, 160)
    for i in range(20):
        path = f"{GROUP}/gFileSelWindow{i + 1}Tex"
        w, h = original(path).size
        x, y = (i % 4) * 64 * SCALE, (i // 4) * 32 * SCALE
        replacements[path] = panel.crop((x, y, x + w * SCALE, y + h * SCALE))
    info = redraw_panel(168, 56, outer=False)
    for i in range(5):
        path = f"{GROUP}/gFileSelFileInfoBox{i + 1}Tex"
        w, h = original(path).size
        x = i * 36 * SCALE
        replacements[path] = info.crop((x, 0, x + w * SCALE, h * SCALE))
    for key in ["BigButton", "MediumButton", "SmallButton", "Char"]:
        path = f"{GROUP}/gFileSel{key}HighlightTex"
        replacements[path] = redraw_highlight(original(path).size)
    path = f"{GROUP}/gFileSelNameBoxTex"
    replacements[path] = redraw_panel(*original(path).size, outer=False)
    path = f"{GROUP}/gFileSelOptionsDividerTex"
    divider = Image.new("RGBA", (256 * SCALE, 2 * SCALE))
    ImageDraw.Draw(divider).line((0, SCALE, divider.width, SCALE), fill="white", width=SCALE)
    replacements[path] = divider
    # Preserve the small shared arrow and connector designs, but smooth their sampling.
    for key in ["BackspaceButton", "Connector", "UnkOval"]:
        path = f"{GROUP}/gFileSel{key}Tex"
        im = original(path)
        replacements[path] = im.resize((im.width * SCALE, im.height * SCALE), Image.Resampling.LANCZOS)
    path = "objects/object_mag/gTitleZeldaShieldLogoTex"
    replacements[path] = original(path).resize((640, 640), Image.Resampling.LANCZOS)
    mod = ROOT / "build-cmake/soh/mods/menu-upscale-sample.o2r"
    mod.parent.mkdir(parents=True, exist_ok=True)
    # Replace atomically so a failed export never truncates the installed mod.
    temporary = mod.with_suffix(".tmp")
    with zipfile.ZipFile(ROOT / "build-cmake/soh/oot.o2r") as source, zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as target:
        for path, image in replacements.items():
            png_path = args.output / f"{path}.png"
            png_path.parent.mkdir(parents=True, exist_ok=True)
            image.save(png_path)
            target.writestr("alt/" + path, pack_texture(source.read(path), image))
    with zipfile.ZipFile(temporary) as archive:
        assert archive.testzip() is None
        assert len(archive.namelist()) == len(replacements)
        for name in archive.namelist():
            blob = archive.read(name)
            _, w, h, flags, hs, vs, size = struct.unpack_from("<4I2fI", blob, 64)
            assert flags == 1 and vs == SCALE and hs in (4, 8, 16, 32)
            assert size == w * h * 4 == len(blob) - 92
    temporary.replace(mod)
    examples = [f"{GROUP}/gFileSel{key}ENGTex" for key in
                ["PleaseSelectAFile", "Controls", "CopyWhichFile", "FileEmpty", "Options", "Language", "SOUND"]]
    comparison([(p.rsplit("/", 1)[1], original(p), replacements[p]) for p in examples], args.output / "labels-comparison.png")
    button_paths = [f"{GROUP}/gFileSel{key}ButtonENGTex" for key in BUTTONS]
    comparison([(p.rsplit("/", 1)[1], original(p), replacements[p]) for p in button_paths], args.output / "buttons-comparison.png")
    panel.save(args.output / "panel-redrawn.png")
    print(f"Created and validated {len(replacements)} replacement textures: {args.output}")
    print(f"Installed optional mod: {mod}")
    print("Restart the game, then press Tab to toggle alternate assets.")


if __name__ == "__main__":
    main()
