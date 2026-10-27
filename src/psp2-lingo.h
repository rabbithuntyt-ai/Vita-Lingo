/* Lingo: an offline Japanese study layer for mGBA on PS Vita.
 *
 * The core (text decoding, text-pointer scanning, dictionary lookup) has no
 * Vita dependencies so it can be unit tested on a PC. The panel UI lives in
 * psp2-lingo-ui.c.
 */
#ifndef PSP2_LINGO_H
#define PSP2_LINGO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINGO_MAX_TEXT 640      /* codepoints in one message */
#define LINGO_MAX_TOKENS 160
#define LINGO_MAX_SCAN_BYTES 700 /* longest message we will walk */

/* ---- Text decoding (Pokemon Gen 3, Japanese character set) ---- */

/* Decodes a 0xFF-terminated Gen 3 string into Unicode codepoints.
 * Line and page breaks become '\n', control codes are skipped, and
 * unexpanded placeholders become U+FF0A. Returns the number of codepoints,
 * or -1 if the bytes do not look like Japanese text (unterminated within
 * max, or containing bytes outside the character set). */
int lingoDecodeGen3(const uint8_t* src, size_t max, uint32_t* out, int outMax);

/* Codepoint for one Gen 3 JP byte, 0 if unmapped. */
uint32_t lingoGen3Char(uint8_t b);

bool lingoIsKana(uint32_t cp);
uint32_t lingoKataToHira(uint32_t cp);

/* ---- Memory view and text-pointer scanner ---- */

struct LingoMem {
	const uint8_t* ewram; /* 0x02000000, 256 KiB */
	const uint8_t* iwram; /* 0x03000000, 32 KiB */
	const uint8_t* rom;   /* 0x08000000 */
	uint32_t romSize;
};

/* Returns a host pointer for a GBA address and how many bytes are readable
 * from there, or NULL. */
const uint8_t* lingoResolve(const struct LingoMem* mem, uint32_t addr, size_t* avail);

struct LingoText {
	uint32_t cp[LINGO_MAX_TEXT];
	uint16_t code[LINGO_MAX_TEXT]; /* the game's code for each character, when hasCodes */
	int len;
	uint32_t start;  /* where it came from (a GBA address, or region << 24 | unit) */
	uint32_t hash;
	bool hasCodes;
};

#define LINGO_CANDIDATES 48

struct LingoCandidate {
	uint32_t addr;      /* where the pointer variable lives */
	uint32_t value;     /* last pointer value seen */
	uint32_t msgStart;  /* where the current message began, if we saw it jump */
	uint32_t lastFrame;
	uint16_t steps;     /* consecutive small advances */
	uint16_t ones;      /* of those, advances by exactly one byte */
	uint8_t checked;    /* the message from msgStart: 0 unchecked, 1 Japanese, 2 not */
};

struct LingoScanner {
	uint32_t prevEwram[0x40000 / 4];
	uint32_t prevIwram[0x8000 / 4];
	struct LingoCandidate cand[LINGO_CANDIDATES];
	uint32_t frame;
	bool primed;
	uint32_t currentStart;
	uint32_t currentHash;
};

void lingoScannerReset(struct LingoScanner* s);

/* Call once per emulated frame. Looks for a pointer in RAM that is walking
 * through Japanese text one character at a time (the game's text printer)
 * and, when it finds a message that differs from the last one reported,
 * decodes it into out and returns true. */
bool lingoScannerStep(struct LingoScanner* s, const struct LingoMem* mem, struct LingoText* out);

/* Decode the message containing addr: walks back to its start, then
 * forward to its terminator. Returns false if it is not valid text. */
bool lingoExtractMessage(const struct LingoMem* mem, uint32_t addr, uint32_t knownStart, struct LingoText* out);

/* ---- Dictionary ---- */

enum LingoPos {
	LPOS_V1 = 1 << 0,     /* ichidan verb */
	LPOS_V5 = 1 << 1,     /* godan verb */
	LPOS_ADJI = 1 << 2,   /* i-adjective */
	LPOS_VK = 1 << 3,     /* kuru */
	LPOS_VS = 1 << 4,     /* suru */
	LPOS_NOUN = 1 << 5,
	LPOS_PRT = 1 << 6,    /* particle */
	LPOS_EXP = 1 << 7,    /* expression */
	LPOS_ADV = 1 << 8,
	LPOS_ADJNA = 1 << 9,
	LPOS_OTHER = 1 << 10,
};

struct LingoEntry {
	const char* headword; /* kanji form if any, else reading (UTF-8) */
	const char* reading;  /* hiragana/katakana (UTF-8) */
	const char* gloss;    /* "[noun] 1. ...; ... 2. ..." (UTF-8) */
	uint16_t pos;
	uint16_t rank;        /* lower is more common */
};

struct LingoDict;

/* Loads a dictionary built by tools/build_dict.py. Returns NULL on failure. */
struct LingoDict* lingoDictLoad(const char* path);
struct LingoDict* lingoDictFromMemory(uint8_t* data, size_t size, bool owned);
void lingoDictFree(struct LingoDict* d);
size_t lingoDictEntryCount(const struct LingoDict* d);

/* Exact lookup of a hiragana key (UTF-32). Fills up to max entry indices,
 * best first. Returns how many. */
int lingoDictLookup(const struct LingoDict* d, const uint32_t* key, int keyLen, uint32_t* ids, int max);
bool lingoDictEntry(const struct LingoDict* d, uint32_t id, struct LingoEntry* e);

/* ---- Tokenizer (longest match + deinflection) ---- */

#define LINGO_MAX_IDS 4
#define LINGO_MAX_REASON 48

struct LingoToken {
	int start; /* index into LingoText.cp */
	int len;
	int nIds;  /* 0 when the span is unknown or punctuation */
	uint32_t ids[LINGO_MAX_IDS];
	char reason[LINGO_MAX_IDS][LINGO_MAX_REASON]; /* per id, e.g. "polite past" */
	bool isWord;
};

int lingoTokenize(const struct LingoDict* d, const uint32_t* cp, int len, struct LingoToken* tokens, int maxTokens);

/* ---- Utilities ---- */
int lingoUtf8Encode(uint32_t cp, char* out);
int lingoUtf8FromCodepoints(const uint32_t* cp, int len, char* out, size_t outSize);
int lingoUtf8Decode(const char* s, uint32_t* out, int outMax);

#endif
