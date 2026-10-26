/* Host test for screen reading: frames made by tests/make_ocr_frames.py.
 * Usage: test_ocr glyphs.bin framedir [lingo.dic] */
#include "../src/psp2-lingo-ocr.h"
#include "../src/psp2-lingo.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* edit distance between two codepoint strings, ignoring spaces */
static int dist(const uint32_t* a, int na, const uint32_t* b, int nb) {
	uint32_t x[256], y[256];
	int n = 0, m = 0;
	for (int i = 0; i < na; ++i) {
		if (a[i] != ' ' && a[i] != 0x3000 && a[i] != '\n') {
			x[n++] = a[i];
		}
	}
	for (int i = 0; i < nb; ++i) {
		if (b[i] != ' ' && b[i] != 0x3000 && b[i] != '\n') {
			y[m++] = b[i];
		}
	}
	static int D[257][257];
	for (int i = 0; i <= n; ++i) {
		D[i][0] = i;
	}
	for (int j = 0; j <= m; ++j) {
		D[0][j] = j;
	}
	for (int i = 1; i <= n; ++i) {
		for (int j = 1; j <= m; ++j) {
			int c = D[i - 1][j - 1] + (x[i - 1] != y[j - 1]);
			int d = D[i - 1][j] + 1, e = D[i][j - 1] + 1;
			D[i][j] = c < d ? (c < e ? c : e) : (d < e ? d : e);
		}
	}
	return D[n][m];
}

static int letters(const uint32_t* a, int n) {
	int k = 0;
	for (int i = 0; i < n; ++i) {
		k += a[i] != ' ' && a[i] != 0x3000 && a[i] != '\n';
	}
	return k;
}

/* dictionary coverage: letters inside known words */
static int coverage(const uint32_t* cp, int n, void* user) {
	static struct LingoToken tok[LINGO_MAX_TOKENS];
	int k = lingoTokenize((const struct LingoDict*) user, cp, n, tok, LINGO_MAX_TOKENS);
	int s = 0;
	for (int i = 0; i < k; ++i) {
		if (tok[i].nIds) {
			s += tok[i].len * tok[i].len; /* longer words are stronger evidence */
		}
	}
	return s;
}

struct Frame {
	char name[64], font[64];
	uint8_t* luma;
	int w, h;
	uint32_t want[256];
	int nw;
	uint32_t lines[4][128];
	int lineLen[4], nLines;
};

static bool loadFrame(const char* dir, const char* name, struct Frame* fr) {
	char p[1024];
	snprintf(fr->name, sizeof(fr->name), "%s", name);
	snprintf(p, sizeof(p), "%s/%s.raw", dir, name);
	FILE* fp = fopen(p, "rb");
	uint8_t hd[4];
	if (!fp || fread(hd, 1, 4, fp) != 4) {
		return false;
	}
	fr->w = hd[0] | hd[1] << 8;
	fr->h = hd[2] | hd[3] << 8;
	fr->luma = malloc((size_t) fr->w * fr->h);
	if (fread(fr->luma, 1, (size_t) fr->w * fr->h, fp) != (size_t) fr->w * fr->h) {
		return false;
	}
	fclose(fp);
	snprintf(p, sizeof(p), "%s/%s.txt", dir, name);
	fp = fopen(p, "r");
	char line[512], all[1024] = "";
	if (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = 0;
		snprintf(fr->font, sizeof(fr->font), "%s", line);
	}
	fr->nLines = 0;
	while (fgets(line, sizeof(line), fp)) {
		strncat(all, line, sizeof(all) - strlen(all) - 1);
		line[strcspn(line, "\n")] = 0;
		if (fr->nLines < 4) {
			fr->lineLen[fr->nLines] = lingoUtf8Decode(line, fr->lines[fr->nLines], 128);
			fr->nLines++;
		}
	}
	fclose(fp);
	fr->nw = lingoUtf8Decode(all, fr->want, 256);
	return true;
}

static int evalFrame(struct LingoOcr* o, const struct Frame* fr, int* letterCount, bool verbose) {
	struct LingoOcrLine lines[16];
	int nl = lingoOcrRead(o, fr->luma, fr->w, fr->h, lines, 16);
	uint32_t got[512];
	int ng = 0;
	for (int i = 0; i < nl; ++i) {
		if (!lines[i].accepted) {
			continue;
		}
		for (int k = 0; k < lines[i].len && ng < 500; ++k) {
			got[ng++] = lines[i].cp[k];
		}
		got[ng++] = '\n';
	}
	int L = letters(fr->want, fr->nw);
	int err = dist(got, ng, fr->want, fr->nw);
	*letterCount = L;
	if (verbose) {
		char s[2048];
		lingoUtf8FromCodepoints(got, ng, s, sizeof(s));
		printf("%s %s: %d/%d wrong\n%s\n", fr->name, fr->font, err > L ? L : err, L, s);
	}
	return err > L ? L : err;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		return 2;
	}
	struct LingoOcr* o = lingoOcrLoad(argv[1]);
	if (!o) {
		printf("FAIL: can't load %s\n", argv[1]);
		return 1;
	}
	struct LingoDict* dict = argc > 3 ? lingoDictLoad(argv[3]) : NULL;
	if (dict) {
		lingoOcrSetScorer(o, coverage, dict);
		printf("with the dictionary settling doubtful letters\n");
	}
	printf("%d reference glyphs\n", lingoOcrTemplates(o));
	DIR* d = opendir(argv[2]);
	struct dirent* e;
	static char names[256][64];
	int nn = 0;
	while (d && (e = readdir(d)) && nn < 256) {
		size_t l = strlen(e->d_name);
		if (l > 4 && !strcmp(e->d_name + l - 4, ".raw")) {
			snprintf(names[nn++], 64, "%.*s", (int) l - 4, e->d_name);
		}
	}
	if (d) {
		closedir(d);
	}
	for (int i = 0; i < nn; ++i) {
		for (int j = i + 1; j < nn; ++j) {
			if (strcmp(names[j], names[i]) < 0) {
				char t[64];
				memcpy(t, names[i], 64);
				memcpy(names[i], names[j], 64);
				memcpy(names[j], t, 64);
			}
		}
	}
	static struct Frame fr[256];
	for (int i = 0; i < nn; ++i) {
		if (!loadFrame(argv[2], names[i], &fr[i])) {
			printf("FAIL: frame %s\n", names[i]);
			return 1;
		}
	}
	bool verbose = getenv("OCR_VERBOSE") != NULL;
	int coldErr = 0, warmErr = 0, total = 0, failures = 0;
	for (int g = 0; g < nn;) {
		int h = g;
		while (h < nn && !strcmp(fr[h].font, fr[g].font)) {
			++h;
		}
		/* a new game: nothing learned yet */
		lingoOcrSetCache(o, NULL);
		int ce = 0, we = 0, letters = 0, L;
		for (int i = g; i < h; ++i) {
			ce += evalFrame(o, &fr[i], &L, verbose);
			letters += L;
		}
		/* the player teaches the first frame's lines, then plays on */
		char why[200];
		for (int k = 0; k < fr[g].nLines; ++k) {
			lingoOcrTeach(o, fr[g].luma, fr[g].w, fr[g].h, fr[g].lines[k], fr[g].lineLen[k], why, sizeof(why));
		}
		for (int i = g; i < h; ++i) {
			we += evalFrame(o, &fr[i], &L, verbose);
		}
		printf("  %-34s %5.1f%% right first time, %5.1f%% after teaching one screen (%d letters known)\n", fr[g].font,
		       100.0 * (letters - ce) / letters, 100.0 * (letters - we) / letters, lingoOcrLearned(o));
		coldErr += ce;
		warmErr += we;
		total += letters;
		g = h;
	}
	double cold = 100.0 * (total - coldErr) / total, warm = 100.0 * (total - warmErr) / total;
	printf("overall %.1f%% first time, %.1f%% after teaching\n", cold, warm);
	if (cold < 80) {
		printf("FAIL: first-time screen reading below 80%%\n");
		++failures;
	}
	if (warm < cold + 2) {
		printf("FAIL: teaching should help\n");
		++failures;
	}
	lingoOcrFree(o);
	return failures ? 1 : 0;
}
