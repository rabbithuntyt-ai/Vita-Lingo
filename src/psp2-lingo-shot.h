/* Lingo: one screenshot per game, used as its picture when there is no box art. */
#ifndef PSP2_LINGO_SHOT_H
#define PSP2_LINGO_SHOT_H

#include <stdbool.h>

enum LingoShotFormat {
	LSF_RGBX8888, /* bytes R G B X */
	LSF_BGRX8888, /* bytes B G R X (libretro XRGB8888) */
	LSF_RGB565,
	LSF_XRGB1555,
};

/* A game started from this file. */
void lingoShotGame(const char* fileName);
/* Every frame of play; saves the picture once, after a minute. */
void lingoShotFrame(const void* pixels, unsigned w, unsigned h, unsigned stride, enum LingoShotFormat f);
/* The saved picture for a game file, or NULL. */
struct vita2d_texture* lingoShotTexture(const char* fileName);

#endif
