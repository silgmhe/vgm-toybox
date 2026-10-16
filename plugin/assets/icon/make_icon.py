#!/usr/bin/env python3
"""
Renders the VGMToybox application icon.

This script is the single source of truth for the artwork. It draws the icon with Pillow at every
size Windows asks for, and **simplifies it as the canvas shrinks** - the chip's pins go first,
then one of the level bars - because a 16 px icon cannot show what a 256 px one can. The results
are packed into one multi-image .ico (DIB entries up to 128 px plus a PNG at 256 px, the
combination rc.exe and every modern shell accept), and the large variant is written out as an SVG
so the same drawing stays resolvable without a rasteriser.

The design follows the application's own language (docs/ui-button-design.md): the accent colour on
a white "material", soft rather than glossy, no detail that only survives at one size.

    python make_icon.py

Outputs, next to this script:
    vgmtoybox-1024.png  ICON_BIG - JUCE's juceaide turns this into the .ico the exe carries
    vgmtoybox-16.png    ICON_SMALL - the simplified variant, which is what a 16 px entry wants
    VGMToybox.ico      16/24/32/48/64/128/256 - the hand-tuned set, kept for packaging
    vgmtoybox.svg      the same design as vectors
    vgmtoybox-256.png  for the README and other documentation
"""

from __future__ import annotations

import pathlib
import struct
from io import BytesIO

from PIL import Image, ImageDraw, ImageFilter

HERE = pathlib.Path(__file__).resolve().parent

SIZES = (16, 24, 32, 48, 64, 128, 256)

# The drawing is authored on a 1024 unit canvas and scaled to the target (times SUPERSAMPLE, then
# downsampled - that is the anti-aliasing).
CANVAS = 1024
SUPERSAMPLE = 4

# theme::accent() and a slightly deepened pair for the tile gradient
ACCENT = (10, 132, 255)
TILE_TOP = (77, 170, 255)
TILE_BOTTOM = (8, 103, 214)
CHIP = (255, 255, 255)
PIN = (255, 255, 255, 236)

# macOS-style rounded square: the corner radius is 22.37 % of the side
CORNER = 0.2237


def variant(size: int) -> dict:
    """What this size can carry. Everything is a fraction of the tile or the chip.

    The artwork is full bleed at every size on purpose. Windows draws an application icon edge to
    edge - window title bar, taskbar, Alt-Tab - so a margin with a macOS-style drop shadow around
    the tile only costs pixels, and a 16 px rendering of it turns the margin into a pale ring
    instead of an icon. The tile's own corner rounding is what says "app icon" here.
    """
    if size <= 20:
        # 16 px: no pins, three fat bars
        return dict(margin=0.0, shadow=False, pins=0, body=(0.58, 0.48),
                    bars=(0.50, 0.88, 0.66))
    if size <= 40:
        # 24 and 32 px: the chip keeps two pins and three bars
        return dict(margin=0.0, shadow=False, pins=2, body=(0.54, 0.45),
                    bars=(0.45, 0.80, 0.60))
    # 48 px and up: the whole design
    return dict(margin=0.0, shadow=False, pins=3, body=(0.50, 0.42),
                bars=(0.42, 0.78, 0.56, 0.92))


def geometry(shape: dict) -> dict:
    """The design in canvas units, from the variant."""
    margin = CANVAS * shape["margin"]
    side = CANVAS - 2 * margin
    body_w, body_h = side * shape["body"][0], side * shape["body"][1]
    body_x = margin + (side - body_w) / 2
    body_y = margin + (side - body_h) / 2
    return dict(margin=margin, side=side, radius=side * CORNER,
                body=(body_x, body_y, body_w, body_h),
                pin_length=side * 0.055, pin_thickness=side * 0.070,
                bar_inset=body_w * 0.13, bar_gap=0.085)


def render(size: int) -> Image.Image:
    """Draws the icon for one target size and down-samples it to exactly that size."""
    shape = variant(size)
    g = geometry(shape)

    scale = size * SUPERSAMPLE / CANVAS
    canvas = size * SUPERSAMPLE

    def px(value: float) -> float:
        return value * scale

    image = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))

    # --- the tile: a rounded square with a soft shadow in front of it -------------------------
    tile = [px(g["margin"]), px(g["margin"]),
            px(g["margin"] + g["side"]), px(g["margin"] + g["side"])]

    mask = Image.new("L", (canvas, canvas), 0)
    ImageDraw.Draw(mask).rounded_rectangle(tile, radius=px(g["radius"]), fill=255)

    if shape["shadow"]:
        blurred = mask.filter(ImageFilter.GaussianBlur(px(g["side"]) * 0.035))
        image.paste((6, 30, 66), (0, int(px(g["side"]) * 0.025)),
                    blurred.point(lambda value: int(value * 0.34)))

    strip = Image.new("RGB", (1, canvas))
    pixels = strip.load()
    for y in range(canvas):
        t = y / max(1, canvas - 1)
        pixels[0, y] = tuple(round(TILE_TOP[i] + (TILE_BOTTOM[i] - TILE_TOP[i]) * t) for i in range(3))
    image.paste(strip.resize((canvas, canvas)), (0, 0), mask)

    # --- the chip: pins, then the body over their middle, then the level bars ------------------
    layer = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    body_x, body_y, body_w, body_h = g["body"]
    pins = shape["pins"]
    if pins:
        # one bar per pin row, drawn right across the chip: the body covers its middle, so only
        # the two tips stick out
        for i in range(pins):
            centre = body_y + body_h * (i + 1) / (pins + 1)
            half = px(g["pin_thickness"]) / 2
            draw.rounded_rectangle(
                [px(body_x - g["pin_length"]), px(centre) - half,
                 px(body_x + body_w + g["pin_length"]), px(centre) + half],
                radius=half, fill=PIN)

    draw.rounded_rectangle([px(body_x), px(body_y), px(body_x + body_w), px(body_y + body_h)],
                           radius=px(body_h * 0.16), fill=CHIP)

    bars = shape["bars"]
    inset = g["bar_inset"]
    inner_x, inner_y = body_x + inset, body_y + inset
    inner_w, inner_h = body_w - 2 * inset, body_h - 2 * inset
    gap = inner_w * g["bar_gap"]
    bar_w = (inner_w - gap * (len(bars) - 1)) / len(bars)
    for i, fraction in enumerate(bars):
        x = inner_x + i * (bar_w + gap)
        top = inner_y + inner_h * (1.0 - fraction)
        draw.rounded_rectangle([px(x), px(top), px(x + bar_w), px(inner_y + inner_h)],
                               radius=px(bar_w) / 2, corners=(True, True, False, False),
                               fill=ACCENT)

    return Image.alpha_composite(image, layer).resize((size, size), Image.LANCZOS)


# --- the .ico container -------------------------------------------------------------------------

def dib_entry(image: Image.Image) -> bytes:
    """One 32bpp bottom-up BMP entry: the oldest form, and the one rc.exe is happiest with."""
    width, height = image.size
    pixels = image.convert("RGBA").load()

    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0,
                         width * height * 4, 0, 0, 0, 0)

    colour = bytearray()
    for y in range(height - 1, -1, -1):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            colour += bytes((b, g, r, a))

    stride = ((width + 31) // 32) * 4
    mask = bytearray()
    for y in range(height - 1, -1, -1):
        row = bytearray(stride)
        for x in range(width):
            if pixels[x, y][3] < 128:              # 1 = transparent in the AND mask
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row

    return header + bytes(colour) + bytes(mask)


def png_bytes(image: Image.Image) -> bytes:
    buffer = BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def write_ico(path: pathlib.Path, images: list[Image.Image]) -> None:
    entries, payloads, offset = [], [], 6 + 16 * len(images)
    for image in images:
        payload = png_bytes(image) if image.width >= 256 else dib_entry(image)
        entries.append(struct.pack("<BBBBHHII",
                                   0 if image.width >= 256 else image.width,
                                   0 if image.height >= 256 else image.height,
                                   0, 0, 1, 32, len(payload), offset))
        payloads.append(payload)
        offset += len(payload)

    with path.open("wb") as handle:
        handle.write(struct.pack("<HHH", 0, 1, len(images)))
        for entry in entries:
            handle.write(entry)
        for payload in payloads:
            handle.write(payload)


# --- the vector twin ----------------------------------------------------------------------------

def svg(shape: dict | None = None) -> str:
    """The large variant as SVG, from the same numbers the rasteriser uses."""
    shape = shape or variant(256)
    g = geometry(shape)
    x, y, w, h = g["body"]
    inset = g["bar_inset"]
    inner_x, inner_y = x + inset, y + inset
    inner_w, inner_h = w - 2 * inset, h - 2 * inset
    bars = shape["bars"]
    gap = inner_w * g["bar_gap"]
    bar_w = (inner_w - gap * (len(bars) - 1)) / len(bars)

    pins = "".join(
        f'\n    <rect x="{x - g["pin_length"]:.1f}" y="{y + h * (i + 1) / (shape["pins"] + 1) - g["pin_thickness"] / 2:.1f}" '
        f'width="{w + 2 * g["pin_length"]:.1f}" height="{g["pin_thickness"]:.1f}" '
        f'rx="{g["pin_thickness"] / 2:.1f}" fill="#ffffff" fill-opacity="0.925"/>'
        for i in range(shape["pins"]))

    level = "".join(
        f'\n    <rect x="{inner_x + i * (bar_w + gap):.1f}" y="{inner_y + inner_h * (1 - f):.1f}" '
        f'width="{bar_w:.1f}" height="{inner_h * f:.1f}" rx="{bar_w / 2:.1f}" fill="#0a84ff"/>'
        for i, f in enumerate(bars))

    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {CANVAS} {CANVAS}" width="1024" height="1024">
  <defs>
    <linearGradient id="tile" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#{TILE_TOP[0]:02x}{TILE_TOP[1]:02x}{TILE_TOP[2]:02x}"/>
      <stop offset="1" stop-color="#{TILE_BOTTOM[0]:02x}{TILE_BOTTOM[1]:02x}{TILE_BOTTOM[2]:02x}"/>
    </linearGradient>
  </defs>

  <rect x="{g["margin"]:.1f}" y="{g["margin"]:.1f}" width="{g["side"]:.1f}" height="{g["side"]:.1f}"
        rx="{g["radius"]:.1f}" fill="url(#tile)"/>{pins}

  <rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{h * 0.16:.1f}" fill="#ffffff"/>{level}
</svg>
'''


def main() -> None:
    images = [render(size) for size in SIZES]
    write_ico(HERE / "VGMToybox.ico", images)
    (HERE / "vgmtoybox.svg").write_text(svg(), encoding="utf-8")

    # The two images JUCE consumes. juceaide generates the platform icon from them, and it loads
    # ordinary images only - an .ico input makes the configure step fail - so the big one is a
    # PNG. The small one carries the simplified artwork, which is what makes the 16 px entry
    # legible.
    images[-1].save(HERE / "vgmtoybox-256.png", optimize=True)
    render(1024).save(HERE / "vgmtoybox-1024.png", optimize=True)
    render(16).save(HERE / "vgmtoybox-16.png", optimize=True)

    print(f"wrote {HERE / 'VGMToybox.ico'} ({', '.join(str(s) for s in SIZES)})")
    print(f"wrote {HERE / 'vgmtoybox-1024.png'}, {HERE / 'vgmtoybox-16.png'}, "
          f"{HERE / 'vgmtoybox-256.png'} and {HERE / 'vgmtoybox.svg'}")


if __name__ == "__main__":
    main()
