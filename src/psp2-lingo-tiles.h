/* Lingo: working out a game's text encoding from its font.
 *
 * Cartridge consoles draw text with tiles: the font is a block of 8x8 letter
 * pictures in video memory, and the game's text codes are (nearly always)
 * the numbers of those tiles, both in its text buffers and in the tile map
 * that is on screen. So reading the pictures gives the table:
 *
 * 1. Decode every tile in video memory in the console's tile format.
 * 2. Recognise each one with the screen reader's reference glyphs.
 * 3. Fonts store kana in a fixed order (あいうえお…), so fit the usual orders
 *    to the recognised tiles: where most of a run agrees, the order fixes
 *    the tiles the recogniser got wrong.
 * 4. Turn tile addresses into codes in each way the console numbers tiles,
 *    try every candidate table on the game's memory, and keep the one that
 *    reads the most real Japanese.
 *
 * No Vita dependencies; tested by tests/test_tiles.c. */
#ifndef PSP2_LINGO_TILES_H
#define PSP2_LINGO_TILES_H

#include "psp2-lingo-ocr.h"
#include "psp2-lingo-reader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum LingoTileFormat {
	LTF_NES,         /* 2bpp: 8 bytes plane 0, then 8 bytes plane 1 */
	LTF_2BPP,        /* 2bpp, rows of (plane 0, plane 1): Game Boy, SNES, WonderSwan */
	LTF_4BPP_PLANAR, /* 4bpp, planes 0/1 rows then planes 2/3 rows: SNES, PC Engine */
	LTF_4BPP_MD,     /* 4bpp packed, high nibble first: Mega Drive */
	LTF_4BPP_MD_SWAP,/* the same with each 16-bit word's bytes swapped (as an emulator stores it) */
	LTF_4BPP_GBA,    /* 4bpp packed, low nibble first: Game Boy Advance */
	LTF_1BPP,        /* 1bpp, one byte a row: MSX */
	LTF_COUNT
};

int lingoTileBytes(enum LingoTileFormat f);

/* Set to make a scan in progress give up quickly (the game is closing). */
extern volatile int lingoTilesStop;

/* A letter found in a font: the tile at byte address addr of the data. */
struct LingoTileHit {
	uint32_t addr;
	uint32_t cp;
	float score;
	bool fromOrder; /* placed by the kana order rather than read alone */
};

struct LingoFontScan {
	enum LingoTileFormat fmt;
	struct LingoTileHit* hits;
	int n, cap;
	int kana;          /* kana among the hits */
	int orderRuns;     /* kana orders that fitted */
	uint32_t blank[64];/* blank tiles near the font (spaces) */
	int nBlank;
};

/* Reads every tile of data in format fmt. Returns the number of letters found. */
int lingoTilesScan(const struct LingoOcr* o, const uint8_t* data, uint32_t size, enum LingoTileFormat fmt,
                   struct LingoFontScan* out);
void lingoTilesFree(struct LingoFontScan* s);

/* Scores a line of text: how many of its letters are believable Japanese
 * (e.g. those covered by dictionary words). */
typedef int (*LingoTilesScorer)(const uint32_t* cp, int n, void* user);

/* A scorer for lingoTilesFind: the letters in dictionary words, for lines
 * that are mostly words; 0 for anything else. */
struct LingoDict;
int lingoTilesWordScore(const struct LingoDict* d, const uint32_t* cp, int n);

/* The whole job for one console: finds the font in its video memory (or, for
 * the NES, in the cartridge's CHR ROM), builds candidate tables and keeps
 * the one that reads the most text in the regions now. best gets it and
 * the return value is its score (letters read as Japanese), 0 if none.
 * scratch is a second table to work in. */
int lingoTilesFind(const struct LingoOcr* o, const char* system, const struct LingoRegions* rs, const uint8_t* rom,
                   size_t romSize, LingoTilesScorer scorer, void* user, struct LingoTable* best,
                   struct LingoTable* scratch, char* why, size_t whyLen);

/* Every line a table reads in the regions, a slice at a time (so graphics
 * read as scraps of text can't crowd out the real text). */
int lingoTilesReadAll(const struct LingoTable* t, const struct LingoRegions* rs, struct LingoText* out, int max);

/* Builds a table from a scan: tile at address a gets code (a - base) / size,
 * in the given mode. Returns how many characters it mapped. */
int lingoTilesTable(const struct LingoFontScan* s, uint32_t base, enum LingoCodeMode mode, uint16_t mask, uint16_t stride,
                    struct LingoTable* t);

#endif
