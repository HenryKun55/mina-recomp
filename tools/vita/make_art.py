#!/usr/bin/env python3
"""
Builds the PS Vita bubble art from your own copy of the game:

    python3 tools/vita/make_art.py TITLE_FRAME [ICNS]

TITLE_FRAME is a full-size frame of the title screen while the logo shows
without the menu, dumped by the Mac build:

    MINA_DUMP=/tmp/t%d.ppm MINA_DUMP_FULL=1 MINA_DUMP_EVERY=400 \\
        MINA_INPUT="6:Return 7:Return" ./build/mina

ICNS defaults to the bundle's MinaTheHollower.icns. It writes, under
deps/art (git-ignored: the art is Yacht Club Games', never committed):

    sce_sys/icon0.png                       128x128  bubble icon (the game's icon on the title's sky)
    sce_sys/pic0.png                        960x544  shown from the tap until the game draws
    sce_sys/livearea/contents/bg.png        840x500  LiveArea background
    sce_sys/livearea/contents/startup.png   280x158  launch gate (the logo)
    sce_sys/livearea/contents/template.xml

The LiveArea wants 8-bit (palette) PNGs. CMake packs deps/art into the VPK
when it exists. Requires Pillow.
"""

import os
import sys

from PIL import Image, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "deps", "art", "sce_sys")
ICNS = os.path.join(ROOT, "gamedata", "app", "Contents", "Resources", "MinaTheHollower.icns")

# a 16:9 window around the logo on the title screen (fractions of the image)
LOGO = (0.18, 0.0, 0.82, 0.642)

TEMPLATE = """<?xml version="1.0" encoding="utf-8" standalone="yes"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
	<livearea-background>
		<image>bg.png</image>
	</livearea-background>
	<gate>
		<startup-image>
			<image>startup.png</image>
		</startup-image>
	</gate>
</livearea>
"""


def palette_png(img, path):
    """The LiveArea only takes 8-bit PNGs: quantized without dithering (pixel art)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT), img.size)


def cover(img, w, h):
    """Scaled to fill w x h, centered, cropping what overflows."""
    s = max(w / img.width, h / img.height)
    r = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    x, y = (r.width - w) // 2, (r.height - h) // 2
    return r.crop((x, y, x + w, y + h))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    frame = Image.open(sys.argv[1]).convert("RGB")
    title = frame.crop(frame.getbbox())             # without the window's black bars
    icns = sys.argv[2] if len(sys.argv) > 2 else ICNS
    icon = Image.open(icns).convert("RGBA")

    # pic0: the whole title screen, 16:9 in 960x544
    pic0 = Image.new("RGB", (960, 544), "black")
    pic0.paste(title.resize((960, 540), Image.LANCZOS), (0, 2))
    palette_png(pic0, os.path.join(OUT, "pic0.png"))

    # bg: the title art, a little darker at the bottom where the LiveArea draws its buttons
    bg = cover(title, 840, 500)
    shade = Image.linear_gradient("L").resize((840, 500))
    dark = ImageEnhance.Brightness(bg).enhance(0.55)
    bg = Image.composite(dark, bg, shade.point(lambda v: max(0, v - 128) * 2))
    palette_png(bg, os.path.join(OUT, "livearea", "contents", "bg.png"))

    # startup: the logo
    lx0, ly0, lx1, ly1 = (round(f * d) for f, d in zip(LOGO, (title.width, title.height) * 2))
    palette_png(cover(title.crop((lx0, ly0, lx1, ly1)), 280, 158),
                os.path.join(OUT, "livearea", "contents", "startup.png"))

    # icon0: the game's pixel-art Mina (nearest neighbour) on the title's night colours
    top, bottom = (14, 16, 46), (52, 18, 52)
    grad = Image.linear_gradient("L").resize((128, 128))
    sky = Image.composite(Image.new("RGB", (128, 128), bottom), Image.new("RGB", (128, 128), top), grad)
    sky = sky.convert("RGBA")
    sky.alpha_composite(icon.resize((112, 112), Image.NEAREST), (8, 10))
    palette_png(sky, os.path.join(OUT, "icon0.png"))

    with open(os.path.join(OUT, "livearea", "contents", "template.xml"), "w") as f:
        f.write(TEMPLATE)
    print("wrote", os.path.relpath(os.path.join(OUT, "livearea", "contents", "template.xml"), ROOT))


if __name__ == "__main__":
    main()
