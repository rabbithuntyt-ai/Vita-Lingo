/* Lingo libretro front end: platform-independent helpers. */
#ifndef RETRO_UTIL_H
#define RETRO_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- audio: stereo int16 from the core's rate to the Vita's 48 kHz ---- */

#define RA_RING (48000 / 2) /* half a second of stereo frames */

struct RetroAudio {
	int16_t ring[RA_RING * 2];
	volatile uint32_t head, tail; /* frames written / read */
	double ratio;  /* output frames per input frame */
	double pos;    /* fractional position between the last two input frames */
	int16_t prev[2];
};

void raInit(struct RetroAudio* a, double inRate, double outRate);
/* Main thread: push core samples. Drift is corrected by nudging the ratio
 * towards keeping the ring about a third full. */
void raPush(struct RetroAudio* a, const int16_t* frames, size_t count);
/* Audio thread: pull n frames; silence fills any shortfall. Returns frames that were real. */
size_t raPull(struct RetroAudio* a, int16_t* out, size_t n);
size_t raFill(const struct RetroAudio* a);

/* ---- saves: notice when the game has finished writing its save ---- */

struct SaveWatch {
	uint32_t hash;
	int stable;   /* checks since the last change */
	bool dirty;   /* changed and not yet written to disk */
	bool primed;
};

/* Call about once a second with the core's save RAM. Returns true when a
 * change has settled and should be written to disk (a finished save). */
bool swCheck(struct SaveWatch* w, const uint8_t* data, size_t size);

#endif
