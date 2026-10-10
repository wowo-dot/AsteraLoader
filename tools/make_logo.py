"""Renders the Astera Loader logo into assets/logo.

The mark: a gold four-point star inside a cream hexagon, on a navy rounded
tile. Writes the SVG master, PNGs, the launcher's icon (AsteraLoader.exe
embeds it), the GitHub social preview and the Nexus cover.

Run with Python 3.10+ and Pillow:  python -B tools/make_logo.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "logo"

NAVY = (19, 35, 58)
GOLD = (242, 181, 68)
CREAM = (244, 235, 217)

# The mark on a 100 x 100 grid.
TILE_RADIUS = 22
HEXAGON = [(50, 13), (82, 31.5), (82, 68.5), (50, 87), (18, 68.5), (18, 31.5)]


def star(long: float, short: float, center: float = 50) -> list[tuple[float, float]]:
    """A four-point star: tips at north, east, south, west."""
    c, s = center, short
    return [(c, c - long), (c + s, c - s), (c + long, c), (c + s, c + s),
            (c, c + long), (c - s, c + s), (c - long, c), (c - s, c - s)]


# Small icons get heavier lines and a bigger star, so they stay readable.
def style(pixels: int) -> tuple[float, float, float]:
    if pixels <= 24:
        return 10, 27, 7.5
    if pixels <= 48:
        return 8, 25, 6.5
    return 6, 23, 6


def mark(pixels: int, tile: bool = True) -> Image.Image:
    """The mark at `pixels` square, drawn large and scaled down smoothly."""
    scale = max(8, 2048 // pixels)
    size = pixels * scale
    unit = size / 100
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    stroke, long, short = style(pixels)
    if tile:
        draw.rounded_rectangle((0, 0, size - 1, size - 1), radius=TILE_RADIUS * unit, fill=NAVY)
    points = [(x * unit, y * unit) for x, y in HEXAGON]
    # Repeat the first two points so every corner gets a round joint.
    draw.line(points + points[:2], fill=CREAM, width=round(stroke * unit), joint="curve")
    draw.polygon([(x * unit, y * unit) for x, y in star(long, short)], fill=GOLD)
    return image.resize((pixels, pixels), Image.LANCZOS)


def svg() -> str:
    stroke, long, short = style(256)
    hexagon = " ".join(f"{x:g},{y:g}" for x, y in HEXAGON)
    points = " ".join(f"{x:g},{y:g}" for x, y in star(long, short))
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="256" height="256">\n'
        "  <title>Astera Loader</title>\n"
        f'  <rect width="100" height="100" rx="{TILE_RADIUS}" fill="#13233A"/>\n'
        f'  <polygon points="{hexagon}" fill="none" stroke="#F4EBD9" stroke-width="{stroke:g}" stroke-linejoin="round"/>\n'
        f'  <polygon points="{points}" fill="#F2B544"/>\n'
        "</svg>\n"
    )


def font(size: int, weight: str) -> ImageFont.FreeTypeFont:
    fonts = Path("C:/Windows/Fonts")
    try:
        face = ImageFont.truetype(str(fonts / "bahnschrift.ttf"), size)
        face.set_variation_by_name(weight)
        return face
    except (OSError, ValueError):
        return ImageFont.truetype(str(fonts / ("segoeuib.ttf" if weight != "Regular" else "segoeui.ttf")), size)


def banner(width: int, height: int) -> Image.Image:
    """The mark and the name on navy: GitHub social preview, Nexus cover."""
    image = Image.new("RGB", (width, height), NAVY)
    # The layout is about 1065 x 300 at unit 1: keep a margin on every side.
    unit = min(width / 1380, height / 640)
    icon = mark(round(300 * unit), tile=False)
    title = font(round(118 * unit), "SemiBold")
    tagline = font(round(40 * unit), "Regular")
    note = font(round(30 * unit), "Regular")
    draw = ImageDraw.Draw(image)
    lines = [("Astera Loader", title, CREAM), ("Plugin loader for Monster Hunter: World", tagline, GOLD),
             ("Open source  \u00b7  Runs Stracker plugins  \u00b7  MIT", note, CREAM)]
    gaps = [0, round(26 * unit), round(30 * unit)]
    boxes = [draw.textbbox((0, 0), text, font=face) for text, face, _ in lines]
    text_width = max(box[2] - box[0] for box in boxes)
    text_height = sum(box[3] - box[1] for box in boxes) + sum(gaps)
    spacing = round(56 * unit)
    left = (width - (icon.width + spacing + text_width)) // 2
    image.paste(icon, (left, (height - icon.height) // 2), icon)
    x, y = left + icon.width + spacing, (height - text_height) // 2
    for (text, face, colour), box, gap in zip(lines, boxes, gaps):
        y += gap
        draw.text((x - box[0], y - box[1]), text, font=face, fill=colour)
        y += box[3] - box[1]
    return image


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "astera-loader.svg").write_text(svg(), encoding="utf-8")
    for pixels in (512, 256, 128, 64):
        mark(pixels).save(OUT / f"astera-loader-{pixels}.png")
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    icons = [mark(pixels) for pixels in sizes]
    icons[-1].save(OUT / "astera-loader.ico", sizes=[(p, p) for p in sizes], append_images=icons[:-1])
    banner(1280, 640).save(OUT / "social-preview-1280x640.png")
    banner(1920, 1080).save(OUT / "nexus-cover-1920x1080.png")
    with Image.open(OUT / "astera-loader.ico") as ico:
        print("icon sizes:", sorted(ico.info.get("sizes", ico.ico.sizes())))
    for path in sorted(OUT.iterdir()):
        print(f"  {path.relative_to(ROOT).as_posix()}  {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
