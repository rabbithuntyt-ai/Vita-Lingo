/* Lingo: reading a game's font to find its text encoding, on fake console
 * memory made by tests/make_tile_dumps.py with fonts Lingo has never seen.
 * Usage: test_tiles glyphs.bin lingo.dic dumpdir */
#include "psp2-lingo.h"
#include "psp2-lingo-reader.h"
#include "psp2-lingo-tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(c, ...)                                         \
	do {                                                      \
		if (!(c)) {                                           \
			++failures;                                       \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
			printf(__VA_ARGS__);                              \
			printf("\n");                                     \
		}                                                     \
	} while (0)

static struct LingoDict* sDict;

static int scorer(const uint32_t* cp, int n, void* user) {
	(void) user;
	return lingoTilesWordScore(sDict, cp, n);
}

static uint8_t* slurp(const char* path, size_t* n) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*n = (size_t) ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* d = malloc(*n);
	if (fread(d, 1, *n, f) != *n) {
		*n = 0;
	}
	fclose(f);
	return d;
}

/* share of b's letters found in order in a (longest common subsequence,
 * ignoring spaces and line breaks) */
static float similar(const char* a, const char* b) {
	uint32_t ra[256], rb[256], ca[256], cb[256];
	int na0 = lingoUtf8Decode(a, ra, 256), nb0 = lingoUtf8Decode(b, rb, 256), na = 0, nb = 0;
	for (int i = 0; i < na0; ++i) {
		if (ra[i] != 0x3000 && ra[i] != '\n') {
			ca[na++] = ra[i];
		}
	}
	for (int i = 0; i < nb0; ++i) {
		if (rb[i] != 0x3000 && rb[i] != '\n') {
			cb[nb++] = rb[i];
		}
	}
	static int L[257][257];
	for (int i = 0; i <= na; ++i) {
		for (int j = 0; j <= nb; ++j) {
			L[i][j] = !i || !j ? 0 : ca[i - 1] == cb[j - 1] ? L[i - 1][j - 1] + 1 : (L[i - 1][j] > L[i][j - 1] ? L[i - 1][j] : L[i][j - 1]);
		}
	}
	return nb ? (float) L[na][nb] / nb : 0;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: test_tiles glyphs.bin lingo.dic dumpdir\n");
		return 2;
	}
	struct LingoOcr* o = lingoOcrLoad(argv[1]);
	sDict = lingoDictLoad(argv[2]);
	CHECK(o && sDict, "glyphs or dictionary didn't load");
	if (!o || !sDict) {
		return 1;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/cases.txt", argv[3]);
	FILE* f = fopen(path, "r");
	CHECK(f, "no cases");
	if (!f) {
		return 1;
	}
	static struct LingoTable best, scratch;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* name = strtok(line, "\t");
		char* system = strtok(NULL, "\t");
		char* expect = strtok(NULL, "\t");
		if (!name || !system || !expect) {
			continue;
		}
		size_t nv, nr;
		snprintf(path, sizeof(path), "%s/%s.vram", argv[3], name);
		uint8_t* vram = slurp(path, &nv);
		snprintf(path, sizeof(path), "%s/%s.ram", argv[3], name);
		uint8_t* ram = slurp(path, &nr);
		struct LingoRegions rs;
		lingoRegionsClear(&rs);
		lingoRegionsAdd(&rs, "ram", ram, nr);
		lingoRegionsAdd(&rs, "vram", vram, nv);
		char why[200];
		clock_t c0 = clock();
		int score = lingoTilesFind(o, system, &rs, NULL, 0, scorer, NULL, &best, &scratch, why, sizeof(why));
		double secs = (double) (clock() - c0) / CLOCKS_PER_SEC;
		printf("%s (%s): score %d in %.2fs. %s\n", name, system, score, secs, why);
		CHECK(score > 0, "%s: no table", name);
		/* the line, as the table reads memory */
		static struct LingoText lines[400];
		int n = score > 0 ? lingoTilesReadAll(&best, &rs, lines, 400) : 0;
		float bestSim = 0;
		char got[512] = "", s[512];
		for (int i = 0; i < n; ++i) {
			lingoUtf8FromCodepoints(lines[i].cp, lines[i].len, s, sizeof(s));
			float sim = similar(s, expect);
			if (getenv("TILES_DEBUG")) {
				printf("    line %08X: %s\n", lines[i].start, s);
			}
			if (sim > bestSim) {
				bestSim = sim;
				snprintf(got, sizeof(got), "%s", s);
			}
		}
		printf("  expect: %s\n  read:   %s (%.0f%%)\n", expect, got, bestSim * 100);
		CHECK(bestSim >= 0.9f, "%s: read the line %.0f%% right", name, bestSim * 100);
		free(vram);
		free(ram);
	}
	fclose(f);
	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("tile tests passed\n");
	return 0;
}
