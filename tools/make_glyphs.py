#!/usr/bin/env python3
"""Builds glyphs.bin: reference pictures of Japanese characters for reading
text off the screen.

Each character is rendered without anti-aliasing (as retro games draw text)
from open fonts at the pixel sizes games use, and cropped the way Lingo
crops a character cell on screen: the advance width across, the line's ink
rows down. The bitmaps are stored as they are; Lingo computes its features
from them with the same code it uses for cells on screen.

Characters: kana, punctuation, Latin letters and digits, and the most
common kanji in everyday vocabulary (ranked by how many common JMdict words
use them).

Usage: make_glyphs.py jamdict.db out.bin font.ttf [font.ttf...]
"""
import sqlite3
import struct
import sys
from collections import Counter

from PIL import Image, ImageDraw, ImageFont

SIZES = [8, 10, 12, 14, 16]
KANJI_SIZES = [12, 16]
N_KANJI = 1500


def kana_set():
    out = [chr(c) for c in range(0x3041, 0x3095)]
    out += [chr(c) for c in range(0x30A1, 0x30FB)]
    out += list("゛゜ー。、！？「」『』・…〜（）：＋－×＝％＆★☆♪→←↑↓○●◆■")
    out += [chr(c) for c in range(0xFF10, 0xFF1A)]  # ０-９
    out += [chr(c) for c in range(0xFF21, 0xFF3B)]  # Ａ-Ｚ
    out += [chr(c) for c in range(0xFF41, 0xFF5B)]  # ａ-ｚ
    return out


def common_kanji(db_path, n):
    db = sqlite3.connect(db_path)
    common = set()
    for (idseq,) in db.execute(
            "SELECT k.idseq FROM KJP p JOIN Kanji k ON k.ID = p.kid "
            "WHERE p.text IN ('ichi1','news1','spec1','spec2')"):
        common.add(idseq)
    count = Counter()
    for idseq, text in db.execute("SELECT idseq, text FROM Kanji"):
        if idseq in common:
            for ch in text:
                if 0x4E00 <= ord(ch) <= 0x9FFF:
                    count[ch] += 1
    return [c for c, _ in count.most_common(n)]


def render(font_path, size, chars):
    """Mono renders of chars at size, cropped like an on-screen cell."""
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    H = ascent + descent + 2
    cells = {}
    band_top, band_bot = 10 ** 9, -1
    # the line band: rows any ordinary kana uses
    for ch in "あいうえおかきくけこアイウエオカキクケコがぎぐ":
        img = Image.new("1", (size * 2, H), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((0, 1), ch, font=font, fill=1)
        bb = img.getbbox()
        if bb:
            band_top = min(band_top, bb[1])
            band_bot = max(band_bot, bb[3])
    if band_bot < 0:
        return cells
    for ch in chars:
        img = Image.new("1", (size * 2, H), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((0, 1), ch, font=font, fill=1)
        if not img.getbbox():
            continue
        adv = int(round(font.getlength(ch))) or size
        cell = img.crop((0, band_top, max(adv, 1), band_bot))
        cells[ch] = cell
    return cells


def main(db_path, out_path, fonts):
    kana = kana_set()
    kanji = common_kanji(db_path, N_KANJI)
    entries = []
    seen = set()
    for fp in fonts:
        for size in SIZES:
            chars = kana + (kanji if size in KANJI_SIZES else [])
            for ch, cell in render(fp, size, chars).items():
                w, h = cell.size
                if w > 32 or h > 32:
                    continue
                bits = bytearray()
                px = cell.load()
                for y in range(h):
                    row = 0
                    for x in range(w):
                        row |= (1 if px[x, y] else 0) << x
                    bits += struct.pack("<I", row)
                key = (ch, w, h, bytes(bits))
                if key in seen:
                    continue
                seen.add(key)
                entries.append((ord(ch), size, w, h, bytes(bits)))
        print(f"{fp}: {len(entries)} glyphs so far", flush=True)
    with open(out_path, "wb") as f:
        f.write(b"LGLY" + struct.pack("<II", 2, len(entries)))
        for cp, size, w, h, bits in entries:
            f.write(struct.pack("<IBBBB", cp, size, w, h, 0) + bits)
    print(f"wrote {out_path}: {len(entries)} glyphs of {len(kana) + len(kanji)} characters")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
