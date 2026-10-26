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
/* Every frame. True with out filled when a new line has appeared. */
bool lingoTextStep(struct LingoText* out);
/* One line for the panel, and progress 0..1 while learning. */
const char* lingoTextStatus(float* progress, bool* reading);
/* The teach screen (the game waits). */
void lingoTextTeachRun(void);
/* Throw away this game's table and learn again. */
void lingoTextForget(void);

#endif
