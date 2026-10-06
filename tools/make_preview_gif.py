#!/usr/bin/env python3
"""Render the rotating companion cube (ASCII, no UI chrome) to docs/companion.gif.

Runs the built ascii3D.exe in --snapshot mode over exactly one full turn and
stitches the frames into a seamlessly looping animated GIF with a transparent
background. Requires Pillow and a build at build/ascii3D.exe.

Usage:  python tools/make_preview_gif.py
"""
import os
import subprocess

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "ascii3D.exe")
OUT = os.path.join(ROOT, "docs", "companion.gif")
FONT = r"C:\Windows\Fonts\consola.ttf"

FRAMES = 36                 # exactly one full turn: 360 / 36 = 10 deg per frame
STEP = 360.0 / FRAMES       # frame N is at N*STEP; frame 0 == frame FRAMES (not stored)
DURATION = 80               # ms per frame
FONT_SIZE = 13
FG = (46, 160, 67)          # green foreground (readable on light and dark)


def snapshot(angle):
    r = subprocess.run([EXE, "--snapshot", "-s", "companion", "--angle", str(angle)],
                       capture_output=True, text=True)
    return [ln.rstrip() for ln in r.stdout.splitlines()]


def to_transparent_p(img_rgba):
    """Convert an RGBA frame to a palettised GIF frame with 1-bit transparency."""
    r, g, b, a = img_rgba.split()
    opaque = a.point(lambda v: 255 if v >= 128 else 0)      # 1-bit alpha
    rgb = Image.new("RGB", img_rgba.size, FG)               # background uses FG colour
    rgb.paste(Image.merge("RGB", (r, g, b)), mask=opaque)   # glyphs where opaque
    pal = rgb.convert("P", palette=Image.ADAPTIVE, colors=255)
    pal.paste(255, opaque.point(lambda v: 255 if v == 0 else 0))  # index 255 = transparent
    return pal


def main():
    frames = [snapshot(i * STEP) for i in range(FRAMES)]
    rows = len(frames[0])
    cols = max(len(ln) for fr in frames for ln in fr)

    # fixed crop = union bounding box of non-space cells, so the cube doesn't jump
    minr, maxr, minc, maxc = rows, -1, cols, -1
    for fr in frames:
        for r, ln in enumerate(fr):
            for c, ch in enumerate(ln):
                if ch != " ":
                    minr = min(minr, r); maxr = max(maxr, r)
                    minc = min(minc, c); maxc = max(maxc, c)
    minr = max(0, minr - 1); maxr = min(rows - 1, maxr + 1)
    minc = max(0, minc - 1); maxc = min(cols - 1, maxc + 1)
    nr, nc = maxr - minr + 1, maxc - minc + 1

    font = ImageFont.truetype(FONT, FONT_SIZE)
    cw = int(round(font.getlength("M")))
    asc, desc = font.getmetrics()
    lh = asc + desc
    img_size = (nc * cw, nr * lh)

    images = []
    for fr in frames:
        img = Image.new("RGBA", img_size, (0, 0, 0, 0))     # transparent background
        d = ImageDraw.Draw(img)
        for r in range(minr, maxr + 1):
            ln = fr[r] if r < len(fr) else ""
            for c in range(minc, maxc + 1):
                ch = ln[c] if c < len(ln) else " "
                if ch != " ":
                    d.text(((c - minc) * cw, (r - minr) * lh), ch, font=font, fill=FG + (255,))
        images.append(to_transparent_p(img))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    images[0].save(OUT, save_all=True, append_images=images[1:],
                   duration=DURATION, loop=0, transparency=255,
                   disposal=2, optimize=False)
    print("wrote %s  (%d frames, %dx%d px, %.0f KB, transparent)"
          % (OUT, len(images), img_size[0], img_size[1], os.path.getsize(OUT) / 1024.0))


if __name__ == "__main__":
    main()
