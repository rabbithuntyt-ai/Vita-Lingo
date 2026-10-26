/* Lingo checkpoints: the words a player meets in each game, when a
 * checkpoint is due, and building the test that has to be passed. */
#ifndef PSP2_LINGO_QUIZ_H
#define PSP2_LINGO_QUIZ_H

#include "psp2-lingo.h"

#define LINGO_CTX_BYTES 200

struct LingoMet {
	uint32_t id;       /* dictionary entry */
	char surface[32];  /* how it appeared (UTF-8), e.g. たべなかった */
	char ctx[LINGO_CTX_BYTES]; /* the line it came from */
	uint16_t seen;
	uint8_t right, wrong;
	uint8_t fresh;     /* met since the last checkpoint */
	uint8_t pad[3];
};

struct LingoBook {
	struct LingoMet* items;
	int n, cap;
	/* checkpoint state */
	float playSince;   /* seconds played since the last checkpoint */
	int wordsSince;    /* new words since the last checkpoint */
	int passed;        /* checkpoints passed in this game */
	bool pending;      /* a test is owed */
};

void lingoBookInit(struct LingoBook* b);
void lingoBookFree(struct LingoBook* b);
bool lingoBookLoad(struct LingoBook* b, const char* path);
bool lingoBookSave(const struct LingoBook* b, const char* path);

/* Records the words of a tokenized line. Particles and one-kana words are
 * skipped: they are not what a checkpoint should be about. Returns how
 * many were new. */
int lingoBookMeet(struct LingoBook* b, const struct LingoDict* d, const struct LingoText* t,
                  const struct LingoToken* tok, int nTok);

/* Checkpoint rules. Call tick every frame of play and saved when the game
 * finishes writing its save. */
#define LINGO_CP_MIN_PLAY (20 * 60)
#define LINGO_CP_MIN_WORDS 15
#define LINGO_CP_FORCE_PLAY (60 * 60)
void lingoCheckpointTick(struct LingoBook* b, float dt);
void lingoCheckpointSaved(struct LingoBook* b);
void lingoCheckpointPassed(struct LingoBook* b);

/* ---- the test ---- */

enum LingoQKind { LQ_MEANING, LQ_READING, LQ_CLOZE };

struct LingoQuestion {
	enum LingoQKind kind;
	int met;              /* index into the book, or -1 for a saved word */
	char prompt[160];     /* the word, or the line with a gap */
	char sub[128];        /* reading, or a hint */
	char context[LINGO_CTX_BYTES];
	char options[4][128];
	int answer;
	char word[64], reading[64], meaning[128]; /* for the review after a miss */
};

/* Words the player starred, offered alongside the ones met in the game. */
struct LingoExtra {
	const char* head;
	const char* reading;
	const char* gloss;
	const char* ctx;
};

/* Builds up to max questions, newest words first. Returns how many (0 if
 * there is not enough to ask about). seed varies the choice and order. */
int lingoBuildTest(const struct LingoDict* d, struct LingoBook* b, const struct LingoExtra* extra, int nExtra,
                   unsigned seed, struct LingoQuestion* out, int max);

/* "[noun] 1. the world; society  2. ..." -> "the world" */
void lingoShortGloss(const char* gloss, char* out, size_t n);

#endif
