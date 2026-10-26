/* Host test for the ROM reader: builds a fake ROM laid out like a Gen 3 JP
 * cartridge (made-up names, art and colours) and checks the reader finds it. */
#include "../src/psp2-lingo-rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t enc(uint32_t cp) {
	for (int b = 0; b < 0xF7; ++b) if (lingoGen3Char(b) == cp) return b;
	return 0;
}

/* LZ77 with literal-only blocks: valid input for the BIOS decompressor */
static size_t lzLiteral(const uint8_t* src, size_t n, uint8_t* out) {
	size_t o = 0;
	out[o++] = 0x10; out[o++] = n & 0xFF; out[o++] = (n >> 8) & 0xFF; out[o++] = (n >> 16) & 0xFF;
	for (size_t i = 0; i < n; i += 8) {
		out[o++] = 0;
		for (size_t k = i; k < i + 8 && k < n; ++k) out[o++] = src[k];
	}
	return o;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

int main(void) {
	size_t size = 0x400000;
	uint8_t* rom = calloc(1, size);
	for (size_t i = 0; i < size; ++i) rom[i] = (uint8_t) (i * 2654435761u >> 13); /* noise */
	/* names: placeholder, the two real first names the reader keys on, then made-up ones */
	const char* names[] = { "ーーーーー", "フシギダネ", "フシギソウ", "テストモン", "ニセモノ", "サンプル" };
	size_t namesAt = 0x100000;
	int nSpecies = 200;
	for (int s = 0; s < nSpecies; ++s) {
		uint8_t* e = rom + namesAt + s * 6;
		memset(e, 0xFF, 6);
		const char* nm = s < 6 ? names[s] : "ダミー";
		uint32_t cps[8];
		int n = lingoUtf8Decode(nm, cps, 8);
		for (int k = 0; k < n && k < 5; ++k) e[k] = enc(cps[k]);
	}
	/* sprite data: species s uses colour index (s % 15) + 1 in a diagonal stripe */
	size_t dataAt = 0x200000;
	uint32_t frontTable = 0x180000, backTable = 0x181000 + 8 * 400, palTable = 0x190000;
	for (int s = 0; s < nSpecies; ++s) {
		uint8_t pic[0x800];
		for (int t = 0; t < 64; ++t)
			for (int y = 0; y < 8; ++y)
				for (int x = 0; x < 8; x += 2) {
					int gx = (t % 8) * 8 + x, gy = (t / 8) * 8 + y;
					int c0 = (gx == gy) ? (s % 15) + 1 : 0, c1 = (gx + 1 == gy) ? (s % 15) + 1 : 0;
					pic[t * 32 + y * 4 + x / 2] = c0 | (c1 << 4);
				}
		size_t picOff = dataAt;
		dataAt += lzLiteral(pic, sizeof pic, rom + dataAt);
		dataAt = (dataAt + 3) & ~3u;
		uint8_t pal[0x20];
		for (int i = 0; i < 16; ++i) put16(pal + i * 2, (uint16_t) (i == (s % 15) + 1 ? 0x001F : 0x7FFF));
		size_t palOff = dataAt;
		dataAt += lzLiteral(pal, sizeof pal, rom + dataAt);
		dataAt = (dataAt + 3) & ~3u;
		put32(rom + frontTable + s * 8, 0x08000000 + picOff); put16(rom + frontTable + s * 8 + 4, 0x800); put16(rom + frontTable + s * 8 + 6, s);
		put32(rom + backTable + s * 8, 0x08000000 + picOff); put16(rom + backTable + s * 8 + 4, 0x800); put16(rom + backTable + s * 8 + 6, s);
		put32(rom + palTable + s * 8, 0x08000000 + palOff); put16(rom + palTable + s * 8 + 4, s); put16(rom + palTable + s * 8 + 6, 0);
	}
	struct LingoRom r;
	bool ok = lingoRomIndex(rom, size, &r);
	printf("index: %s names@%zx pics@%x pals@%x species %d\n", ok ? "found" : "missing", r.names, r.pics, r.pals, r.nSpecies);
	CHECK(ok, "tables not found");
	CHECK(r.names == namesAt, "names at %zx", r.names);
	CHECK(r.pics == frontTable, "front table at %x, want %x (not the back table)", r.pics, frontTable);
	CHECK(r.pals == palTable, "palettes at %x", r.pals);
	uint32_t q[8];
	int n = lingoUtf8Decode("ニセモノ", q, 8);
	int sp = lingoRomFindSpecies(&r, q, n);
	CHECK(sp == 4, "species for ニセモノ = %d", sp);
	n = lingoUtf8Decode("ニセ", q, 8);
	CHECK(lingoRomFindSpecies(&r, q, n) == -1, "prefix should not match");
	static uint32_t px[64 * 64];
	CHECK(lingoRomSprite(&r, 4, px), "sprite decode failed");
	/* species 4: colour index 5 is red (0x001F -> R=255), on the diagonal; elsewhere clear */
	CHECK(px[10 * 64 + 10] == 0xFF0000FFu, "diagonal pixel %08X", px[10 * 64 + 10]);
	CHECK(px[10 * 64 + 20] == 0, "off-diagonal pixel %08X", px[10 * 64 + 20]);
	/* a ROM with no Pokémon tables */
	uint8_t* other = calloc(1, size);
	CHECK(!lingoRomIndex(other, size, &r), "found tables in an empty ROM");
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
