"""Makes fake console memory for tests/test_tiles.c: a font drawn as tiles in
each console's format (in an order like games use, among unrelated graphics
tiles), a tile map showing a line of dialogue, and a RAM buffer holding it.

Fonts are ones Lingo's reference glyphs don't include, so recognition is
tested on letters it has never seen.

Usage: make_tile_dumps.py outdir font8.ttf font16.ttf
Writes <case>.vram, <case>.ram and cases.txt (case, system, expected line).
"""
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFont

OUT, FONT8, FONT16 = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)
rnd = random.Random(7)

GOJUON = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん"
GOJUON_K = "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン"
SMALL = "ぁぃぅぇぉっゃゅょ"
SMALL_K = "ァィゥェォッャュョ"
VOICED = "がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ"
VOICED_K = "ガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ"
PUNCT = "ー。、！？「」"
UNVOICE = {}
for a, b in zip("がぎぐげござじずぜぞだぢづでどばびぶべぼ", "かきくけこさしすせそたちつてとはひふへほ"):
    UNVOICE[a] = (b, "゛")
for a, b in zip("ぱぴぷぺぽ", "はひふへほ"):
    UNVOICE[a] = (b, "゜")
for a, b in zip("ガギグゲゴザジズゼゾダヂヅデドバビブベボ", "カキクケコサシスセソタチツテトハヒフヘホ"):
    UNVOICE[a] = (b, "゛")


def glyph(font, ch, w, h, size):
    img = Image.new("1", (w, h), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    f = ImageFont.truetype(font, size)
    l, t, r, b = d.textbbox((0, 0), ch, font=f)
    d.text(((w - (r - l)) // 2 - l, (h - (b - t)) // 2 - t), ch, font=f, fill=1)
    return [[img.getpixel((x, y)) for x in range(w)] for y in range(h)]


def noise_tile():
    """An unrelated graphics tile: blobs and stripes."""
    kind = rnd.randrange(3)
    if kind == 0:
        return [[rnd.randrange(4) for _ in range(8)] for _ in range(8)]
    if kind == 1:
        return [[(x + y) // 2 % 4 for x in range(8)] for y in range(8)]
    c = rnd.randrange(1, 4)
    return [[c if (x - 3.5) ** 2 + (y - 3.5) ** 2 < 10 else 0 for x in range(8)] for y in range(8)]


def enc(fmt, px, ink=3):
    """8x8 values (0/1 letter or 0..3 noise) to bytes."""
    vals = [[(ink if v == 1 else v) if fmt != "1bpp" else (1 if v else 0) for v in row] for row in px]
    out = bytearray()
    if fmt == "nes":
        for p in range(2):
            for r in range(8):
                out.append(sum(((vals[r][x] >> p) & 1) << (7 - x) for x in range(8)))
    elif fmt == "2bpp":
        for r in range(8):
            for p in range(2):
                out.append(sum(((vals[r][x] >> p) & 1) << (7 - x) for x in range(8)))
    elif fmt == "4bpp_planar":
        for pl in ((0, 1), (2, 3)):
            for r in range(8):
                for p in pl:
                    out.append(sum(((vals[r][x] >> p) & 1) << (7 - x) for x in range(8)))
    elif fmt in ("md", "md_swap"):
        for r in range(8):
            for x in range(0, 8, 2):
                out.append((vals[r][x] << 4) | vals[r][x + 1])
        if fmt == "md_swap":
            for i in range(0, len(out), 2):
                out[i], out[i + 1] = out[i + 1], out[i]
    elif fmt == "1bpp":
        for r in range(8):
            out.append(sum(vals[r][x] << (7 - x) for x in range(8)))
    return bytes(out)


def split16(px16, w):
    """A w x 16 letter into 8x8 tiles: [(dx, dy, tile)]."""
    tiles = []
    for dy in (0, 8):
        for dx in range(0, w, 8):
            tiles.append((dx, dy, [row[dx:dx + 8] for row in px16[dy:dy + 8]]))
    return tiles


cases = []


def make(name, system, fmt, size, font_at, chars, line, layout="8", tile_stride=16, map_mode="byte",
         map_at=None, map_w=32, attr=0, dakuten_above=False, base_tile=0):
    tb = {"nes": 16, "2bpp": 16, "4bpp_planar": 32, "md": 32, "md_swap": 32, "1bpp": 8}[fmt]
    vram = bytearray(size)
    nt = size // tb
    # noise everywhere first
    for t in range(nt):
        if rnd.random() < 0.5:
            vram[t * tb:(t + 1) * tb] = enc(fmt, noise_tile())
    code_of = {}
    first = font_at // tb
    if layout == "8":
        for i, ch in enumerate(chars):
            t = first + i
            vram[t * tb:(t + 1) * tb] = enc(fmt, glyph(FONT8, ch, 8, 8, 8))
            code_of[ch] = t
    else:  # 16x16 letters as 2x2 tiles in a sheet tile_stride wide, two tiles a letter
        for i, ch in enumerate(chars):
            t = first + (i % (tile_stride // 2)) * 2 + (i // (tile_stride // 2)) * tile_stride * 2
            for dx, dy, tile in split16(glyph(FONT16, ch, 16, 16, 15), 16):
                tt = t + dx // 8 + (dy // 8) * tile_stride
                vram[tt * tb:(tt + 1) * tb] = enc(fmt, tile)
            code_of[ch] = t
    # a blank tile for the space, right after the font
    if layout == "8":
        space = first + len(chars)
    else:  # past the last row of the sheet
        space = first + ((len(chars) + tile_stride // 2 - 1) // (tile_stride // 2)) * tile_stride * 2
    vram[space * tb:(space + 1) * tb] = bytes(tb)

    def code(t):
        """the tile's number as the game writes it: counted from base_tile"""
        return t - base_tile

    # the line as codes, with voiced marks on the row above when the font has none
    row, above = [], []
    for ch in line:
        if ch == "　":
            row.append(space)
            above.append(space)
        elif ch in code_of:
            row.append(code_of[ch])
            above.append(space)
        elif dakuten_above and ch in UNVOICE:
            b, m = UNVOICE[ch]
            row.append(code_of[b])
            above.append(code_of[m])
        else:
            raise SystemExit(f"{name}: no tile for {ch}")
    ram = bytearray(rnd.randrange(256) for _ in range(0x800))
    codes = [code(t) & 0xFF if map_mode == "byte" else code(t) for t in row]
    if map_mode == "byte":
        ram[0x300:0x300 + len(codes)] = bytes(codes)
        ram[0x300 + len(codes)] = 0xFF
    if map_at is not None:
        unit = 1 if map_mode == "byte" else 2
        # an empty screen: the blank tile everywhere
        for cell in range(map_w * 32):
            a = map_at + cell * unit
            c = code(space)
            if map_mode == "byte":
                vram[a] = c & 0xFF
            elif map_mode == "be16":
                vram[a], vram[a + 1] = (c | attr) >> 8, (c | attr) & 0xFF
            else:
                vram[a], vram[a + 1] = (c | attr) & 0xFF, (c | attr) >> 8
        cells = []  # (row, column, tile)
        if layout == "8":
            for rr, seq in ((0, above if dakuten_above else None), (1, row)):
                for i, t in enumerate(seq or []):
                    cells.append((rr, i, t))
        else:  # each letter is 2x2 tiles on screen
            for i, t in enumerate(row):
                parts = (t, t + 1, t + tile_stride, t + tile_stride + 1) if t != space else (space,) * 4
                for k, p in enumerate(parts):
                    cells.append((k // 2, i * 2 + k % 2, p))
        # the rest of the text box is blank
        for rr in range(2 if layout != "8" or dakuten_above else 1):
            for i in range(len(row) * (2 if layout != "8" else 1), map_w - 2):
                cells.append((rr + (0 if layout != "8" or dakuten_above else 1), i, space))
        for rr, i, t in cells:
                a = map_at + ((5 + rr) * map_w + 2 + i) * unit
                c = code(t)
                if map_mode == "byte":
                    vram[a] = c & 0xFF
                else:
                    v = (c & 0x3FF) | attr
                    if map_mode == "be16":
                        vram[a], vram[a + 1] = v >> 8, v & 0xFF
                    else:
                        vram[a], vram[a + 1] = v & 0xFF, v >> 8
    open(os.path.join(OUT, name + ".vram"), "wb").write(vram)
    open(os.path.join(OUT, name + ".ram"), "wb").write(ram)
    cases.append((name, system, line.replace("　", "　")))


DQ_FONT = list("0123456789") + list(GOJUON) + list(GOJUON_K) + list("゛゜") + list(PUNCT)
# NES, Dragon Quest style: 8x8 font, voiced marks on the row above, nametable after the patterns
make("nes", "fc", "nes", 0x2800, 0x1000 + 0x10 * 16, DQ_FONT, "その　むかし　ゆうしやロトが　カミから　ひかりのたまを　さずかり",
     map_mode="byte", map_at=0x2000, dakuten_above=True, base_tile=0x100)
# Game Boy: font in the 9000 block (signed tile numbers), letters with voicing built in
GB_FONT = list(GOJUON) + list(SMALL) + list(VOICED) + list(PUNCT)
make("gb", "gb", "2bpp", 0x1800, 0x1000, GB_FONT, "ぽけもんの　せかいへ　ようこそ",
     map_mode="byte", map_at=None, base_tile=0x100)
# Super Famicom: 16x16 kana as 2x2 tiles of 2bpp, tile map entries with palette bits, tiles counted from 0x4000
SFC_FONT = list(GOJUON) + list(SMALL) + list(VOICED) + list(PUNCT)
make("sfc", "sfc", "2bpp", 0x10000, 0x4000 + 2 * 16, SFC_FONT, "きょうは　いい　てんきですね", layout="16", tile_stride=16,
     map_mode="le16", map_at=0xF000, attr=0x2400, base_tile=0x400)
# Mega Drive: 8x8 4bpp as the emulator stores it, map entries in host order
MD_FONT = list(GOJUON) + list(GOJUON_K) + list(VOICED) + list(PUNCT)
make("md", "md", "md_swap", 0x10000, 0x800, MD_FONT, "ここは　まおうの　しろだ", map_mode="le16", map_at=0xC000,
     map_w=64, attr=0x8000)

with open(os.path.join(OUT, "cases.txt"), "w") as f:
    for name, system, line in cases:
        f.write(f"{name}\t{system}\t{line}\n")
print("wrote", len(cases), "cases")
