#!/usr/bin/env python3
"""Makes test frames for tests/test_ocr.c: a busy "game picture" on top and
a text box with two lines of Japanese below, as 8-bit brightness .raw files
with the expected text alongside.

Usage: make_ocr_frames.py outdir font.ttf:size[:name] ...
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFont

LINES = [
    ("われわれ　にんげんは　ポケモンと", "なかよく　あそんだり"),
    ("この　さきは　あぶないから", "きを　つけて　いくんだよ"),
    ("ゆうしゃよ　まおうを　たおすのだ", "ちからを　あわせて　たたかえ"),
    ("やまの うえに しろが みえる", "だれか いるようだ"),  # narrow spaces, as some games use
    ("勇者よ、魔王を倒してくれ", "この村は平和です"),
]


def frame(font_path, size, lines, seed, w=320, h=224, invert=False):
    rnd = random.Random(seed)
    img = Image.new("L", (w, h), 40)
    d = ImageDraw.Draw(img)
    # the scene: noise, tiles and a few "sprites"
    for _ in range(900):
        x, y = rnd.randrange(w), rnd.randrange(h - 80)
        d.rectangle((x, y, x + rnd.randrange(1, 6), y + rnd.randrange(1, 6)), fill=rnd.randrange(256))
    for _ in range(6):
        x, y = rnd.randrange(w - 16), rnd.randrange(h - 100)
        d.rectangle((x, y, x + 15, y + 15), fill=rnd.randrange(256))
    # the text box
    bg, fg = (230, 20) if invert else (24, 235)
    top = h - 76
    d.rectangle((6, top, w - 7, h - 7), fill=bg, outline=fg)
    font = ImageFont.truetype(font_path, size)
    d.fontmode = "1"
    y = top + 10
    for line in lines:
        d.text((16, y), line, font=font, fill=fg)
        y += size + 8
    return img


def main(out, specs):
    os.makedirs(out, exist_ok=True)
    k = 0
    for spec in specs:
        parts = spec.split(":")
        path, size = parts[0], int(parts[1])
        for i, lines in enumerate(LINES):
            if "勇" in lines[0] and size < 12:
                continue  # kanji at 8-10 px: not a fair test
            img = frame(path, size, lines, seed=k, invert=(i % 2 == 1))
            base = os.path.join(out, f"f{k:03d}")
            with open(base + ".raw", "wb") as f:
                f.write(bytes([img.width % 256, img.width // 256, img.height % 256, img.height // 256]) + img.tobytes())
            with open(base + ".txt", "w", encoding="utf-8") as f:
                f.write(os.path.basename(path) + ":" + str(size) + "\n" + "\n".join(lines) + "\n")
            k += 1
    print(f"{k} frames in {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
