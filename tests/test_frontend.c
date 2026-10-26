/* Host tests for the libretro front end's zip reader, core options, rewind
 * and voice capture. */
#include "../retro/retro-options.h"
#include "../retro/retro-rewind.h"
#include "../retro/retro-voice.h"
#include "../retro/retro-zip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int failures;
#define CHECK(c, ...)                      \
	do {                                   \
		if (!(c)) {                        \
			++failures;                    \
			printf("FAIL %d: ", __LINE__); \
			printf(__VA_ARGS__);           \
			printf("\n");                  \
		}                                  \
	} while (0)

/* a minimal zip writer for the test: one stored file and one deflated file */
static void put16(FILE* f, unsigned v) {
	fputc(v & 255, f);
	fputc((v >> 8) & 255, f);
}
static void put32(FILE* f, unsigned v) {
	put16(f, v & 0xFFFF);
	put16(f, v >> 16);
}

static void writeZip(const char* path, const char* n1, const uint8_t* d1, size_t l1, const char* n2, const uint8_t* d2, size_t l2) {
	FILE* f = fopen(path, "wb");
	uLongf cl = compressBound(l2);
	uint8_t* c = malloc(cl);
	compress2(c, &cl, d2, l2, 9); /* zlib stream: strip 2-byte header and 4-byte adler for raw deflate */
	const uint8_t* raw = c + 2;
	size_t rawLen = cl - 6;
	unsigned off1 = 0;
	unsigned crc1 = (unsigned) crc32(0, d1, (uInt) l1), crc2 = (unsigned) crc32(0, d2, (uInt) l2);
	put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
	put32(f, crc1); put32(f, (unsigned) l1); put32(f, (unsigned) l1); put16(f, (unsigned) strlen(n1)); put16(f, 0);
	fwrite(n1, 1, strlen(n1), f); fwrite(d1, 1, l1, f);
	unsigned off2 = (unsigned) ftell(f);
	put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, 8); put16(f, 0); put16(f, 0);
	put32(f, crc2); put32(f, (unsigned) rawLen); put32(f, (unsigned) l2); put16(f, (unsigned) strlen(n2)); put16(f, 0);
	fwrite(n2, 1, strlen(n2), f); fwrite(raw, 1, rawLen, f);
	unsigned cd = (unsigned) ftell(f);
	const char* names[2] = { n1, n2 };
	unsigned meth[2] = { 0, 8 }, crcs[2] = { crc1, crc2 }, cs[2] = { (unsigned) l1, (unsigned) rawLen }, us[2] = { (unsigned) l1, (unsigned) l2 },
	         offs[2] = { off1, off2 };
	for (int i = 0; i < 2; ++i) {
		put32(f, 0x02014b50); put16(f, 20); put16(f, 20); put16(f, 0); put16(f, meth[i]); put16(f, 0); put16(f, 0);
		put32(f, crcs[i]); put32(f, cs[i]); put32(f, us[i]); put16(f, (unsigned) strlen(names[i])); put16(f, 0); put16(f, 0);
		put16(f, 0); put16(f, 0); put32(f, 0); put32(f, offs[i]);
		fwrite(names[i], 1, strlen(names[i]), f);
	}
	unsigned cdEnd = (unsigned) ftell(f);
	put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, 2); put16(f, 2); put32(f, cdEnd - cd); put32(f, cd); put16(f, 0);
	fclose(f);
	free(c);
}

static void testZip(void) {
	uint8_t readme[] = "hello";
	static uint8_t rom[300000];
	for (size_t i = 0; i < sizeof(rom); ++i) {
		rom[i] = (uint8_t) ((i * 7) ^ (i >> 5));
	}
	writeZip("/tmp/lingo-test.zip", "readme.txt", readme, 5, "Game (Japan).sfc", rom, sizeof(rom));
	CHECK(retroZipIs("/tmp/lingo-test.zip"), "zip detected");
	uint8_t* data;
	size_t size;
	char name[200], why[200];
	bool ok = retroZipExtract("/tmp/lingo-test.zip", ".sfc.smc", &data, &size, name, sizeof(name), why, sizeof(why));
	CHECK(ok, "extract: %s", why);
	if (ok) {
		CHECK(size == sizeof(rom) && !memcmp(data, rom, size), "game bytes match");
		CHECK(!strcmp(name, "Game (Japan).sfc"), "picked the game, not the readme (%s)", name);
		free(data);
	}
	/* no extension match: the largest file */
	ok = retroZipExtract("/tmp/lingo-test.zip", ".nes", &data, &size, name, sizeof(name), why, sizeof(why));
	CHECK(ok && size == sizeof(rom), "largest file when no extension matches");
	free(ok ? data : NULL);
}

static void testOptions(void) {
	struct RetroOptions* o = malloc(sizeof(*o));
	roInit(o, "/tmp/lingo-test.opt");
	remove("/tmp/lingo-test.opt");
	struct retro_variable vars[] = {
		{ "snes_frameskip", "Frameskip; disabled|auto|manual" },
		{ "snes_region", "Region; auto|ntsc|pal" },
		{ NULL, NULL },
	};
	roSetVariables(o, vars);
	CHECK(o->n == 2 && !strcmp(roGet(o, "snes_frameskip"), "disabled"), "v0 parsed (n %d)", o->n);
	roStep(o, 0, 1);
	CHECK(!strcmp(roGet(o, "snes_frameskip"), "auto"), "stepped");
	roStep(o, 1, -1);
	CHECK(!strcmp(roGet(o, "snes_region"), "pal"), "stepped back wraps");
	/* reload */
	roSetVariables(o, vars);
	CHECK(!strcmp(roGet(o, "snes_frameskip"), "auto") && !strcmp(roGet(o, "snes_region"), "pal"), "saved values reload");
	struct retro_core_option_v2_definition defs[2];
	memset(defs, 0, sizeof(defs));
	defs[0].key = "pcsx_dither";
	defs[0].desc = "Dithering";
	defs[0].values[0].value = "disabled";
	defs[0].values[1].value = "enabled";
	defs[0].values[1].label = "On";
	defs[0].default_value = "enabled";
	struct retro_core_options_v2 v2 = { NULL, defs };
	roSetV2(o, &v2);
	CHECK(o->n == 1 && !strcmp(roGet(o, "pcsx_dither"), "enabled") && !strcmp(o->opt[0].labels[1], "On"), "v2 parsed");
	roFree(o);
	free(o);
}

static void testRewind(void) {
	const size_t N = 200000;
	uint8_t* s = malloc(N);
	uint8_t* out = malloc(N);
	uint8_t* hist[40];
	for (size_t i = 0; i < N; ++i) {
		s[i] = (uint8_t) i;
	}
	struct RetroRewind* r = rwNew(N, 4 * 1024 * 1024);
	for (int k = 0; k < 40; ++k) {
		/* a few bytes change each frame */
		for (int j = 0; j < 300; ++j) {
			s[(k * 977 + j * 131) % N] ^= (uint8_t) (k + j + 1);
		}
		hist[k] = malloc(N);
		memcpy(hist[k], s, N);
		rwPush(r, s);
	}
	CHECK(rwCount(r) == 39, "39 steps back (got %d)", rwCount(r));
	bool good = true;
	for (int k = 38; k >= 0; --k) {
		if (!rwPop(r, out) || memcmp(out, hist[k], N)) {
			good = false;
			printf("  mismatch at step %d\n", k);
			break;
		}
	}
	CHECK(good, "every step back restores the earlier state");
	CHECK(!rwPop(r, out), "empty at the start");
	/* a small budget keeps only the newest */
	rwFree(r);
	r = rwNew(N, N * 3 + 20000);
	for (int k = 0; k < 40; ++k) {
		rwPush(r, hist[k]);
	}
	CHECK(rwCount(r) > 0 && rwCount(r) < 39, "budget limits history (%d)", rwCount(r));
	if (rwPop(r, out)) {
		CHECK(!memcmp(out, hist[38], N), "newest step still right with a small budget");
	}
	rwFree(r);
	for (int k = 0; k < 40; ++k) {
		free(hist[k]);
	}
	free(s);
	free(out);
}

static double pitchHz(const int16_t* x, size_t n, double rate) {
	/* autocorrelation peak between 80 and 500 Hz */
	size_t lo = (size_t) (rate / 500), hi = (size_t) (rate / 80);
	double best = 0;
	size_t lag = lo;
	for (size_t l = lo; l <= hi; ++l) {
		double s = 0;
		for (size_t i = 0; i + l < n; ++i) {
			s += (double) x[i] * x[i + l];
		}
		if (s > best) {
			best = s;
			lag = l;
		}
	}
	return rate / lag;
}

static void testVoice(void) {
	struct VoiceCapture v;
	vcInit(&v, 32000);
	/* 2 s silence, 1.5 s of a 200 Hz voice-like tone, 1 s silence */
	static int16_t st[32000 * 5 * 2];
	size_t frames = 0;
	for (int i = 0; i < 32000 * 2; ++i, ++frames) {
		st[frames * 2] = st[frames * 2 + 1] = 0;
	}
	for (int i = 0; i < 48000; ++i, ++frames) {
		int16_t s = (int16_t) (6000 * sin(2 * M_PI * 200 * i / 32000.0) + 2000 * sin(2 * M_PI * 400 * i / 32000.0));
		st[frames * 2] = st[frames * 2 + 1] = s;
	}
	for (int i = 0; i < 32000; ++i, ++frames) {
		st[frames * 2] = st[frames * 2 + 1] = 0;
	}
	vcPush(&v, st, frames);
	static int16_t seg[32000 * 6];
	uint64_t start;
	size_t n = vcCopy(&v, 0, v.written, seg, sizeof(seg) / 2, &start);
	CHECK(n > 46000 && n < 52000, "silence trimmed (got %zu samples)", n);
	CHECK(start > 62000 && start < 64100, "starts at the voice (%llu)", (unsigned long long) start);
	static int16_t slow[32000 * 6];
	size_t m = vcStretch(seg, n, 32000, 0.7, slow, sizeof(slow) / 2);
	CHECK(m > n * 1.3 && m < n * 1.55, "slowed to about 1/0.7 the length (%zu -> %zu)", n, m);
	double p0 = pitchHz(seg + 4000, 8000, 32000), p1 = pitchHz(slow + 6000, 8000, 32000);
	CHECK(fabs(p0 - p1) < 6, "pitch kept (%.1f vs %.1f Hz)", p0, p1);
	static int16_t w16[16000 * 6];
	size_t k = vcResample(seg, n, 32000, w16, sizeof(w16) / 2, 16000);
	CHECK(k > n / 2 - 10 && k < n / 2 + 10, "resampled to 16 kHz (%zu)", k);
	CHECK(fabs(pitchHz(w16 + 2000, 6000, 16000) - p0) < 6, "pitch survives resampling");
	vcFree(&v);
}

int main(void) {
	testZip();
	testOptions();
	testRewind();
	testVoice();
	if (failures) {
		printf("%d failures\n", failures);
		return 1;
	}
	printf("front end tests passed\n");
	return 0;
}
