/* Lingo libretro front end: recent audio for voiced lines (see retro-voice.h). */
#include "retro-voice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool vcInit(struct VoiceCapture* v, double rate) {
	memset(v, 0, sizeof(*v));
	v->rate = rate > 0 ? rate : 44100;
	v->cap = (size_t) (v->rate * VC_SECONDS);
	v->ring = calloc(v->cap, sizeof(int16_t));
	return v->ring != NULL;
}

void vcFree(struct VoiceCapture* v) {
	free(v->ring);
	memset(v, 0, sizeof(*v));
}

void vcPush(struct VoiceCapture* v, const int16_t* stereo, size_t frames) {
	if (!v->ring) {
		return;
	}
	for (size_t i = 0; i < frames; ++i) {
		v->ring[v->written % v->cap] = (int16_t) (((int) stereo[i * 2] + stereo[i * 2 + 1]) / 2);
		v->written++;
	}
}

size_t vcCopy(const struct VoiceCapture* v, uint64_t from, uint64_t to, int16_t* out, size_t max, uint64_t* start) {
	*start = from;
	if (!v->ring || to <= from) {
		return 0;
	}
	uint64_t oldest = v->written > v->cap ? v->written - v->cap : 0;
	if (from < oldest) {
		from = oldest;
	}
	if (to > v->written) {
		to = v->written;
	}
	if (to <= from) {
		return 0;
	}
	/* trim silence: windows of 20 ms below a quiet level */
	size_t win = (size_t) (v->rate / 50);
	const int quiet = 400;
	uint64_t a = from, b = to;
	while (a + win < b) {
		int peak = 0;
		for (size_t k = 0; k < win; ++k) {
			int s = abs(v->ring[(a + k) % v->cap]);
			peak = s > peak ? s : peak;
		}
		if (peak > quiet) {
			break;
		}
		a += win;
	}
	while (b > a + win) {
		int peak = 0;
		for (size_t k = 1; k <= win; ++k) {
			int s = abs(v->ring[(b - k) % v->cap]);
			peak = s > peak ? s : peak;
		}
		if (peak > quiet) {
			break;
		}
		b -= win;
	}
	/* keep a little air either side */
	a = a > from + win ? a - win : from;
	b = b + win < to ? b + win : to;
	size_t n = (size_t) (b - a);
	if (n > max) {
		a = b - max; /* the end matters most: it's what was just said */
		n = max;
	}
	for (size_t i = 0; i < n; ++i) {
		out[i] = v->ring[(a + i) % v->cap];
	}
	*start = a;
	return n;
}

size_t vcResample(const int16_t* in, size_t n, double inRate, int16_t* out, size_t max, double outRate) {
	if (!n || inRate <= 0 || outRate <= 0) {
		return 0;
	}
	double step = inRate / outRate;
	/* a box average over the step when shrinking, so high tones don't fold down */
	int box = step > 1.5 ? (int) step : 1;
	size_t o = 0;
	for (double pos = 0; pos < n - 1 && o < max; pos += step) {
		size_t i = (size_t) pos;
		double f = pos - i;
		double s;
		if (box > 1) {
			long acc = 0;
			int c = 0;
			for (int k = 0; k < box && i + k < n; ++k) {
				acc += in[i + k];
				++c;
			}
			s = (double) acc / c;
		} else {
			s = in[i] + (in[i + 1] - in[i]) * f;
		}
		out[o++] = (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
	return o;
}

size_t vcStretch(const int16_t* in, size_t n, double rate, double speed, int16_t* out, size_t max) {
	if (speed >= 0.999) {
		size_t k = n < max ? n : max;
		memcpy(out, in, k * sizeof(int16_t));
		return k;
	}
	if (speed < 0.4) {
		speed = 0.4;
	}
	/* WSOLA: 40 ms Hann windows, output hop of half a window; each input
	 * frame is taken near pos (advancing slower than the output) where it
	 * best continues the last one, searched within ±10 ms */
	size_t W = (size_t) (rate * 0.040);
	if (W < 64) {
		W = 64;
	}
	size_t H = W / 2;
	size_t search = (size_t) (rate * 0.010);
	size_t outLen = (size_t) (n / speed) + W;
	if (outLen > max) {
		outLen = max;
	}
	float* acc = calloc(outLen + W, sizeof(float));
	float* win = malloc(W * sizeof(float));
	if (!acc || !win) {
		free(acc);
		free(win);
		return 0;
	}
	for (size_t i = 0; i < W; ++i) {
		win[i] = 0.5f - 0.5f * cosf(2 * (float) M_PI * i / (W - 1));
	}
	size_t prevStart = 0;
	bool first = true;
	size_t o = 0;
	for (size_t k = 0; o + W <= outLen; ++k, o += H) {
		double pos = k * H * speed;
		size_t want = (size_t) pos;
		if (want + W >= n) {
			break;
		}
		size_t best = want;
		if (!first) {
			/* the natural continuation of the last frame */
			size_t nat = prevStart + H;
			double bestScore = -1e30;
			size_t lo = want > search ? want - search : 0;
			size_t hi = want + search + W < n ? want + search : n - W - 1;
			for (size_t c = lo; c <= hi; c += 2) {
				double sc = 0;
				for (size_t t = 0; t < H && nat + t < n && c + t < n; t += 2) {
					sc += (double) in[nat + t] * in[c + t];
				}
				if (sc > bestScore) {
					bestScore = sc;
					best = c;
				}
			}
		}
		for (size_t t = 0; t < W; ++t) {
			acc[o + t] += in[best + t] * win[t];
		}
		prevStart = best;
		first = false;
	}
	size_t total = o + H < outLen ? o + H : outLen;
	for (size_t i = 0; i < total; ++i) {
		float s = acc[i];
		out[i] = (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
	free(acc);
	free(win);
	return total;
}
