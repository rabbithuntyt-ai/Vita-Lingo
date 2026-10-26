/* Lingo / Kaku shared UI layer for the Vita: chalkboard style, fonts,
 * input, the review store and the saved-word deck. */
#ifndef PSP2_LINGO_UI_H
#define PSP2_LINGO_UI_H

#include "psp2-kaku.h"
#include "psp2-lingo.h"

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Kaku's blackboard palette (ABGR as vita2d expects) */
#define RGBA(r, g, b, a) ((uint32_t) (((a) << 24) | ((b) << 16) | ((g) << 8) | (r)))
#define C_BOARD RGBA(0x1a, 0x2e, 0x25, 0xff)
#define C_SHEET RGBA(0x20, 0x38, 0x2d, 0xff)
#define C_SHEET_HI RGBA(0x29, 0x45, 0x38, 0xff)
#define C_LINE RGBA(0x4f, 0x7a, 0x67, 0xff)
#define C_LINE_SOFT RGBA(0x32, 0x52, 0x46, 0xff)
#define C_CHALK RGBA(0xee, 0xf1, 0xe6, 0xff)
#define C_SOFT RGBA(0x9d, 0xb2, 0xa4, 0xff)
#define C_MUTED RGBA(0xad, 0xbd, 0xb2, 0xff)
#define C_YELLOW RGBA(0xf1, 0xd3, 0x7c, 0xff)
#define C_RED RGBA(0xff, 0x8e, 0x7c, 0xff)
#define C_HANAMARU RGBA(0xe8, 0x4a, 0x3c, 0xff)
#define C_GREEN RGBA(0xa6, 0xe3, 0xbd, 0xff)
#define C_PINK RGBA(0xf4, 0xa6, 0xc0, 0xff)
#define C_BLUE RGBA(0x9c, 0xc8, 0xf0, 0xff)
#define C_SHADE RGBA(0x0c, 0x16, 0x12, 0xb0)

uint32_t uiAlpha(uint32_t c, float a);

/* ---- lifecycle ---- */
void uiInit(void);
void uiDeinit(void);
/* Runs one frame of an owned loop: start drawing, clear, (caller draws), end. */
void uiFrameBegin(void);
void uiFrameEnd(void);
double uiTime(void); /* seconds */

/* ---- drawing ---- */
void uiRect(float x, float y, float w, float h, uint32_t c);
void uiRound(float x, float y, float w, float h, float r, uint32_t c);
void uiRoundOutline(float x, float y, float w, float h, float r, float t, uint32_t c);
void uiCircle(float x, float y, float r, uint32_t c);
void uiThickLine(const struct KakuPt* p, int n, float scale, float ox, float oy, float width, uint32_t c);
void uiSegment(float x0, float y0, float x1, float y1, float width, uint32_t c);
void uiChalkBackground(void);
void uiDust(float x, float y, uint32_t c, int count);
void uiDustUpdate(float dt);
void uiDustDraw(void);

/* UI text in the system font; y is the baseline; px is the approximate cap height in pixels. */
int uiText(float x, float y, float px, uint32_t c, const char* s);
int uiTextW(float px, const char* s);
void uiTextCentered(float cx, float y, float px, uint32_t c, const char* s);
/* Wraps on spaces (or any character for Japanese). Returns the height used. */
int uiTextWrap(float x, float y, float w, float px, float lineH, uint32_t c, const char* s, int maxLines);
/* Japanese display text in the handwriting font when it is available. */
int uiJp(float x, float y, float px, uint32_t c, const char* s);
int uiJpW(float px, const char* s);
void uiJpCentered(float cx, float y, float px, uint32_t c, const char* s);

/* ---- input ---- */
struct UiInput {
	bool down, pressed, released;
	float x, y, sx, sy; /* current and start of touch */
	uint32_t buttons, buttonsPressed;
};
extern struct UiInput IN;
void uiPollInput(void);
bool uiTapped(float x, float y, float w, float h);   /* released inside, started inside */
bool uiHeld(float x, float y, float w, float h);
bool uiBack(void);   /* circle */
bool uiOk(void);     /* cross */

/* A chunky button; returns true when tapped. focus draws a highlight. */
bool uiButton(float x, float y, float w, float h, const char* label, const char* jp, uint32_t accent, bool focus);

/* ---- persistence ---- */
uint32_t uiToday(void);
struct KakuCard* storeGet(const char* key, bool create);
int storeDueCount(const char* prefix, uint32_t today);
void storeSave(void);

struct SavedWord {
	char head[64];
	char reading[64];
	char gloss[192];
	char context[256];
};
int deckCount(void);
const struct SavedWord* deckGet(int i);
bool deckHas(const char* head, const char* reading);
bool deckAdd(const struct SavedWord* w);
void deckLoad(void);

/* ---- screens ---- */
/* Title screen. Returns true with path set when the player picks a Game Boy
 * or GBA game for mGBA; other consoles launch their own core from here. */
bool lingoLauncherRun(char* path, size_t pathSize);
/* Receive games from a phone (QR code + local upload page). */
void lingoUploadRun(void);
/* Kaku practice. Returns when the player backs out. */
void kakuRun(void);
/* Saved-word review, also reachable from Kaku. */
void kakuReviewWords(void);
/* Kaku's classes and units. */
void kakuLessonsRun(void);
/* The starter-games shelf. */
void lingoGamesRun(void);

/* The in-game study panel. */
void lingoPanelInit(void);
void lingoPanelGameLoaded(const char* gameCode, const char* title);
void lingoPanelGameUnloaded(void);
/* Every frame of play: reads text (GBA), times play and watches the save
 * flag (any system) so checkpoints know when the player has saved. */
void lingoPanelFrame(const struct LingoMem* mem, int saveDirty);
/* The shared dictionary (loaded on first use), or NULL. */
const struct LingoDict* lingoDictGet(void);
/* A word looked up by hand counts as met in the current game. */
void lingoNoteLookup(const uint32_t* cp, int len, const struct LingoToken* tok);
/* Type-what-you-see dictionary lookup, for any game. */
void lingoLookupRun(void);
int lingoRecentLookups(const char** words, const char** meanings, int max);

/* Side panel for consoles without live text reading. */
enum LingoPanelAction { LPA_NONE, LPA_KAKU, LPA_LOOKUP, LPA_MENU };
enum LingoPanelAction lingoRetroPanelDraw(float gx, float gy, float gw, float gh, const char* system, const char* game);

/* True when a checkpoint test is owed; run it with lingoCheckpointRun. */
bool lingoCheckpointDue(void);
void lingoCheckpointRun(void);
bool lingoCheckpointsEnabled(void);
void lingoSetCheckpointsEnabled(bool on);
/* Draws the panel around a game picture drawn at (gx, gy, gw, gh). Returns
 * true if the player asked for Kaku practice. */
bool lingoPanelDraw(float gx, float gy, float gw, float gh);
/* Set when the GBA panel's Look up button was tapped (cleared on read). */
bool lingoPanelWantsLookup(void);

#endif
