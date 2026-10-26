/* Lingo libretro front end: keeping the game's recent audio, for replaying
 * voiced lines (normal or slowed without lowering the pitch) and for
 * speech recognition. No Vita dependencies; tested by tests/test_voice.c. */
#ifndef RETRO_VOICE_H
#define RETRO_VOICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VC_SECONDS 30

struct VoiceCapture {
	int16_t* ring; /* mono, at the core's rate */
	size_t cap;
	uint64_t written; /* total samples ever written */
	double rate;
};

bool vcInit(struct VoiceCapture* v, double rate);
void vcFree(struct VoiceCapture* v);
/* Stereo frames from the core. */
void vcPush(struct VoiceCapture* v, const int16_t* stereo, size_t frames);
/* Copies samples [from, to) (absolute sample numbers, clipped to what is
 * still in the ring) into out, trimming silence at both ends. Returns the
 * count, and *start gets the absolute sample number of out[0]. */
size_t vcCopy(const struct VoiceCapture* v, uint64_t from, uint64_t to, int16_t* out, size_t max, uint64_t* start);

/* Resample mono in (inRate) to mono out (outRate), linear with a light low-pass. */
size_t vcResample(const int16_t* in, size_t n, double inRate, int16_t* out, size_t max, double outRate);
/* Slows speech to 1/speed of its pace at the same pitch (WSOLA). speed in (0.4, 1]. */
size_t vcStretch(const int16_t* in, size_t n, double rate, double speed, int16_t* out, size_t max);

#endif
