/* Host tests for the front end's audio resampler and save watcher. */
#include "../retro/retro-util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void) {
	/* SNES-like 32040 Hz in, 48000 out: one second of a 440 Hz tone */
	static struct RetroAudio a;
	raInit(&a, 32040, 48000);
	static int16_t in[32040 * 2], out[48000 * 2];
	for (int i = 0; i < 32040; ++i) in[i * 2] = in[i * 2 + 1] = (int16_t) (8000 * sin(2 * M_PI * 440 * i / 32040.0));
	size_t pulled = 0;
	/* feed per 60 Hz frame (534 frames) and pull per 1024 like the audio thread */
	int16_t buf[1024 * 2];
	for (int f = 0; f < 60; ++f) {
		raPush(&a, in + f * 534 * 2, 534);
		while (raFill(&a) >= 1024 && pulled + 1024 <= 48000) { raPull(&a, buf, 1024); memcpy(out + pulled * 2, buf, sizeof buf); pulled += 1024; }
	}
	printf("pulled %zu frames, fill %zu\n", pulled, raFill(&a));
	CHECK(pulled > 44000 && pulled <= 48000, "rate conversion off: %zu", pulled);
	/* the tone survives: count zero crossings in 0.5 s of output */
	int cross = 0;
	for (int i = 1; i < 24000; ++i) if ((out[(i - 1) * 2] < 0) != (out[i * 2] < 0)) ++cross;
	printf("zero crossings in 0.5s: %d (want ~440)\n", cross);
	CHECK(abs(cross - 440) < 15, "pitch wrong: %d crossings", cross);
	/* underrun gives silence, not garbage */
	static struct RetroAudio b; raInit(&b, 44100, 48000);
	memset(buf, 0x55, sizeof buf);
	CHECK(raPull(&b, buf, 1024) == 0 && buf[0] == 0 && buf[2047] == 0, "underrun not silent");
	/* overflow drops instead of wrapping */
	static int16_t big[44100 * 2 * 2];
	raPush(&b, big, 44100 * 2);
	CHECK(raFill(&b) < RA_RING, "ring overflowed: %zu", raFill(&b));

	/* save watcher */
	struct SaveWatch w; memset(&w, 0, sizeof w);
	uint8_t sram[8192] = { 0 };
	CHECK(!swCheck(&w, sram, sizeof sram), "first look is not a save");
	CHECK(!swCheck(&w, sram, sizeof sram), "nothing changed");
	sram[100] = 1; CHECK(!swCheck(&w, sram, sizeof sram), "mid-write");
	sram[200] = 2; CHECK(!swCheck(&w, sram, sizeof sram), "still writing");
	CHECK(!swCheck(&w, sram, sizeof sram), "settling");
	CHECK(swCheck(&w, sram, sizeof sram), "save should be reported once settled");
	CHECK(!swCheck(&w, sram, sizeof sram), "reported twice");
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
