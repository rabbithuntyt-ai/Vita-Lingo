/* Kaku lessons: grammar notes and drills by unit, and catch-up classes. */
#ifndef PSP2_KAKU_LESSONS_H
#define PSP2_KAKU_LESSONS_H

#include <stddef.h>

struct KakuNote {
	const char* title;
	const char* body;
	const char* const* ex; /* pairs: Japanese, English */
	int nEx;
};

struct KakuDrill {
	const char* q;
	const char* ctx;
	const char* answer;
	const char* wrong[3];
	const char* wrongWhy[3];
	int nWrong;
	const char* why;
};

struct KakuUnit {
	const char* topic;
	const char* kanji;
	const struct KakuNote* notes;
	int nNotes;
	const struct KakuDrill* drills;
	int nDrills;
};

struct KakuCheck {
	const char* q;
	const char* o[4];
	int nO;
	int answer;
	const char* why;
};

struct KakuClass {
	const char* title;
	const char* idea;
	const char* const* body;
	int nBody;
	const char* const* shape; /* pairs: pattern, meaning */
	int nShape;
	const char* const* ex;    /* triples: Japanese, English, note */
	int nEx;
	const char* const* slips; /* triples: wrong, right, why */
	int nSlips;
	const struct KakuCheck* checks;
	int nChecks;
};

extern const struct KakuUnit KAKU_UNITS[];
extern const int KAKU_UNIT_COUNT;
extern const struct KakuClass KAKU_CLASSES[];
extern const int KAKU_CLASS_COUNT;

#endif
