"""Render resources/icons/moyou.svg into the PNGs the app loads and the .ico the Windows executable embeds.

Run it after editing the SVG and commit what it writes:

    pip install resvg-py pillow
    python tools/make_icons.py
"""
import io
import pathlib

import resvg_py
from PIL import Image

ICONS = pathlib.Path(__file__).resolve().parent.parent / "resources" / "icons"
PNG_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]   # window icon (icon.cpp lists the same)
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]        # Explorer and the taskbar


def render(size: int) -> Image.Image:
    png = resvg_py.svg_to_bytes(svg_path=str(ICONS / "moyou.svg"), width=size, height=size)
    return Image.open(io.BytesIO(bytes(png))).convert("RGBA")


def main() -> None:
    images = {s: render(s) for s in sorted(set(PNG_SIZES) | set(ICO_SIZES))}
    for s in PNG_SIZES:
        images[s].save(ICONS / f"moyou-{s}.png", optimize=True)
    # Each size of the .ico is its own rendering, not a downscale of the largest one.
    big = images[max(ICO_SIZES)]
    big.save(ICONS / "moyou.ico", sizes=[(s, s) for s in ICO_SIZES],
             append_images=[images[s] for s in ICO_SIZES if s != max(ICO_SIZES)])
    print(f"wrote {len(PNG_SIZES)} PNGs and moyou.ico to {ICONS}")


if __name__ == "__main__":
    main()
