#!/usr/bin/env python3
"""Render the rotating companion cube (ASCII, no UI chrome) to docs/companion.gif.

Runs the built ascii3D.exe in --snapshot mode at several angles and stitches the
frames into an animated GIF. Requires Pillow and a build at build/ascii3D.exe.

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

FRAMES = 36          # full turn
FONT_SIZE = 13
FG = (40, 230, 110)
BG = (0, 0, 0)


def snapshot(angle):
    r = subprocess.run([EXE, "--snapshot", "-s", "companion", "--angle", str(angle)],
                       capture_output=True, text=True)
    return [ln.rstrip() for ln in r.stdout.splitlines()]


def main():
    frames = [snapshot(i * (360.0 / FRAMES)) for i in range(FRAMES)]
    rows = len(frames[0])
    cols = max(len(ln) for fr in frames for ln in fr)

    # union bounding box of non-space cells (fixed crop, so the cube doesn't jump)
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
        img = Image.new("RGB", img_size, BG)
        d = ImageDraw.Draw(img)
        for r in range(minr, maxr + 1):
            ln = fr[r] if r < len(fr) else ""
            for c in range(minc, maxc + 1):
                ch = ln[c] if c < len(ln) else " "
                if ch != " ":
                    d.text(((c - minc) * cw, (r - minr) * lh), ch, font=font, fill=FG)
        images.append(img)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    images[0].save(OUT, save_all=True, append_images=images[1:],
                   duration=80, loop=0, optimize=True, disposal=2)
    print("wrote %s  (%d frames, %dx%d px, %.0f KB)"
          % (OUT, len(images), img_size[0], img_size[1], os.path.getsize(OUT) / 1024.0))


if __name__ == "__main__":
    main()
