/* Lingo: reads Pokémon names and sprites from the player's own Gen 3 ROM at
 * runtime. Nothing from any game is bundled; the tables are found by their
 * shape, so no version-specific addresses are needed. */
#include "psp2-lingo-rom.h"

#include <string.h>

#define GBA_ROM 0x08000000u

static uint32_t rd32(const uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t rd16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

/* GBA BIOS LZ77 (type 0x10). Returns bytes written or -1. */
int lingoLz77(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstMax) {
	if (srcLen < 4 || src[0] != 0x10) {
		return -1;
	}
	size_t out = src[1] | (src[2] << 8) | (src[3] << 16);
	if (out > dstMax) {
		return -1;
	}
	size_t s = 4, d = 0;
	while (d < out) {
		if (s >= srcLen) {
			return -1;
		}
		uint8_t flags = src[s++];
		for (int b = 0; b < 8 && d < out; ++b, flags <<= 1) {
			if (flags & 0x80) {
				if (s + 1 >= srcLen) {
					return -1;
				}
				int len = (src[s] >> 4) + 3;
				int disp = (((src[s] & 0xF) << 8) | src[s + 1]) + 1;
				s += 2;
				if ((size_t) disp > d) {
					return -1;
				}
				for (int k = 0; k < len && d < out; ++k, ++d) {
					dst[d] = dst[d - disp];
				}
			} else {
				if (s >= srcLen) {
					return -1;
				}
				dst[d++] = src[s++];
			}
		}
	}
	return (int) out;
}

static bool lzHeader(const uint8_t* rom, size_t size, uint32_t ptr, uint32_t* outSize) {
	if (ptr < GBA_ROM || ptr - GBA_ROM + 4 > size) {
		return false;
	}
	const uint8_t* p = rom + (ptr - GBA_ROM);
	if (p[0] != 0x10) {
		return false;
	}
	*outSize = p[1] | (p[2] << 8) | (p[3] << 16);
	return true;
}

/* Gen 3 JP names are 5 characters plus a terminator: 6 bytes each. The
 * table starts with a placeholder at index 0, then フシギダネ, フシギソウ. */
static const uint8_t FIRST_NAMES[12] = { 0x6C, 0x5C, 0x88, 0x91, 0x68, 0xFF, 0x6C, 0x5C, 0x88, 0x5F, 0x53, 0xFF };

static bool findNames(const uint8_t* rom, size_t size, struct LingoRom* r) {
	for (size_t i = 6; i + sizeof(FIRST_NAMES) < size; ++i) {
		if (rom[i] == 0x6C && !memcmp(rom + i, FIRST_NAMES, sizeof(FIRST_NAMES))) {
			r->names = i - 6;
			/* count entries that look like names */
			int n = 0;
			while (n < LINGO_MAX_SPECIES && r->names + (n + 1) * 6 <= size) {
				const uint8_t* e = rom + r->names + n * 6;
				bool ok = false;
				for (int k = 0; k < 6; ++k) {
					if (e[k] == 0xFF) {
						ok = true;
						break;
					}
				}
				if (!ok && n > 1) {
					break;
				}
				++n;
			}
			r->nSpecies = n;
			return n > 100;
		}
	}
	return false;
}

/* An array of {const void* data; u16 size_or_tag; u16 tag} where entry i
 * carries tag i and points at LZ77 data that decompresses to one of the
 * accepted sizes. tagAt is the byte offset of the tag inside the entry. */
static uint32_t findTable(const uint8_t* rom, size_t size, int tagAt, const uint32_t* sizes, int nSizes, uint32_t after) {
	for (size_t p = (after + 3) & ~3u; p + 8 * 12 < size; p += 4) {
		bool ok = true;
		for (int i = 1; i <= 10 && ok; ++i) {
			const uint8_t* e = rom + p + i * 8;
			uint32_t ptr = rd32(e);
			uint32_t got;
			if (rd16(e + tagAt) != i || !lzHeader(rom, size, ptr, &got)) {
				ok = false;
				break;
			}
			bool sizeOk = false;
			for (int s = 0; s < nSizes; ++s) {
				if (got == sizes[s]) {
					sizeOk = true;
				}
			}
			ok = sizeOk;
		}
		if (ok) {
			return (uint32_t) p;
		}
	}
	return 0;
}

bool lingoRomIndex(const uint8_t* rom, size_t size, struct LingoRom* r) {
	memset(r, 0, sizeof(*r));
	r->rom = rom;
	r->size = size;
	if (!rom || size < 0x100000 || !findNames(rom, size, r)) {
		return false;
	}
	static const uint32_t PIC[] = { 0x800, 0x1000 };
	static const uint32_t PAL[] = { 0x20 };
	r->pics = findTable(rom, size, 6, PIC, 2, 0);
	/* the normal palettes come first; the shiny ones follow */
	r->pals = findTable(rom, size, 4, PAL, 1, 0);
	r->ready = r->pics && r->pals;
	return r->ready;
}

int lingoRomFindSpecies(const struct LingoRom* r, const uint32_t* name, int len) {
	if (!r->names || len <= 0 || len > 5) {
		return -1;
	}
	for (int s = 1; s < r->nSpecies; ++s) {
		const uint8_t* e = r->rom + r->names + s * 6;
		int k = 0;
		for (; k < 6 && e[k] != 0xFF; ++k) {
			if (k >= len || lingoGen3Char(e[k]) != name[k]) {
				break;
			}
		}
		if (k == len && (k == 6 || e[k] == 0xFF)) {
			return s;
		}
	}
	return -1;
}

int lingoRomSpeciesName(const struct LingoRom* r, int species, uint32_t* out) {
	if (!r->names || species <= 0 || species >= r->nSpecies) {
		return 0;
	}
	const uint8_t* e = r->rom + r->names + species * 6;
	int k = 0;
	for (; k < 5 && e[k] != 0xFF; ++k) {
		out[k] = lingoGen3Char(e[k]);
	}
	return k;
}

bool lingoRomSprite(const struct LingoRom* r, int species, uint32_t* rgba) {
	if (!r->ready || species <= 0 || species >= r->nSpecies) {
		return false;
	}
	const uint8_t* pe = r->rom + r->pics + species * 8;
	const uint8_t* le = r->rom + r->pals + species * 8;
	if ((size_t) (pe - r->rom) + 8 > r->size || (size_t) (le - r->rom) + 8 > r->size) {
		return false;
	}
	uint32_t picPtr = rd32(pe), palPtr = rd32(le);
	static uint8_t pic[0x1000];
	uint8_t pal[0x20];
	if (picPtr < GBA_ROM || palPtr < GBA_ROM || picPtr - GBA_ROM >= r->size || palPtr - GBA_ROM >= r->size) {
		return false;
	}
	int pn = lingoLz77(r->rom + (picPtr - GBA_ROM), r->size - (picPtr - GBA_ROM), pic, sizeof(pic));
	int ln = lingoLz77(r->rom + (palPtr - GBA_ROM), r->size - (palPtr - GBA_ROM), pal, sizeof(pal));
	if (pn < 0x800 || ln != 0x20) {
		return false;
	}
	uint32_t colors[16];
	for (int i = 0; i < 16; ++i) {
		uint16_t c = rd16(pal + i * 2);
		uint32_t R = (c & 0x1F) * 255 / 31, G = ((c >> 5) & 0x1F) * 255 / 31, B = ((c >> 10) & 0x1F) * 255 / 31;
		colors[i] = i == 0 ? 0 : (0xFFu << 24) | (B << 16) | (G << 8) | R; /* ABGR, colour 0 is clear */
	}
	/* 8x8 tiles of 4bpp, 8 tiles across: the first 64x64 frame */
	for (int t = 0; t < 64; ++t) {
		int tx = (t % 8) * 8, ty = (t / 8) * 8;
		for (int y = 0; y < 8; ++y) {
			for (int x = 0; x < 8; ++x) {
				uint8_t b = pic[t * 32 + y * 4 + x / 2];
				int idx = (x & 1) ? b >> 4 : b & 0xF;
				rgba[(ty + y) * 64 + tx + x] = colors[idx];
			}
		}
	}
	return true;
}
