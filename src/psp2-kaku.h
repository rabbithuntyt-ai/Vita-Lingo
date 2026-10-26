/* Kaku for Vita: kana and kanji writing practice.
 *
 * Stroke matching is a C port of Kaku's matcher, itself adapted from the
 * approach used by Hanzi Writer (MIT). Coordinates are KanjiVG units: a
 * 109 x 109 box. No Vita dependencies, so it is unit tested on a PC.
 */
#ifndef PSP2_KAKU_H
#define PSP2_KAKU_H

#include <stdbool.h>
#include <stdint.h>

struct KakuStroke {
	int first; /* index into KAKU_POINTS, in points */
	int count;
};

struct KakuKana {
	const char* ch;
	const char* romaji;
	const char* lookalikes; /* visually close characters, for quiz choices */
	int firstStroke;
	int nStrokes;
};

struct KakuKanji {
	const char* ch;
	const char* meaning;
	const char* on;  /* katakana readings, 、-separated */
	const char* kun; /* hiragana readings, 、-separated */
	int group;
	int firstStroke;
	int nStrokes;
};

extern const float KAKU_POINTS[];
extern const struct KakuStroke KAKU_STROKES[];
extern const struct KakuKana KAKU_KANA[];
extern const int KAKU_KANA_COUNT;
extern const struct KakuKanji KAKU_KANJI[];
extern const int KAKU_KANJI_COUNT;
extern const char* const KAKU_GROUPS[];
extern const int KAKU_GROUP_COUNT;

struct KakuPt {
	float x, y;
};

/* A character prepared for matching. */
struct KakuChar;

struct KakuChar* kakuCharLoad(int firstStroke, int nStrokes);
void kakuCharFree(struct KakuChar* c);
int kakuCharStrokes(const struct KakuChar* c);
/* The reference polyline of stroke i, for drawing. */
const struct KakuPt* kakuCharStroke(const struct KakuChar* c, int i, int* count);

struct KakuResult {
	bool isMatch;
	bool backwards;  /* drawn in the wrong direction */
	bool tap;        /* too short to judge */
	int wrongOrder;  /* index of a later stroke it matched instead, or -1 */
};

#define KAKU_LENIENCY_RELAXED 1.55f
#define KAKU_LENIENCY_NORMAL 1.25f
#define KAKU_LENIENCY_STRICT 1.0f

/* Compares a drawn stroke (KanjiVG units) with stroke idx of c. */
struct KakuResult kakuCheck(struct KakuChar* c, const struct KakuPt* pts, int n, int idx, float leniency, bool outline);

/* ---- Review scheduling (shared by kana, kanji and saved words) ---- */

struct KakuCard {
	uint32_t due;      /* day number */
	uint16_t interval; /* days */
	uint16_t ease;     /* x100 */
	uint16_t reps;
	uint16_t lapses;
};

enum KakuGrade { KAKU_AGAIN, KAKU_HARD, KAKU_GOOD, KAKU_EASY };

void kakuCardInit(struct KakuCard* c, uint32_t today);
void kakuCardGrade(struct KakuCard* c, enum KakuGrade g, uint32_t today);
/* Days until the next review if g were chosen, for button labels. */
int kakuCardPreview(const struct KakuCard* c, enum KakuGrade g);

#endif
