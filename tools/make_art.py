#!/usr/bin/env python3
"""Draws Lingo's LiveArea art in the chalkboard style: generic chalk doodles of
retro game hardware (no real console's design) around a CRT showing a pixel
text box, its words underlined and explained the way the study panel does.
The Vita wants these as 8-bit palette PNGs.

Usage: make_art.py KleeOne-SemiBold.ttf DotGothic16-Regular.ttf outdir"""
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
BLUE = (0x9c, 0xc8, 0xf0)
KLEE = DOT = None
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


def chalk_line(d, pts, fill, width):
    """A slightly wobbly chalk stroke through pts."""
    out = []
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        n = max(2, int(math.hypot(x1 - x0, y1 - y0) / 6))
        for i in range(n):
            t = i / n
            out.append((x0 + (x1 - x0) * t + random.uniform(-0.6, 0.6), y0 + (y1 - y0) * t + random.uniform(-0.6, 0.6)))
    out.append(pts[-1])
    d.line(out, fill=fill, width=width, joint="curve")


def chalk_box(d, x0, y0, x1, y1, r, fill, width):
    """A rounded rectangle in chalk."""
    pts = []
    for cx, cy, a0 in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for k in range(7):
            a = math.radians(a0 + k * 15)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    pts.append(pts[0])
    chalk_line(d, pts, fill, width)


def cartridge(img, x, y, s, label):
    """A generic game cartridge: notched top, grip ridges, a label."""
    d = ImageDraw.Draw(img, "RGBA")
    c = CHALK + (170,)
    w = max(2, int(3 * s))
    chalk_line(d, [(x, y + 14 * s), (x + 14 * s, y), (x + 96 * s, y), (x + 96 * s, y + 120 * s), (x, y + 120 * s),
                   (x, y + 14 * s)], c, w)
    for k in range(4):
        yy = y + (12 + k * 7) * s
        chalk_line(d, [(x + 24 * s, yy), (x + 72 * s, yy)], CHALK + (90,), max(1, int(2 * s)))
    chalk_box(d, x + 14 * s, y + 44 * s, x + 82 * s, y + 104 * s, 4 * s, YELLOW + (190,), w)
    font = ImageFont.truetype(KLEE, max(10, int(36 * s)))
    chalk_text(img, (x + 48 * s, y + 75 * s), label, font, CHALK, anchor="mm", passes=2)


def disc(img, cx, cy, r, s):
    """A CD with a chalk shine."""
    d = ImageDraw.Draw(img, "RGBA")
    w = max(2, int(3 * s))
    pts = [(cx + r * math.cos(t / 40 * 2 * math.pi), cy + r * math.sin(t / 40 * 2 * math.pi)) for t in range(41)]
    chalk_line(d, pts, CHALK + (160,), w)
    hole = [(cx + r * 0.18 * math.cos(t / 20 * 2 * math.pi), cy + r * 0.18 * math.sin(t / 20 * 2 * math.pi)) for t in range(21)]
    chalk_line(d, hole, CHALK + (140,), w)
    for k, col in ((0.55, BLUE), (0.75, PINK)):
        arc = [(cx + r * k * math.cos(a), cy + r * k * math.sin(a)) for a in [math.radians(200 + i * 6) for i in range(12)]]
        chalk_line(d, arc, col + (150,), w)


def pad(img, x, y, s):
    """A generic controller: d-pad, two buttons, start/select."""
    d = ImageDraw.Draw(img, "RGBA")
    w = max(2, int(3 * s))
    chalk_box(d, x, y, x + 150 * s, y + 70 * s, 30 * s, CHALK + (160,), w)
    cx, cy, a = x + 38 * s, y + 35 * s, 13 * s
    chalk_line(d, [(cx - a, cy), (cx + a, cy)], CHALK + (170,), max(3, int(6 * s)))
    chalk_line(d, [(cx, cy - a), (cx, cy + a)], CHALK + (170,), max(3, int(6 * s)))
    for bx, by, col in ((112, 42, RED), (128, 28, YELLOW)):
        d.ellipse([x + (bx - 7) * s, y + (by - 7) * s, x + (bx + 7) * s, y + (by + 7) * s], fill=col + (190,))
    for k in range(2):
        chalk_line(d, [(x + (62 + k * 16) * s, y + 50 * s), (x + (72 + k * 16) * s, y + 46 * s)], CHALK + (120,), w)


def pixel_text(text, size, scale, fg, bg):
    """Text as a game would draw it: an 1-bit font, scaled up with hard pixels."""
    font = ImageFont.truetype(DOT, size)
    w = int(font.getlength(text)) + 2
    img = Image.new("RGB", (w, size + 4), bg)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((1, 1), text, font=font, fill=fg)
    return img.resize((w * scale, (size + 4) * scale), Image.NEAREST)


def crt(img, x, y, s, lines, underline, note):
    """A chalk CRT showing a game's text box; one word underlined and explained."""
    d = ImageDraw.Draw(img, "RGBA")
    w = max(2, int(3 * s))
    W, H = 330 * s, 240 * s
    chalk_box(d, x, y, x + W, y + H, 22 * s, CHALK + (170,), w)
    # the stand
    chalk_line(d, [(x + W * 0.38, y + H), (x + W * 0.32, y + H + 22 * s), (x + W * 0.68, y + H + 22 * s),
                   (x + W * 0.62, y + H)], CHALK + (150,), w)
    # the screen: a dark game picture with a text box
    sx0, sy0, sx1, sy1 = x + 22 * s, y + 20 * s, x + W - 22 * s, y + H - 22 * s
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=12 * s, fill=(0x10, 0x1c, 0x2c, 255))
    # a little pixel landscape
    random.seed(4)
    tile = max(2, int(6 * s))
    for k in range(int((sx1 - sx0) / tile)):
        hgt = 3 + int(2 * math.sin(k * 0.6)) + random.randrange(2)
        for j in range(hgt):
            d.rectangle([sx0 + k * tile, sy1 - 90 * s - j * tile, sx0 + (k + 1) * tile - 1, sy1 - 90 * s - (j - 1) * tile - 1],
                        fill=(0x2f, 0x6b, 0x4a, 255) if j < hgt - 1 else (0x4f, 0x9b, 0x6a, 255))
    for k in range(5):
        px, py = sx0 + (40 + k * 52) * s, sy0 + (18 + (k % 2) * 10) * s
        d.rectangle([px, py, px + tile - 1, py + tile - 1], fill=(0xf1, 0xd3, 0x7c, 255))
    # the text box
    bx0, by0, bx1, by1 = sx0 + 10 * s, sy1 - 82 * s, sx1 - 10 * s, sy1 - 10 * s
    d.rectangle([bx0, by0, bx1, by1], fill=(0x0c, 0x0c, 0x30, 255), outline=(0xee, 0xee, 0xee, 255), width=max(2, int(3 * s)))
    scale = max(1, int(round(2 * s)))
    ty = by0 + 9 * s
    for i, line in enumerate(lines):
        pic = pixel_text(line, 12, scale, (0xf4, 0xf4, 0xf4), (0x0c, 0x0c, 0x30))
        img.paste(pic, (int(bx0 + 10 * s), int(ty)))
        if i == underline[0]:
            # chalk underline under the word, like the study panel
            font = ImageFont.truetype(DOT, 12)
            pre = font.getlength(line[:underline[1]]) * scale
            wl = font.getlength(line[underline[1]:underline[1] + underline[2]]) * scale
            ux = bx0 + 10 * s + pre
            uy = ty + 15 * scale
            chalk_line(d, [(ux, uy), (ux + wl, uy)], YELLOW + (240,), max(2, int(3 * s)))
            anchor = (ux + wl / 2, ty - 2 * s)
        ty += 30 * s
    # the explanation, chalked beside the TV
    nx, ny = x + W + 26 * s, by0 - 4 * s
    bend = (anchor[0] + 14 * s, by0 - 22 * s)
    chalk_line(d, [anchor, bend, (nx - 6 * s, ny - 8 * s)], YELLOW + (200,), w)
    chalk_line(d, [(nx - 6 * s, ny - 8 * s), (nx - 18 * s, ny - 14 * s)], YELLOW + (200,), w)
    chalk_line(d, [(nx - 6 * s, ny - 8 * s), (nx - 16 * s, ny + 2 * s)], YELLOW + (200,), w)
    head, reading, gloss = note
    chalk_text(img, (nx, ny - 26 * s), reading, ImageFont.truetype(KLEE, max(10, int(16 * s))), YELLOW)
    chalk_text(img, (nx, ny - 6 * s), head, ImageFont.truetype(KLEE, max(12, int(40 * s))), CHALK)
    chalk_text(img, (nx, ny + 44 * s), gloss, ImageFont.truetype(KLEE, max(10, int(17 * s))), GREEN)


def tray(img):
    w, h = img.size
    d = ImageDraw.Draw(img)
    d.rectangle([0, h - 14, w, h], fill=WOOD)
    d.rectangle([0, h - 14, w, h - 11], fill=WOOD2)
    d.rounded_rectangle([w * 0.62, h - 22, w * 0.62 + 46, h - 13], radius=4, fill=CHALK)
    d.rounded_rectangle([w * 0.62 + 56, h - 21, w * 0.62 + 86, h - 13], radius=4, fill=YELLOW)


def save(img, path, colors=128):
    img.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)


def main(font_path, dot_path, out):
    global KLEE, DOT
    KLEE, DOT = font_path, dot_path
    f = lambda size: ImageFont.truetype(font_path, size)

    # icon 128x128: a cartridge whose label says 語 (word), and a hanamaru
    icon = board(128, 128, 3)
    d = ImageDraw.Draw(icon)
    d.rounded_rectangle([0, 0, 127, 127], radius=10, outline=WOOD2, width=6)
    random.seed(5)
    cartridge(icon, 25, 10, 0.74, "語")
    hanamaru(icon, 106, 26, 13, 3)
    chalk_text(icon, (64, 113), "LINGO", f(17), YELLOW, anchor="mm", passes=2)
    save(icon, f"{out}/icon0.png")

    # pic0 960x544 and bg 840x500 share a layout
    for name, (w, h), seed in (("pic0.png", (960, 544), 7), ("bg.png", (840, 500), 9)):
        img = board(w, h, seed)
        s = w / 960
        random.seed(seed)
        chalk_text(img, (56 * s, 54 * s), "ことば", f(int(84 * s)), CHALK)
        chalk_text(img, (60 * s, 176 * s), "LINGO", f(int(52 * s)), YELLOW)
        chalk_text(img, (62 * s, 250 * s), "Any retro Japanese game.", f(int(24 * s)), CHALK)
        chalk_text(img, (62 * s, 282 * s), "Every word, explained as you play.", f(int(24 * s)), (0x9d, 0xb2, 0xa4))
        chalk_text(img, (62 * s, 330 * s), "GB · GBA · FC · SFC · MD · PCE · PS · SS · and more", f(int(16 * s)), GREEN)
        cartridge(img, 70 * s, 378 * s, 0.9 * s, "あ")
        disc(img, 236 * s, 436 * s, 52 * s, s)
        pad(img, 310 * s, 398 * s, 1.0 * s)
        crt(img, 470 * s, 46 * s, 1.0 * s, ["ゆうしゃよ　まおうを", "たおしてくれ！"], (0, 6, 3),
            ("魔王", "まおう", "demon king"))
        chalk_text(img, (520 * s, 372 * s), "書", f(int(40 * s)), GREEN)
        chalk_text(img, (566 * s, 372 * s), "Kaku practice  ·  checkpoints", f(int(18 * s)), (0x9d, 0xb2, 0xa4))
        chalk_text(img, (566 * s, 400 * s), "voiced lines  ·  offline", f(int(18 * s)), (0x9d, 0xb2, 0xa4))
        hanamaru(img, 868 * s, 430 * s, 54 * s, max(3, int(6 * s)))
        tray(img)
        save(img, f"{out}/{name}")

    # startup gate 280x158
    st = board(280, 158, 11)
    random.seed(11)
    chalk_text(st, (18, 14), "ことば", f(38), CHALK)
    chalk_text(st, (20, 70), "LINGO", f(30), YELLOW)
    chalk_text(st, (21, 112), "any retro Japanese game", f(14), GREEN)
    cartridge(st, 196, 22, 0.62, "あ")
    save(st, f"{out}/startup.png", 64)
    print("art written to", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
