/* Lingo libretro front end: audio resampling and save watching. */
#include "retro-util.h"

#include <string.h>

void raInit(struct RetroAudio* a, double inRate, double outRate) {
	memset(a, 0, sizeof(*a));
	a->ratio = inRate > 0 ? outRate / inRate : 1;
}

size_t raFill(const struct RetroAudio* a) {
	return a->head - a->tail;
}

void raPush(struct RetroAudio* a, const int16_t* frames, size_t count) {
	/* aim for a third of the ring; drift at most half a percent either way */
	double fill = (double) raFill(a) / RA_RING;
	double adjust = 1.0 + (1.0 / 3.0 - fill) * 0.015;
	if (adjust < 0.995) {
		adjust = 0.995;
	}
	if (adjust > 1.005) {
		adjust = 1.005;
	}
	double step = 1.0 / (a->ratio * adjust); /* input frames per output frame */
	for (size_t i = 0; i < count; ++i) {
		const int16_t* cur = frames + i * 2;
		while (a->pos < 1.0) {
			if (raFill(a) >= RA_RING - 1) {
				a->pos += step; /* full: drop rather than block the game */
				continue;
			}
			uint32_t h = a->head % RA_RING;
			a->ring[h * 2] = (int16_t) (a->prev[0] + (cur[0] - a->prev[0]) * a->pos);
			a->ring[h * 2 + 1] = (int16_t) (a->prev[1] + (cur[1] - a->prev[1]) * a->pos);
			a->head++;
			a->pos += step;
		}
		a->pos -= 1.0;
		a->prev[0] = cur[0];
		a->prev[1] = cur[1];
	}
}

size_t raPull(struct RetroAudio* a, int16_t* out, size_t n) {
	size_t got = 0;
	while (got < n && a->tail != a->head) {
		uint32_t t = a->tail % RA_RING;
		out[got * 2] = a->ring[t * 2];
		out[got * 2 + 1] = a->ring[t * 2 + 1];
		a->tail++;
		++got;
	}
	if (got < n) {
		memset(out + got * 2, 0, (n - got) * 2 * sizeof(int16_t));
	}
	return got;
}

static uint32_t fnv(const uint8_t* d, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; ++i) {
		h = (h ^ d[i]) * 16777619u;
	}
	return h;
}

bool swCheck(struct SaveWatch* w, const uint8_t* data, size_t size) {
	if (!data || !size) {
		return false;
	}
	uint32_t h = fnv(data, size);
	if (!w->primed) {
		w->hash = h;
		w->primed = true;
		return false;
	}
	if (h != w->hash) {
		w->hash = h;
		w->dirty = true;
		w->stable = 0;
		return false;
	}
	if (w->dirty && ++w->stable >= 2) {
		w->dirty = false;
		return true;
	}
	return false;
}
