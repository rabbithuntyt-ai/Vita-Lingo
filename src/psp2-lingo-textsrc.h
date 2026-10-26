/* Lingo: the study panel's text source for any game (tables, detection, teaching). */
#ifndef PSP2_LINGO_TEXTSRC_H
#define PSP2_LINGO_TEXTSRC_H

#include "psp2-lingo-reader.h"

/* A game started: loads its saved table, a built-in one, or starts learning.
 * key names the game (used for its table file); baseName is the file name. */
void lingoTextSetGame(const char* system, const char* key, const char* baseName, const uint8_t* rom, size_t romSize);
void lingoTextGameUnloaded(void);
/* Every frame, before lingoTextStep: the memory to read this frame. */
void lingoTextSetRegions(const struct LingoRegions* rs);
/* Every frame: the picture on screen, for reading text off it (format as in
 * psp2-lingo-shot.h). The pixels must stay valid until the next call. */
void lingoTextSetFrame(const void* pixels, int w, int h, int stride, int format);
/* Every frame. True with out filled when a new line has appeared. */
bool lingoTextStep(struct LingoText* out);
/* One line for the panel, and progress 0..1 while learning. */
const char* lingoTextStatus(float* progress, bool* reading);
/* The teach screen (the game waits). */
void lingoTextTeachRun(void);
/* Skip text from around where this line came from (a LingoText.start). */
void lingoTextIgnore(uint32_t start);
/* The player corrected a character: this game's code now means cp. */
bool lingoTextFix(uint16_t code, uint32_t cp);
/* Throw away this game's table and learn again. */
void lingoTextForget(void);

#endif
