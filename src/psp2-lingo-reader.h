/* Lingo: live text reading for any game.
 *
 * Most games keep the text they are printing somewhere in RAM or video RAM,
 * as numbers that stand for characters in the game's own font. Lingo needs
 * that mapping (a "table", as ROM hackers call it) to read the text. It gets
 * one in one of four ways:
 *
 *   - built in, for games whose encoding is published (Pokémon disassemblies);
 *   - Shift-JIS, which many CD-era games use as is;
 *   - found automatically: while text is printing, Lingo tries the common
 *     font layouts (kana in あいうえお order) at every offset and keeps the
 *     one whose decoded text reads like Japanese (kana bigram statistics);
 *   - taught: the player types the line on screen and Lingo searches memory
 *     for it by relative search (letters keep their order in the font, so
 *     "code minus position in あいうえお" is the same for every letter).
 *
 * With a table, the reader watches which memory pages change, decodes the
 * text found there and reports a line once it has stopped changing.
 *
 * No Vita dependencies: tested on a PC by tests/test_reader.c. */
#ifndef PSP2_LINGO_READER_H
#define PSP2_LINGO_READER_H

#include "psp2-lingo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINGO_BIGRAM_N 84 /* ぁ..ん then ー */
extern const int8_t LINGO_BIGRAM[LINGO_BIGRAM_N][LINGO_BIGRAM_N];
#define LINGO_SJIS_ROWS 47
extern const uint16_t LINGO_SJIS[LINGO_SJIS_ROWS * 189];
/* Unicode for a Shift-JIS double-byte code, 0 if not one. */
uint32_t lingoSjis(uint8_t lead, uint8_t trail);
/* Index into LINGO_BIGRAM for a kana (katakana folded), -1 otherwise. */
int lingoBigramIndex(uint32_t cp);

/* How much of this reads like Japanese: the count of kana and kanji, or 0
 * when it looks like data (too few letters, too many other symbols, or kana
 * in orders Japanese never uses). strict raises the bar for kana-only text. */
int lingoJudgeText(const uint32_t* cp, int n, bool strict);

/* ---- memory the reader may look at ---- */

#define LR_MAX_REGIONS 8
struct LingoRegion {
	const uint8_t* data;
	uint32_t size;
	char name[16]; /* "wram", "vram", "ram", "ram2"... */
};
struct LingoRegions {
	struct LingoRegion r[LR_MAX_REGIONS];
	int n;
};
void lingoRegionsClear(struct LingoRegions* rs);
void lingoRegionsAdd(struct LingoRegions* rs, const char* name, const void* data, size_t size);

/* ---- tables ---- */

enum LingoCodeMode {
	LCM_BYTE = 0, /* one byte per character */
	LCM_LE16,     /* 16-bit little-endian units (tile maps: SNES, GBA, PC Engine) */
	LCM_BE16,     /* 16-bit big-endian units (Mega Drive) */
	LCM_SJIS,     /* Shift-JIS bytes: no table needed */
};

#define LT_NONE 0xFFFF

struct LingoTable {
	uint8_t mode;
	uint16_t mask;         /* 16-bit modes: which bits are the tile number */
	char region[16];       /* region name to read, "" for every region */
	uint16_t stride;       /* units per screen row, for joining lines and marks; 0 if unknown */
	uint16_t dakuten;      /* code of ゛ drawn on the row above the letter, LT_NONE if not used */
	uint16_t handakuten;   /* code of ゜, likewise */
	uint16_t newline[4];   /* codes that start a new line */
	int nNewline;
	int nMapped;
	char source[24];       /* how the table was made, for the panel */
	uint32_t ignore[16];   /* places (region << 24 | unit) whose text the player hid */
	int nIgnore;
	uint16_t map[65536];   /* code -> Unicode (BMP), 0 = not a character */
};

void lingoTableInit(struct LingoTable* t, enum LingoCodeMode mode, uint16_t mask);
void lingoTableSet(struct LingoTable* t, uint16_t code, uint32_t cp);
/* Puts the characters of utf8 at consecutive codes from first. */
void lingoTableSetRun(struct LingoTable* t, uint16_t first, const char* utf8);
/* Atlas/Thingy ".tbl" text: "B1=あ" lines, "*4F" newline, plus Lingo's
 * own ";lingo mode=byte mask=ffff region=wram stride=20 dakuten=e5 handakuten=e4". */
bool lingoTableLoad(struct LingoTable* t, const char* path);
bool lingoTableSave(const struct LingoTable* t, const char* path);
bool lingoTableParse(struct LingoTable* t, const char* text);
/* A table this game is known to use. system is Lingo's console id ("gb",
 * "gba", "fc"...); rom may be NULL. */
bool lingoTablePreset(struct LingoTable* t, const char* system, const uint8_t* rom, size_t romSize);

/* Reads the unit at index i of a region in the table's mode. */
uint32_t lingoUnit(const struct LingoTable* t, const struct LingoRegion* r, uint32_t i);
uint32_t lingoUnitCount(const struct LingoTable* t, const struct LingoRegion* r);
/* Decodes text from unit start until the first non-character (at most maxUnits).
 * Writes codepoints, returns how many; *used gets the units consumed. */
int lingoTableDecode(const struct LingoTable* t, const struct LingoRegion* r, uint32_t start, uint32_t maxUnits,
                     uint32_t* out, int outMax, uint32_t* used, uint16_t* codes);
/* Stop reading text from around this place (a LingoText.start). */
void lingoTableIgnore(struct LingoTable* t, uint32_t start);
/* The player corrected a character: code now means cp. */
void lingoTableFix(struct LingoTable* t, uint16_t code, uint32_t cp);

/* ---- the live reader ---- */

#define LR_PAGE 512
#define LR_MAX_RUNS 96

struct LingoReader {
	struct LingoTable* table; /* not owned */
	uint8_t* prev[LR_MAX_REGIONS];
	uint32_t* pageChanged[LR_MAX_REGIONS]; /* scan number when each page last changed */
	uint32_t size[LR_MAX_REGIONS];
	const uint8_t* data[LR_MAX_REGIONS];
	uint32_t scan;
	uint32_t frame;
	uint32_t candHash, candStart;
	int candStable;
	uint32_t lastHash;
	bool primed;
	int lastKana; /* kana in the line last reported, for diagnostics */
	/* lines seen recently, to tell dialogue typing itself out from menus */
	struct {
		uint32_t start, hash;
		uint16_t len;
		uint32_t grewScan;
	} seen[32];
};

void lingoReaderInit(struct LingoReader* rd, struct LingoTable* table);
void lingoReaderFree(struct LingoReader* rd);
/* Every emulated frame. Returns true with out filled when a new or longer line has settled. */
bool lingoReaderStep(struct LingoReader* rd, const struct LingoRegions* rs, struct LingoText* out);
/* Finds every line of text in the regions right now (for diagnostics and tests). */
int lingoReaderFindAll(const struct LingoTable* t, const struct LingoRegions* rs, struct LingoText* out, int max);

/* ---- finding a table automatically ---- */

struct LingoDetect;
struct LingoDetect* lingoDetectNew(void);
void lingoDetectFree(struct LingoDetect* d);
/* Call every few frames while the game is printing text (a few seconds is plenty). */
void lingoDetectSample(struct LingoDetect* d, const struct LingoRegions* rs);
/* How sure the detector is so far, 0..1 (for a progress bar). */
float lingoDetectConfidence(const struct LingoDetect* d);
/* Picks the best table. False (and a reason in why) when nothing reads like Japanese. */
bool lingoDetectFinish(struct LingoDetect* d, const struct LingoRegions* rs, struct LingoTable* out, char* why, size_t whyLen);

/* ---- teaching: the player types what is on screen ---- */

/* typed: the line as the player read it (kana, spaces optional). On success
 * out holds a table good for at least those letters and, usually, the whole
 * font; why says what was found or what went wrong. */
bool lingoTeach(const struct LingoRegions* rs, const uint32_t* typed, int n, struct LingoTable* out, char* why,
                size_t whyLen);

#endif
