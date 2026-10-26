/* Lingo: reading text off the screen, for games whose text can't be found in
 * memory (drawn as pictures, compressed, or an unusual encoding).
 *
 * 1. Find text: rows of small, high-contrast shapes on a flat background
 *    (a text box), split into evenly spaced letter cells.
 * 2. Recognise each cell against reference glyphs rendered from open fonts
 *    at retro sizes (glyphs.bin, see tools/make_glyphs.py).
 * 3. Settle doubtful letters with Japanese kana statistics.
 * 4. Remember the game's own letters: once a cell's exact pixels have been
 *    read confidently (or taught), they are known for good.
 *
 * No Vita dependencies; tested by tests/test_ocr.c. */
#ifndef PSP2_LINGO_OCR_H
#define PSP2_LINGO_OCR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OCR_MAX_CELLS 64

struct LingoOcrLine {
	uint32_t cp[OCR_MAX_CELLS];
	int len;
	int x, y, w, h;   /* where on screen */
	int pitch;
	int gridX;        /* where the letter grid starts */
	int16_t cellX[OCR_MAX_CELLS]; /* each letter's cell (for teaching), by index in cp */
	uint8_t cellW[OCR_MAX_CELLS];
	float conf;       /* mean match score of the letters, 0..1 */
	uint32_t hash;    /* of the cells' pixels: changes when the text does */
	bool accepted;    /* reads like Japanese */
};

struct LingoOcr;

struct LingoOcr* lingoOcrLoad(const char* glyphsPath);
struct LingoOcr* lingoOcrFromMemory(const uint8_t* data, size_t size);
void lingoOcrFree(struct LingoOcr* o);
int lingoOcrTemplates(const struct LingoOcr* o);

/* The game's learned letters live in this file (loaded now, saved on change). */
void lingoOcrSetCache(struct LingoOcr* o, const char* path);
bool lingoOcrSaveCache(struct LingoOcr* o);
int lingoOcrLearned(const struct LingoOcr* o);

/* Reads every text line in a frame (8-bit brightness, w x h). Returns the
 * number of lines written to out; only accepted lines teach the cache. */
int lingoOcrRead(struct LingoOcr* o, const uint8_t* luma, int w, int h, struct LingoOcrLine* out, int max);

/* The player typed a line that is on screen: label its letters. */
bool lingoOcrTeach(struct LingoOcr* o, const uint8_t* luma, int w, int h, const uint32_t* typed, int n, char* why,
                   size_t whyLen);

/* Optional: how much of a line real words cover (higher is better). With it,
 * doubtful letters are settled by whichever reading makes real words. */
typedef int (*LingoOcrScorer)(const uint32_t* cp, int n, void* user);
void lingoOcrSetScorer(struct LingoOcr* o, LingoOcrScorer scorer, void* user);

/* Brightness from a frame in one of the shot formats (see psp2-lingo-shot.h:
 * 0 RGBX8888 bytes, 1 BGRX8888, 2 RGB565, 3 XRGB1555). */
void lingoOcrLuma(const void* pixels, int w, int h, int stride, int format, uint8_t* out);

#endif
