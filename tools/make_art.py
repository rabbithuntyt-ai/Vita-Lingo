#!/usr/bin/env python3
"""Draws Lingo's LiveArea art in the chalkboard style. The Vita wants these as
8-bit palette PNGs. Usage: make_art.py path/to/KleeOne-SemiBold.ttf outdir"""
import math
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

BOARD = (0x1a, 0x2e, 0x25)
BOARD2 = (0x20, 0x38, 0x2d)
CHALK = (0xee, 0xf1, 0xe6)
YELLOW = (0xf1, 0xd3, 0x7c)
GREEN = (0xa6, 0xe3, 0xbd)
PINK = (0xf4, 0xa6, 0xc0)
RED = (0xe8, 0x4a, 0x3c)
WOOD = (0x5a, 0x3e, 0x2b)
WOOD2 = (0x7d, 0x58, 0x3c)


def board(w, h, seed=1):
    random.seed(seed)
    img = Image.new("RGB", (w, h), BOARD)
    d = ImageDraw.Draw(img, "RGBA")
    # soft smudges, like an erased board
    for _ in range(max(3, w * h // 40000)):
        x, y = random.randrange(w), random.randrange(h)
        r = random.randrange(min(w, h) // 6, min(w, h) // 2 + 1)
        d.ellipse([x - r, y - r, x + r, y + r], fill=CHALK + (random.randrange(6, 12),))
    img = img.filter(ImageFilter.GaussianBlur(radius=max(2, min(w, h) // 30)))
    # chalk grain
    d = ImageDraw.Draw(img, "RGBA")
    for _ in range(w * h // 60):
        x, y = random.randrange(w), random.randrange(h)
        d.point((x, y), fill=CHALK + (random.randrange(4, 14),))
    return img


def chalk_text(img, xy, text, font, fill, anchor="la", passes=3):
    d = ImageDraw.Draw(img, "RGBA")
    x, y = xy
    for i in range(passes):
        dx, dy = random.uniform(-0.8, 0.8), random.uniform(-0.8, 0.8)
        d.text((x + dx, y + dy), text, font=font, fill=fill + (150 if i else 235,), anchor=anchor)


def hanamaru(img, cx, cy, R, width):
    d = ImageDraw.Draw(img, "RGBA")
    pts = []
    for i in range(121):
        t = i / 120
        a = -math.pi / 2 + t * 4.2 * math.pi
        r = R * (0.08 + 0.42 * t)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    for i in range(241):
        t = i / 240
        a = -math.pi / 2 + 4.2 * math.pi + t * 2 * math.pi
        r = R * (0.62 + 0.3 * abs(math.sin(t * 5 * math.pi)))
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.line(pts, fill=RED + (230,), width=width, joint="curve")


def gameboy(img, x, y, s):
    d = ImageDraw.Draw(img, "RGBA")
    c = CHALK + (150,)
    d.rounded_rectangle([x, y, x + 150 * s, y + 200 * s], radius=14 * s, outline=c, width=max(2, int(3 * s)))
    d.rounded_rectangle([x + 18 * s, y + 20 * s, x + 132 * s, y + 100 * s], radius=6 * s, outline=CHALK + (120,), width=max(2, int(3 * s)))
    d.line([x + 30 * s, y + 140 * s, x + 60 * s, y + 140 * s], fill=c, width=max(2, int(4 * s)))
    d.line([x + 45 * s, y + 125 * s, x + 45 * s, y + 155 * s], fill=c, width=max(2, int(4 * s)))
    d.ellipse([x + 102 * s, y + 124 * s, x + 118 * s, y + 140 * s], fill=RED + (180,))
    d.ellipse([x + 118 * s, y + 140 * s, x + 134 * s, y + 156 * s], fill=YELLOW + (180,))


def tray(img):
    w, h = img.size
    d = ImageDraw.Draw(img)
    d.rectangle([0, h - 14, w, h], fill=WOOD)
    d.rectangle([0, h - 14, w, h - 11], fill=WOOD2)
    d.rounded_rectangle([w * 0.62, h - 22, w * 0.62 + 46, h - 13], radius=4, fill=CHALK)
    d.rounded_rectangle([w * 0.62 + 56, h - 21, w * 0.62 + 86, h - 13], radius=4, fill=YELLOW)


def save(img, path, colors=128):
    img.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)


def main(font_path, out):
    f = lambda size: ImageFont.truetype(font_path, size)

    # icon 128x128
    icon = board(128, 128, 3)
    d = ImageDraw.Draw(icon)
    d.rounded_rectangle([0, 0, 127, 127], radius=10, outline=WOOD2, width=6)
    random.seed(5)
    chalk_text(icon, (60, 56), "語", f(74), CHALK, anchor="mm")
    hanamaru(icon, 100, 28, 16, 3)
    chalk_text(icon, (64, 113), "LINGO", f(17), YELLOW, anchor="mm", passes=2)
    save(icon, f"{out}/icon0.png")

    # pic0 960x544 and bg 840x500 share a layout
    for name, (w, h), seed in (("pic0.png", (960, 544), 7), ("bg.png", (840, 500), 9)):
        img = board(w, h, seed)
        s = w / 960
        random.seed(seed)
        chalk_text(img, (70 * s, 90 * s), "ことば", f(int(96 * s)), CHALK)
        chalk_text(img, (76 * s, 230 * s), "LINGO", f(int(58 * s)), YELLOW)
        chalk_text(img, (78 * s, 310 * s), "Play in Japanese. Learn every word.", f(int(24 * s)), (0x9d, 0xb2, 0xa4))
        chalk_text(img, (78 * s, 390 * s), "書く  Kaku practice inside", f(int(26 * s)), GREEN)
        gameboy(img, 640 * s, 90 * s, 1.3 * s)
        chalk_text(img, (737 * s, 192 * s), "あ", f(int(56 * s)), GREEN, anchor="mm")
        hanamaru(img, 860 * s, 400 * s, 60 * s, max(3, int(6 * s)))
        tray(img)
        save(img, f"{out}/{name}")

    # startup gate 280x158
    st = board(280, 158, 11)
    random.seed(11)
    chalk_text(st, (24, 22), "ことば", f(40), CHALK)
    chalk_text(st, (26, 82), "LINGO", f(34), YELLOW)
    chalk_text(st, (28, 126), "mGBA + Kaku", f(16), GREEN)
    hanamaru(st, 228, 60, 34, 4)
    save(st, f"{out}/startup.png", 64)
    print("art written to", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
