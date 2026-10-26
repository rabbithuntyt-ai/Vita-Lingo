/* Lingo: reading text off the screen (see psp2-lingo-ocr.h). */
#include "psp2-lingo-ocr.h"
#include "psp2-lingo-reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F 16
#define K_COARSE 96
#define CACHE_SLOTS 8192

/* What a letter looks like, independent of the font's size and weight:
 * its own shape (cropped tight, scaled to fit), the directions of its
 * strokes, and where it sits in the line (small kana sit low and small). */
struct Feat {
	uint8_t raw[F * F];
	uint8_t dir[64];
	uint8_t rel[4]; /* top, bottom, width, height of the ink, 0..255 of the cell */
	float rm, ri, dm, di;
	uint16_t coarse[16];
};

struct Tmpl {
	uint32_t cp;
	uint16_t size;
	struct Feat f;
};

struct Learned {
	uint64_t key; /* 0 = empty */
	uint32_t cp;
	uint16_t count;
	uint16_t pad;
};

struct LingoOcr {
	struct Tmpl* t;
	int n;
	struct Learned* cache;
	int learned;
	bool dirty;
	char cachePath[256];
	/* scratch */
	uint8_t* ink;
	int inkCap;
	LingoOcrScorer scorer;
	void* scorerUser;
};

void lingoOcrSetScorer(struct LingoOcr* o, LingoOcrScorer scorer, void* user) {
	if (o) {
		o->scorer = scorer;
		o->scorerUser = user;
	}
}

/* How likely a character is in game text before looking at it: symbols and
 * Latin letters turn up rarely, kana constantly. */
static float prior(uint32_t cp) {
	if ((cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || cp == 0x30FC) {
		return 0;
	}
	if (cp >= 0x4E00 && cp <= 0x9FFF) {
		return -0.6f;
	}
	if (cp == 0x3001 || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0x300C || cp == 0x300D || cp == 0x2026) {
		return -0.4f;
	}
	if (cp >= 0xFF10 && cp <= 0xFF19) {
		return -1.0f; /* digits */
	}
	return -2.0f; /* other symbols and Latin letters */
}

/* か <-> が and the like, for trying a letter with and without its marks */
static int variants(uint32_t cp, uint32_t* out) {
	int n = 0;
	uint32_t base = cp >= 0x30A1 ? cp - 0x60 : cp;
	uint32_t off = cp >= 0x30A1 ? 0x60 : 0;
	/* か..ぢ, つ..ど come in voiced pairs; は行 in threes */
	if ((base >= 0x304B && base <= 0x3062) || (base >= 0x3064 && base <= 0x3069)) {
		bool unvoiced = (base >= 0x304B && base <= 0x3062) ? ((base - 0x304B) % 2 == 0) : (base == 0x3064 || base == 0x3066 || base == 0x3068);
		if (base == 0x3063) {
			return 0; /* small っ */
		}
		out[n++] = (unvoiced ? base + 1 : base - 1) + off;
	} else if (base >= 0x306F && base <= 0x307D) {
		uint32_t row = 0x306F + (base - 0x306F) / 3 * 3;
		for (uint32_t k = 0; k < 3; ++k) {
			if (row + k != base) {
				out[n++] = row + k + off;
			}
		}
	}
	return n;
}

/* ---- templates ---- */

static void stats(const uint8_t* v, int n, float* mean, float* inv) {
	double sum = 0;
	for (int i = 0; i < n; ++i) {
		sum += v[i];
	}
	double m = sum / n, var = 0;
	for (int i = 0; i < n; ++i) {
		double d = v[i] - m;
		var += d * d;
	}
	*mean = (float) m;
	*inv = var > 0 ? (float) (1.0 / sqrt(var)) : 0;
}

/* bits: w*h bytes of 0/1 */
static bool featFromBits(const uint8_t* bits, int w, int h, struct Feat* out) {
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			if (bits[y * w + x]) {
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
		}
	}
	if (x1 < 0) {
		return false;
	}
	int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
	out->rel[0] = (uint8_t) (255 * y0 / h);
	out->rel[1] = (uint8_t) (255 * (y1 + 1) / h);
	out->rel[2] = (uint8_t) (255 * bw / w > 255 ? 255 : 255 * bw / w);
	out->rel[3] = (uint8_t) (255 * bh / h);
	/* the ink, scaled to fit 26 px inside a 32x32 square, centred */
	static float g[32][32], t[32][32];
	float sc = 26.0f / (bw > bh ? bw : bh);
	float ox = (32 - bw * sc) / 2, oy = (32 - bh * sc) / 2;
	for (int cy = 0; cy < 32; ++cy) {
		for (int cx = 0; cx < 32; ++cx) {
			int hit = 0;
			for (int sy = 0; sy < 2; ++sy) {
				for (int sx = 0; sx < 2; ++sx) {
					float fx = (cx + 0.25f + 0.5f * sx - ox) / sc, fy = (cy + 0.25f + 0.5f * sy - oy) / sc;
					int ix = (int) floorf(fx), iy = (int) floorf(fy);
					if (ix >= 0 && iy >= 0 && ix < bw && iy < bh && bits[(y0 + iy) * w + x0 + ix]) {
						++hit;
					}
				}
			}
			t[cy][cx] = hit / 4.0f;
		}
	}
	/* soften: a pixel's place matters, its exact edge less so */
	for (int y = 0; y < 32; ++y) {
		for (int x = 0; x < 32; ++x) {
			float acc = 0;
			int n = 0;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					int yy = y + dy, xx = x + dx;
					if (yy >= 0 && yy < 32 && xx >= 0 && xx < 32) {
						acc += t[yy][xx] * (dx || dy ? 1 : 2);
						n += dx || dy ? 1 : 2;
					}
				}
			}
			g[y][x] = acc / n;
		}
	}
	for (int y = 0; y < F; ++y) {
		for (int x = 0; x < F; ++x) {
			float v = (g[y * 2][x * 2] + g[y * 2 + 1][x * 2] + g[y * 2][x * 2 + 1] + g[y * 2 + 1][x * 2 + 1]) / 4;
			out->raw[y * F + x] = (uint8_t) (v * 255 + 0.5f);
		}
	}
	/* stroke directions: gradient orientation in four bins, pooled 4x4 */
	float d[64];
	memset(d, 0, sizeof(d));
	for (int y = 1; y < 31; ++y) {
		for (int x = 1; x < 31; ++x) {
			float gx = g[y][x + 1] - g[y][x - 1], gy = g[y + 1][x] - g[y - 1][x];
			float m = sqrtf(gx * gx + gy * gy);
			if (m < 0.05f) {
				continue;
			}
			float a = atan2f(gy, gx);
			if (a < 0) {
				a += (float) M_PI;
			}
			float bin = a / ((float) M_PI / 4);
			int b0 = (int) bin % 4, b1 = (b0 + 1) % 4;
			float f1 = bin - floorf(bin), f0 = 1 - f1;
			int zone = (y / 8) * 4 + x / 8;
			d[zone * 4 + b0] += m * f0;
			d[zone * 4 + b1] += m * f1;
		}
	}
	float mx = 0;
	for (int i = 0; i < 64; ++i) {
		mx = d[i] > mx ? d[i] : mx;
	}
	for (int i = 0; i < 64; ++i) {
		out->dir[i] = (uint8_t) (mx > 0 ? 255 * d[i] / mx : 0);
	}
	stats(out->raw, F * F, &out->rm, &out->ri);
	stats(out->dir, 64, &out->dm, &out->di);
	for (int by = 0; by < 4; ++by) {
		for (int bx = 0; bx < 4; ++bx) {
			unsigned acc = 0;
			for (int y = 0; y < 4; ++y) {
				for (int x = 0; x < 4; ++x) {
					acc += out->raw[(by * 4 + y) * F + bx * 4 + x];
				}
			}
			out->coarse[by * 4 + bx] = (uint16_t) (acc / 16);
		}
	}
	return true;
}

static float ncc(const uint8_t* a, float am, float ai, const uint8_t* b, float bm, float bi, int n) {
	if (ai == 0 || bi == 0) {
		return 0;
	}
	float acc = 0;
	for (int i = 0; i < n; ++i) {
		acc += (a[i] - am) * (b[i] - bm);
	}
	return acc * ai * bi;
}

static float similarity(const struct Feat* a, const struct Feat* b) {
	float shape = 0.5f * ncc(a->raw, a->rm, a->ri, b->raw, b->rm, b->ri, F * F) +
	              0.5f * ncc(a->dir, a->dm, a->di, b->dir, b->dm, b->di, 64);
	float place = (abs(a->rel[3] - b->rel[3]) + 0.6f * abs(a->rel[2] - b->rel[2]) + 0.5f * abs(a->rel[0] - b->rel[0]) +
	               0.5f * abs(a->rel[1] - b->rel[1])) /
	              255.0f;
	return shape - 0.9f * place;
}

struct LingoOcr* lingoOcrFromMemory(const uint8_t* data, size_t size) {
	if (size < 12 || memcmp(data, "LGLY", 4)) {
		return NULL;
	}
	uint32_t ver, n;
	memcpy(&ver, data + 4, 4);
	memcpy(&n, data + 8, 4);
	if (ver != 2) {
		return NULL;
	}
	struct LingoOcr* o = calloc(1, sizeof(*o));
	if (!o) {
		return NULL;
	}
	o->t = malloc(sizeof(struct Tmpl) * n);
	o->cache = calloc(CACHE_SLOTS, sizeof(struct Learned));
	if (!o->t || !o->cache) {
		lingoOcrFree(o);
		return NULL;
	}
	size_t p = 12;
	uint8_t bits[32 * 32];
	for (uint32_t i = 0; i < n && p + 8 <= size; ++i) {
		uint32_t cp;
		memcpy(&cp, data + p, 4);
		int sz = data[p + 4], w = data[p + 5], h = data[p + 6];
		p += 8;
		if (p + (size_t) h * 4 > size || w > 32 || h > 32) {
			break;
		}
		for (int y = 0; y < h; ++y) {
			uint32_t row;
			memcpy(&row, data + p + y * 4, 4);
			for (int x = 0; x < w; ++x) {
				bits[y * w + x] = (uint8_t) ((row >> x) & 1);
			}
		}
		p += (size_t) h * 4;
		struct Tmpl* t = &o->t[o->n];
		if (featFromBits(bits, w, h, &t->f)) {
			t->cp = cp;
			t->size = (uint16_t) sz;
			o->n++;
		}
	}
	return o;
}

struct LingoOcr* lingoOcrLoad(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* d = malloc((size_t) n);
	struct LingoOcr* o = NULL;
	if (d && fread(d, 1, (size_t) n, f) == (size_t) n) {
		o = lingoOcrFromMemory(d, (size_t) n);
	}
	free(d);
	fclose(f);
	return o;
}

void lingoOcrFree(struct LingoOcr* o) {
	if (!o) {
		return;
	}
	free(o->t);
	free(o->cache);
	free(o->ink);
	free(o);
}

int lingoOcrTemplates(const struct LingoOcr* o) {
	return o ? o->n : 0;
}

/* ---- the game's own letters ---- */

static struct Learned* cacheFind(struct LingoOcr* o, uint64_t key, bool create) {
	uint32_t h = (uint32_t) (key ^ (key >> 29)) * 2654435761u;
	for (int p = 0; p < 64; ++p) {
		struct Learned* l = &o->cache[(h + (uint32_t) p) % CACHE_SLOTS];
		if (l->key == key) {
			return l;
		}
		if (!l->key) {
			if (!create || o->learned >= CACHE_SLOTS * 3 / 4) {
				return NULL;
			}
			l->key = key;
			l->cp = 0;
			l->count = 0;
			o->learned++;
			return l;
		}
	}
	return NULL;
}

static void learn(struct LingoOcr* o, uint64_t key, uint32_t cp, int weight) {
	struct Learned* l = cacheFind(o, key, true);
	if (!l) {
		return;
	}
	if (l->cp == cp) {
		l->count = (uint16_t) (l->count + weight > 1000 ? 1000 : l->count + weight);
	} else if (l->count <= (uint16_t) weight) {
		l->cp = cp; /* a correction (or a first sighting) */
		l->count = (uint16_t) weight;
	} else {
		l->count = (uint16_t) (l->count - weight); /* disagreement: lose confidence */
	}
	o->dirty = true;
}

void lingoOcrSetCache(struct LingoOcr* o, const char* path) {
	if (!o) {
		return;
	}
	memset(o->cache, 0, CACHE_SLOTS * sizeof(struct Learned));
	o->learned = 0;
	o->dirty = false;
	snprintf(o->cachePath, sizeof(o->cachePath), "%s", path ? path : "");
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		return;
	}
	char magic[4];
	uint32_t n = 0;
	if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "LGLC", 4) && fread(&n, 4, 1, f) == 1) {
		for (uint32_t i = 0; i < n; ++i) {
			struct Learned l;
			if (fread(&l, sizeof(l), 1, f) != 1) {
				break;
			}
			struct Learned* s = cacheFind(o, l.key, true);
			if (s) {
				s->cp = l.cp;
				s->count = l.count;
			}
		}
	}
	fclose(f);
	o->dirty = false;
}

bool lingoOcrSaveCache(struct LingoOcr* o) {
	if (!o || !o->dirty || !o->cachePath[0]) {
		return false;
	}
	char tmp[280];
	snprintf(tmp, sizeof(tmp), "%s.tmp", o->cachePath);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	uint32_t n = 0;
	for (int i = 0; i < CACHE_SLOTS; ++i) {
		n += o->cache[i].key && o->cache[i].cp;
	}
	fwrite("LGLC", 1, 4, f);
	fwrite(&n, 4, 1, f);
	for (int i = 0; i < CACHE_SLOTS; ++i) {
		if (o->cache[i].key && o->cache[i].cp) {
			fwrite(&o->cache[i], sizeof(struct Learned), 1, f);
		}
	}
	bool ok = !ferror(f);
	fclose(f);
	if (ok) {
		remove(o->cachePath);
		ok = rename(tmp, o->cachePath) == 0;
	}
	o->dirty = !ok;
	return ok;
}

int lingoOcrLearned(const struct LingoOcr* o) {
	if (!o) {
		return 0;
	}
	int n = 0;
	for (int i = 0; i < CACHE_SLOTS; ++i) {
		n += o->cache[i].key && o->cache[i].cp && o->cache[i].count >= 2;
	}
	return n;
}

/* ---- finding text ---- */

void lingoOcrLuma(const void* pixels, int w, int h, int stride, int format, uint8_t* out) {
	for (int y = 0; y < h; ++y) {
		const uint8_t* s = (const uint8_t*) pixels + (size_t) y * stride;
		uint8_t* d = out + (size_t) y * w;
		for (int x = 0; x < w; ++x) {
			int r, g, b;
			switch (format) {
			case 0:
				r = s[x * 4];
				g = s[x * 4 + 1];
				b = s[x * 4 + 2];
				break;
			case 1:
				b = s[x * 4];
				g = s[x * 4 + 1];
				r = s[x * 4 + 2];
				break;
			case 2: {
				unsigned v = (unsigned) (s[x * 2] | (s[x * 2 + 1] << 8));
				r = ((v >> 11) & 31) << 3;
				g = ((v >> 5) & 63) << 2;
				b = (v & 31) << 3;
				break;
			}
			default: {
				unsigned v = (unsigned) (s[x * 2] | (s[x * 2 + 1] << 8));
				r = ((v >> 10) & 31) << 3;
				g = ((v >> 5) & 31) << 3;
				b = (v & 31) << 3;
				break;
			}
			}
			d[x] = (uint8_t) ((r * 77 + g * 150 + b * 29) >> 8);
		}
	}
}

/* Ink: pixels that stand out from the flat background of their 8x8 block. */
static void inkMask(struct LingoOcr* o, const uint8_t* luma, int w, int h) {
	if (o->inkCap < w * h) {
		free(o->ink);
		o->ink = malloc((size_t) w * h);
		o->inkCap = o->ink ? w * h : 0;
	}
	memset(o->ink, 0, (size_t) w * h);
	for (int by = 0; by < h; by += 8) {
		for (int bx = 0; bx < w; bx += 8) {
			/* the block's background: its most common brightness, looking a
			 * little beyond it so a thick stroke can't win */
			int hist[16] = { 0 };
			int total = 0;
			for (int y = by - 4; y < by + 12; ++y) {
				if (y < 0 || y >= h) {
					continue;
				}
				for (int x = bx - 4; x < bx + 12; ++x) {
					if (x < 0 || x >= w) {
						continue;
					}
					hist[luma[y * w + x] >> 4]++;
					++total;
				}
			}
			int mode = 0;
			for (int i = 1; i < 16; ++i) {
				if (hist[i] > hist[mode]) {
					mode = i;
				}
			}
			int near = hist[mode] + (mode ? hist[mode - 1] : 0) + (mode < 15 ? hist[mode + 1] : 0);
			if (near * 100 < total * 50) {
				continue; /* busy picture, not a text box */
			}
			int bg = mode * 16 + 8;
			for (int y = by; y < by + 8 && y < h; ++y) {
				for (int x = bx; x < bx + 8 && x < w; ++x) {
					int d = luma[y * w + x] - bg;
					o->ink[y * w + x] = (uint8_t) (d > 52 || d < -52);
				}
			}
		}
	}
	/* box borders and other long straight lines are not letters */
	for (int y = 0; y < h; ++y) {
		int x = 0;
		while (x < w) {
			if (!o->ink[y * w + x]) {
				++x;
				continue;
			}
			int s = x;
			while (x < w && o->ink[y * w + x]) {
				++x;
			}
			if (x - s > 40) {
				memset(o->ink + y * w + s, 0, (size_t) (x - s));
			}
		}
	}
	for (int x = 0; x < w; ++x) {
		int y = 0;
		while (y < h) {
			if (!o->ink[y * w + x]) {
				++y;
				continue;
			}
			int s = y;
			while (y < h && o->ink[y * w + x]) {
				++y;
			}
			if (y - s > 40) {
				for (int k = s; k < y; ++k) {
					o->ink[k * w + x] = 0;
				}
			}
		}
	}
}

struct Band {
	int y0, y1;
};

static int findBands(const struct LingoOcr* o, int w, int h, struct Band* out, int max) {
	int n = 0;
	int y = 0;
	while (y < h && n < max) {
		/* a row counts if it has ink but isn't one long line (a box border) */
		int cnt = 0, run = 0, best = 0;
		for (int x = 0; x < w; ++x) {
			if (o->ink[y * w + x]) {
				++cnt;
				if (++run > best) {
					best = run;
				}
			} else {
				run = 0;
			}
		}
		if (cnt < 2 || best > w / 3) {
			++y;
			continue;
		}
		int y0 = y;
		while (y < h) {
			cnt = 0;
			run = 0;
			best = 0;
			for (int x = 0; x < w; ++x) {
				if (o->ink[y * w + x]) {
					++cnt;
					if (++run > best) {
						best = run;
					}
				} else {
					run = 0;
				}
			}
			if (cnt < 1 || best > w / 3) {
				break;
			}
			++y;
		}
		int bh = y - y0;
		if (bh >= 6 && bh <= 40) {
			out[n].y0 = y0;
			out[n].y1 = y;
			++n;
		}
	}
	return n;
}

/* ---- recognising a cell ---- */

static bool cellFeature(const struct LingoOcr* o, int w, int x0, int cw, int y0, int ch, struct Feat* f) {
	static uint8_t bits[64 * 64];
	if (cw > 64 || ch > 64) {
		return false;
	}
	for (int y = 0; y < ch; ++y) {
		memcpy(bits + y * cw, o->ink + (y0 + y) * w + x0, (size_t) cw);
	}
	return featFromBits(bits, cw, ch, f);
}

static uint64_t cellKey(const struct LingoOcr* o, int w, int x0, int cw, int y0, int ch) {
	uint64_t h = 1469598103934665603ull ^ (uint64_t) (cw * 64 + ch);
	for (int y = 0; y < ch; ++y) {
		uint64_t row = 0;
		for (int x = 0; x < cw && x < 64; ++x) {
			row = (row << 1) | o->ink[(y0 + y) * w + x0 + x];
		}
		h = (h ^ row) * 1099511628211ull;
	}
	return h ? h : 1;
}

struct Cand {
	uint32_t cp;
	float score;
};

/* Best three distinct characters for a cell. */
static int recognise(const struct LingoOcr* o, const struct Feat* f, struct Cand* out) {
	/* shortlist by the 4x4 summary and the letter's height */
	int best[K_COARSE];
	unsigned bestD[K_COARSE];
	int nb = 0;
	for (int i = 0; i < o->n; ++i) {
		const struct Feat* t = &o->t[i].f;
		unsigned d = (unsigned) abs(f->rel[3] - t->rel[3]) * 6 + (unsigned) abs(f->rel[2] - t->rel[2]) * 3;
		for (int k = 0; k < 16; ++k) {
			int e = (int) f->coarse[k] - t->coarse[k];
			d += (unsigned) (e < 0 ? -e : e);
		}
		if (nb < K_COARSE || d < bestD[K_COARSE - 1]) {
			int j = nb < K_COARSE ? nb++ : K_COARSE - 1;
			while (j > 0 && bestD[j - 1] > d) {
				bestD[j] = bestD[j - 1];
				best[j] = best[j - 1];
				--j;
			}
			bestD[j] = d;
			best[j] = i;
		}
	}
	int n = 0;
	for (int s = 0; s < nb; ++s) {
		const struct Tmpl* t = &o->t[best[s]];
		float sc = similarity(f, &t->f);
		int k;
		for (k = 0; k < n; ++k) {
			if (out[k].cp == t->cp) {
				break;
			}
		}
		if (k < n) {
			if (sc <= out[k].score) {
				continue;
			}
			out[k].score = sc;
		} else if (n < 3) {
			out[n].cp = t->cp;
			out[n].score = sc;
			k = n++;
		} else if (sc > out[2].score) {
			out[2].cp = t->cp;
			out[2].score = sc;
			k = 2;
		} else {
			continue;
		}
		while (k > 0 && out[k].score > out[k - 1].score) {
			struct Cand tmp = out[k];
			out[k] = out[k - 1];
			out[k - 1] = tmp;
			--k;
		}
	}
	return n;
}

/* ---- lines ---- */

struct Cell {
	int x, w;
	bool space;
	uint64_t key;
	struct Cand c[3];
	int nc;
	bool known; /* from the game's own letters */
};

static float columnInk(const struct LingoOcr* o, int w, int x, int y0, int y1) {
	int n = 0;
	for (int y = y0; y < y1; ++y) {
		n += o->ink[y * w + x];
	}
	return (float) n;
}

/* Splits a band into lines (clusters of ink across) and letter cells. */
static int readBand(struct LingoOcr* o, int w, struct Band b, struct LingoOcrLine* out, int max, bool learnFromIt) {
	int bh = b.y1 - b.y0;
	static float col[2048];
	for (int x = 0; x < w && x < 2048; ++x) {
		col[x] = columnInk(o, w, x, b.y0, b.y1);
	}
	int nOut = 0;
	int x = 0;
	while (x < w && nOut < max) {
		while (x < w && col[x] == 0) {
			++x;
		}
		if (x >= w) {
			break;
		}
		/* a cluster ends at a gap wider than two letters */
		int cx0 = x, last = x, gap = 0;
		while (x < w) {
			if (col[x] > 0) {
				last = x;
				gap = 0;
			} else if (++gap > bh * 5 / 2 + 2) {
				break;
			}
			++x;
		}
		int cx1 = last + 1;
		if (cx1 - cx0 < bh * 2) {
			continue; /* too short to be a line of text */
		}
		/* letter pitch: the spacing whose cell edges cross the least ink,
		 * fitted word by word so spaces of any width don't matter. Japanese
		 * fonts are about as wide as they are tall. */
		int pMin = bh - 1 > 6 ? bh - 1 : 6, pMax = bh + 3;
		int bestP = bh, bestO = 0;
		float bestCost = 1e30f;
		for (int p = pMin; p <= pMax; ++p) {
			float cost = 0;
			int words = 0;
			int gx = cx0;
			while (gx < cx1) {
				while (gx < cx1 && col[gx] == 0) {
					++gx;
				}
				if (gx >= cx1) {
					break;
				}
				int ge = gx, blank = 0;
				for (int xx = gx; xx < cx1; ++xx) {
					if (col[xx] > 0) {
						ge = xx + 1;
						blank = 0;
					} else if (++blank >= (p + 1) / 2) {
						break;
					}
				}
				float wb = 1e30f;
				for (int st = gx - p / 2; st <= gx; ++st) {
					float c = 0;
					for (int ex = st; ex <= ge; ex += p) {
						if (ex >= gx && ex < ge) {
							c += col[ex];
						}
					}
					/* the word should fill whole cells */
					int cellsN = (ge - st + p - 1) / p;
					c += 0.6f * (float) (cellsN * p - (ge - st)) / p;
					if (c < wb) {
						wb = c;
					}
				}
				cost += wb;
				++words;
				gx = ge;
			}
			cost = words ? cost / words : 1e30f;
			cost += 0.1f * (float) abs(p - (bh + 1));
			if (cost < bestCost) {
				bestCost = cost;
				bestP = p;
			}
		}
		/* lay the grid word by word: after a gap (a space, maybe a narrow
		 * one) the letters may start off the old grid */
		static struct Cell cells[OCR_MAX_CELLS];
		int nc = 0;
		int gx = cx0;
		int firstGrid = -1;
		(void) bestO;
		while (gx < cx1 && nc < OCR_MAX_CELLS) {
			while (gx < cx1 && col[gx] == 0) {
				++gx;
			}
			if (gx >= cx1) {
				break;
			}
			/* this word runs until a gap of half a letter or more */
			int ge = gx, blank = 0;
			for (int xx = gx; xx < cx1; ++xx) {
				if (col[xx] > 0) {
					ge = xx + 1;
					blank = 0;
				} else if (++blank >= (bestP + 1) / 2) {
					break;
				}
			}
			/* best grid offset for this word: edges through the least ink */
			int bestStart = gx;
			float bc = 1e30f;
			for (int st = gx - bestP / 2; st <= gx; ++st) {
				float cost = 0;
				for (int ex = st; ex <= ge; ex += bestP) {
					if (ex >= gx && ex < ge) {
						cost += col[ex];
					}
				}
				/* glyphs usually sit a pixel in from the cell's left edge */
				cost += 0.05f * (float) abs(gx - st - 1);
				if (cost < bc) {
					bc = cost;
					bestStart = st;
				}
			}
			if (nc && gx - (cells[nc - 1].x + bestP) >= bestP / 2) {
				/* the gap was a space */
				struct Cell* sp = &cells[nc++];
				memset(sp, 0, sizeof(*sp));
				sp->space = true;
			}
			if (firstGrid < 0) {
				firstGrid = bestStart;
			}
			for (int cx = bestStart; cx < ge && nc < OCR_MAX_CELLS; cx += bestP) {
				int a = cx < 0 ? 0 : cx, e = cx + bestP > w ? w : cx + bestP;
				struct Cell* c = &cells[nc++];
				memset(c, 0, sizeof(*c));
				c->x = a;
				c->w = e - a;
				float ink = 0;
				for (int xx = a; xx < e; ++xx) {
					ink += col[xx];
				}
				if (ink == 0 || e - a < 3) {
					c->space = true;
					continue;
				}
				c->key = cellKey(o, w, a, e - a, b.y0, bh);
				struct Learned* l = cacheFind(o, c->key, false);
				if (l && l->cp && l->count >= 2) {
					c->c[0].cp = l->cp;
					c->c[0].score = 1;
					c->nc = 1;
					c->known = true;
					continue;
				}
				struct Feat f;
				c->nc = cellFeature(o, w, a, e - a, b.y0, bh, &f) ? recognise(o, &f, c->c) : 0;
				if (!c->nc) {
					c->space = true;
				}
			}
			gx = ge;
		}
		int start = firstGrid < 0 ? cx0 : firstGrid;
		/* Viterbi: letter scores plus how Japanese the sequence is */
		static float score[OCR_MAX_CELLS][3];
		static int from[OCR_MAX_CELLS][3];
		int prev = -1;
		for (int i = 0; i < nc; ++i) {
			if (cells[i].space) {
				continue;
			}
			for (int k = 0; k < cells[i].nc; ++k) {
				float em = cells[i].c[k].score * 10 + prior(cells[i].c[k].cp);
				float bestIn = 0;
				int arg = 0;
				if (prev >= 0) {
					bestIn = -1e30f;
					for (int j = 0; j < cells[prev].nc; ++j) {
						float tr = 0;
						int a = lingoBigramIndex(cells[prev].c[j].cp), bi = lingoBigramIndex(cells[i].c[k].cp);
						if (a >= 0 && bi >= 0 && prev == i - 1) {
							tr = LINGO_BIGRAM[a][bi] / 8.0f * 0.9f;
						}
						if (score[prev][j] + tr > bestIn) {
							bestIn = score[prev][j] + tr;
							arg = j;
						}
					}
				}
				score[i][k] = bestIn + em;
				from[i][k] = arg;
			}
			prev = i;
		}
		int choice[OCR_MAX_CELLS];
		if (prev >= 0) {
			int k = 0;
			for (int j = 1; j < cells[prev].nc; ++j) {
				if (score[prev][j] > score[prev][k]) {
					k = j;
				}
			}
			for (int i = prev; i >= 0; --i) {
				if (cells[i].space) {
					continue;
				}
				choice[i] = k;
				k = from[i][k];
			}
		}
		struct LingoOcrLine* L = &out[nOut];
		memset(L, 0, sizeof(*L));
		L->x = cx0;
		L->y = b.y0;
		L->w = cx1 - cx0;
		L->h = bh;
		L->pitch = bestP;
		L->gridX = start;
		uint32_t hash = 2166136261u;
		float sum = 0;
		int letters = 0, good = 0;
		for (int i = 0; i < nc && L->len < OCR_MAX_CELLS; ++i) {
			if (cells[i].space) {
				if (L->len && L->cp[L->len - 1] != 0x3000) {
					L->cp[L->len++] = 0x3000;
				}
				hash = (hash ^ 0x3000) * 16777619u;
				continue;
			}
			const struct Cand* c = &cells[i].c[choice[i]];
			L->cellX[L->len] = (int16_t) cells[i].x;
			L->cellW[L->len] = (uint8_t) cells[i].w;
			L->cp[L->len++] = c->cp;
			hash = (hash ^ (uint32_t) cells[i].key) * 16777619u;
			sum += c->score;
			++letters;
			good += c->score >= 0.5f;
		}
		while (L->len && L->cp[L->len - 1] == 0x3000) {
			--L->len;
		}
		/* settle doubtful letters with real words: try each one's runners-up
		 * and its forms with and without ゛ ゜, keeping changes that make
		 * more of the line into dictionary words */
		if (o->scorer && L->len) {
			int base = o->scorer(L->cp, L->len, o->scorerUser);
			int li = 0;
			for (int i = 0; i < nc && li < L->len; ++i) {
				if (cells[i].space) {
					if (L->cp[li] == 0x3000) {
						++li;
					}
					continue;
				}
				const struct Cell* c = &cells[i];
				float sc = c->c[choice[i]].score;
				if (!c->known && sc < 0.9f) {
					uint32_t alts[8];
					int na = 0;
					for (int k = 0; k < c->nc; ++k) {
						if (k != choice[i] && c->c[k].score > sc - 0.15f) {
							alts[na++] = c->c[k].cp;
						}
					}
					na += variants(L->cp[li], alts + na);
					uint32_t keep = L->cp[li];
					for (int k = 0; k < na; ++k) {
						L->cp[li] = alts[k];
						int v = o->scorer(L->cp, L->len, o->scorerUser);
						if (v > base) {
							base = v;
							keep = alts[k];
						}
					}
					L->cp[li] = keep;
				}
				++li;
			}
		}
		L->hash = hash;
		L->conf = letters ? sum / letters : 0;
		L->accepted = letters >= 3 && L->conf >= 0.55f && good * 10 >= letters * 6 && lingoJudgeText(L->cp, L->len, false);
		if (L->accepted && learnFromIt && L->conf >= 0.68f) {
			/* remember the letters read surely (seen twice before they count) */
			for (int i = 0; i < nc; ++i) {
				if (!cells[i].space && !cells[i].known && cells[i].c[choice[i]].score >= 0.78f &&
				    (cells[i].nc < 2 || cells[i].c[0].score - cells[i].c[1].score >= 0.06f || choice[i] != 0)) {
					learn(o, cells[i].key, cells[i].c[choice[i]].cp, 1);
				}
			}
		}
		++nOut;
	}
	return nOut;
}

int lingoOcrRead(struct LingoOcr* o, const uint8_t* luma, int w, int h, struct LingoOcrLine* out, int max) {
	if (!o || !luma || w <= 0 || h <= 0 || w > 2048) {
		return 0;
	}
	inkMask(o, luma, w, h);
	struct Band bands[64];
	int nb = findBands(o, w, h, bands, 64);
	int n = 0;
	for (int i = 0; i < nb && n < max; ++i) {
		n += readBand(o, w, bands[i], out + n, max - n, true);
	}
	return n;
}

bool lingoOcrTeach(struct LingoOcr* o, const uint8_t* luma, int w, int h, const uint32_t* typed, int n, char* why, size_t whyLen) {
	if (!o) {
		snprintf(why, whyLen, "Screen reading isn't available.");
		return false;
	}
	uint32_t letters[OCR_MAX_CELLS];
	int nl = 0;
	for (int i = 0; i < n && nl < OCR_MAX_CELLS; ++i) {
		if (typed[i] != ' ' && typed[i] != 0x3000 && typed[i] != '\n') {
			letters[nl++] = typed[i];
		}
	}
	if (nl < 2) {
		snprintf(why, whyLen, "Type the line you can see.");
		return false;
	}
	inkMask(o, luma, w, h);
	struct Band bands[64];
	int nb = findBands(o, w, h, bands, 64);
	/* the line with as many letter cells as letters typed, agreeing best */
	static struct LingoOcrLine lines[32];
	int bestLine = -1, bestAgree = -1;
	struct Band bestBand = { 0, 0 };
	for (int i = 0; i < nb; ++i) {
		int k = readBand(o, w, bands[i], lines, 32, false);
		for (int j = 0; j < k; ++j) {
			int cells = 0, agree = 0;
			for (int c = 0; c < lines[j].len; ++c) {
				if (lines[j].cp[c] != 0x3000) {
					if (cells < nl && lines[j].cp[c] == letters[cells]) {
						++agree;
					}
					++cells;
				}
			}
			if (cells == nl && agree > bestAgree) {
				bestAgree = agree;
				bestLine = j;
				bestBand = bands[i];
				lines[0] = lines[j]; /* keep it */
				bestLine = 0;
			}
		}
	}
	if (bestLine < 0) {
		snprintf(why, whyLen,
		         "Couldn't match that to a line on screen (count the letters: every small っ and mark counts). "
		         "Lingo reads text in plain boxes best.");
		return false;
	}
	/* label the line's letter cells with what was typed */
	struct LingoOcrLine* L = &lines[0];
	int bh = bestBand.y1 - bestBand.y0;
	int idx = 0;
	for (int c = 0; c < L->len && idx < nl; ++c) {
		if (L->cp[c] == 0x3000) {
			continue;
		}
		learn(o, cellKey(o, w, L->cellX[c], L->cellW[c], bestBand.y0, bh), letters[idx++], 5);
	}
	snprintf(why, whyLen, "Learned %d of this game's letters from the screen (%d known now).", idx, lingoOcrLearned(o));
	return idx > 0;
}
