/* Lingo: working out a game's text encoding from its font (see the header). */
#include "psp2-lingo-tiles.h"
#include "psp2-lingo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int lingoTilesStop;

int lingoTileBytes(enum LingoTileFormat f) {
	switch (f) {
	case LTF_NES:
	case LTF_2BPP:
		return 16;
	case LTF_4BPP_PLANAR:
	case LTF_4BPP_MD:
	case LTF_4BPP_MD_SWAP:
	case LTF_4BPP_GBA:
		return 32;
	case LTF_1BPP:
		return 8;
	default:
		return 16;
	}
}

/* An 8x8 tile's pixel values (0..15). */
static void decode(enum LingoTileFormat f, const uint8_t* p, uint8_t px[64]) {
	for (int r = 0; r < 8; ++r) {
		for (int x = 0; x < 8; ++x) {
			int bit = 7 - x, v = 0;
			switch (f) {
			case LTF_NES:
				v = ((p[r] >> bit) & 1) | (((p[r + 8] >> bit) & 1) << 1);
				break;
			case LTF_2BPP:
				v = ((p[2 * r] >> bit) & 1) | (((p[2 * r + 1] >> bit) & 1) << 1);
				break;
			case LTF_4BPP_PLANAR:
				v = ((p[2 * r] >> bit) & 1) | (((p[2 * r + 1] >> bit) & 1) << 1) | (((p[16 + 2 * r] >> bit) & 1) << 2) |
				    (((p[17 + 2 * r] >> bit) & 1) << 3);
				break;
			case LTF_4BPP_MD:
				v = (x & 1) ? p[4 * r + x / 2] & 15 : p[4 * r + x / 2] >> 4;
				break;
			case LTF_4BPP_MD_SWAP:
				v = (x & 1) ? p[(4 * r + x / 2) ^ 1] & 15 : p[(4 * r + x / 2) ^ 1] >> 4;
				break;
			case LTF_4BPP_GBA:
				v = (x & 1) ? p[4 * r + x / 2] >> 4 : p[4 * r + x / 2] & 15;
				break;
			case LTF_1BPP:
				v = (p[r] >> bit) & 1;
				break;
			default:
				break;
			}
			px[r * 8 + x] = (uint8_t) v;
		}
	}
}

/* The letter's ink: everything not the background (the most common value).
 * Returns the ink count. */
static int inkOf(const uint8_t px[64], uint8_t bits[64]) {
	int count[16] = { 0 };
	for (int i = 0; i < 64; ++i) {
		count[px[i]]++;
	}
	int bg = 0;
	for (int v = 1; v < 16; ++v) {
		if (count[v] > count[bg]) {
			bg = v;
		}
	}
	int n = 0;
	for (int i = 0; i < 64; ++i) {
		bits[i] = px[i] != bg;
		n += bits[i];
	}
	return n;
}

/* ---- the kana orders fonts use ---- */

static const char* const ORDERS[] = {
	/* gojūon, plain letters only */
	"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん",
	"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン",
	/* Unicode / JIS order: small letters and voiced letters in place */
	"ぁあぃいぅうぇえぉおかがきぎくぐけげこごさざしじすずせぜそぞただちぢっつづてでとどなにぬねのはばぱひびぴふぶぷへべぺほぼぽまみむめもゃやゅゆょよらりるれろゎわゐゑをん",
	"ァアィイゥウェエォオカガキギクグケゲコゴサザシジスズセゼソゾタダチヂッツヅテデトドナニヌネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨラリルレロヮワヰヱヲンヴ",
	/* gojūon, then the small letters, then the voiced ones */
	"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんぁぃぅぇぉっゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ",
	"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンァィゥェォッャュョガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ",
	/* voiced letters in a row of their own */
	"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ",
	"ガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ",
};
/* even orders are hiragana, odd katakana; 0-1 plain letters only, 2-5 all
 * letters, 6-7 voiced letters only */
#define ORDER_VOICED_ONLY(k) ((k) >= 6)
#define ORDER_HAS_VOICED(k) ((k) >= 2)
#define NORDERS ((int) (sizeof(ORDERS) / sizeof(ORDERS[0])))

static int utf32(const char* s, uint32_t* out, int max) {
	int n = 0;
	const unsigned char* p = (const unsigned char*) s;
	while (*p && n < max) {
		uint32_t c;
		if (*p < 0x80) {
			c = *p++;
		} else if ((*p & 0xE0) == 0xC0) {
			c = (uint32_t) (*p & 0x1F) << 6 | (p[1] & 0x3F);
			p += 2;
		} else {
			c = (uint32_t) (*p & 0x0F) << 12 | (uint32_t) (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			p += 3;
		}
		out[n++] = c;
	}
	return n;
}

static bool isKana(uint32_t c) {
	return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA);
}

/* ---- scanning ---- */

struct TileRead {
	uint32_t cp[3];
	float sc[3];
	uint8_t n;
	bool blank;
};

/* A cache of recognised ink shapes: video memory repeats letters a lot. */
#define SHAPES 4096
struct Shape {
	uint64_t key;
	struct TileRead r;
	bool used;
};

static uint64_t keyOf(const uint8_t* bits, int n) {
	uint64_t k = 1469598103934665603ULL;
	for (int i = 0; i < n; ++i) {
		k = (k ^ bits[i]) * 1099511628211ULL;
	}
	return k ? k : 1;
}

static void readBits(const struct LingoOcr* o, struct Shape* cache, const uint8_t* bits, int w, int h, struct TileRead* r) {
	uint64_t key = keyOf(bits, w * h) ^ (uint64_t) (w * 31 + h);
	struct Shape* s = &cache[key % SHAPES];
	if (s->used && s->key == key) {
		*r = s->r;
		return;
	}
	memset(r, 0, sizeof(*r));
	r->n = (uint8_t) lingoOcrClassify(o, bits, w, h, true, r->cp, r->sc, 3);
	s->used = true;
	s->key = key;
	s->r = *r;
}

static bool addHit(struct LingoFontScan* s, uint32_t addr, uint32_t cp, float score, bool fromOrder) {
	for (int i = 0; i < s->n; ++i) {
		if (s->hits[i].addr == addr) {
			s->hits[i].cp = cp;
			s->hits[i].score = score;
			s->hits[i].fromOrder = fromOrder;
			return true;
		}
	}
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 256;
		struct LingoTileHit* h = realloc(s->hits, (size_t) cap * sizeof(*h));
		if (!h) {
			return false;
		}
		s->hits = h;
		s->cap = cap;
	}
	s->hits[s->n++] = (struct LingoTileHit) { addr, cp, score, fromOrder };
	return true;
}

/* Where letters sit in a font sheet: a letter is w tiles across; the sheet
 * is S tiles wide, and the next row of letters starts R tiles further on (two
 * tile rows for letters two tiles tall). S = 0: letters simply follow each other. */
struct Sheet {
	int layout, part; /* 0 = 8x8, 1 = 8x16 (bottom half `part` tiles on), 2 = 16x16 (right half +1, bottom +part) */
	int w, S, R;
};

static int letterTile(const struct Sheet* g, int first, int i) {
	if (!g->S) {
		return first + i * g->w;
	}
	int base = first - first % g->S;
	int col = first % g->S + i * g->w;
	return base + (col / g->S) * g->R + col % g->S;
}

/* How well an order fits the reads from tile `first` on: 2 for a best
 * match, 1 for a runner-up. */
static int fitAt(const struct TileRead* rd, int nt, const struct Sheet* g, int first, const uint32_t* order, int len,
                 int* matched) {
	int score = 0, m = 0;
	for (int i = 0; i < len; ++i) {
		int t = letterTile(g, first, i);
		if (t >= nt) {
			break;
		}
		const struct TileRead* r = &rd[t];
		if (r->n && r->cp[0] == order[i]) {
			score += 2;
			++m;
		} else if ((r->n > 1 && r->cp[1] == order[i]) || (r->n > 2 && r->cp[2] == order[i])) {
			score += 1;
			++m;
		}
	}
	*matched = m;
	return score;
}

/* ゛ and ゜ drawn alone in a tile (fonts that put them above the letter):
 * a few pixels (at the top for marks beside the letter, at the bottom for
 * marks drawn on the row above it), two strokes or one small ring.
 * Too small for the recogniser to tell apart from specks. */
static bool voicingMark(const uint8_t bits[64], struct TileRead* r) {
	int x0 = 8, y0 = 8, x1 = -1, y1 = -1, ink = 0;
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			if (bits[y * 8 + x]) {
				++ink;
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
		}
	}
	/* marks are short: a digit or small letter is as narrow but taller */
	if (ink < 2 || ink > 8 || x1 - x0 > 4 || y1 - y0 > 2) {
		return false;
	}
	/* count the strokes (8-connected) */
	uint8_t seen[64] = { 0 };
	int comps = 0;
	for (int i = 0; i < 64; ++i) {
		if (!bits[i] || seen[i]) {
			continue;
		}
		++comps;
		int stack[64], sp = 0;
		stack[sp++] = i;
		seen[i] = 1;
		while (sp) {
			int c = stack[--sp], cx = c % 8, cy = c / 8;
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					int nx = cx + dx, ny = cy + dy;
					if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && bits[ny * 8 + nx] && !seen[ny * 8 + nx]) {
						seen[ny * 8 + nx] = 1;
						stack[sp++] = ny * 8 + nx;
					}
				}
			}
		}
	}
	bool ring = comps == 1 && x1 - x0 >= 2 && y1 - y0 >= 2 && !bits[((y0 + y1) / 2) * 8 + (x0 + x1) / 2];
	if (comps == 2) {
		r->cp[0] = 0x309B;
	} else if (ring) {
		r->cp[0] = 0x309C;
	} else {
		return false;
	}
	r->sc[0] = 0.95f;
	r->n = 1;
	return true;
}

/* Reads every letter-sized picture: 8x8 tiles, and 8x16 / 16x16 letters made
 * of tiles stacked with a row stride. layout: 0 = 8x8; 1 = 8x16 with the
 * bottom half `stride` tiles on; 2 = 16x16 (tiles t, t+1, t+stride, t+stride+1). */
static void readLayout(const struct LingoOcr* o, struct Shape* cache, const uint8_t* data, int nt, int tb,
                       enum LingoTileFormat fmt, int layout, int stride, struct TileRead* rd) {
	uint8_t px[64], a[64], bits[256];
	for (int t = 0; t < nt; ++t) {
		struct TileRead* r = &rd[t];
		memset(r, 0, sizeof(*r));
		if (lingoTilesStop) {
			continue; /* the game is closing: finish quickly with nothing */
		}
		int w = layout == 2 ? 16 : 8, h = layout ? 16 : 8;
		int parts[4] = { t, -1, -1, -1 };
		if (layout == 1) {
			parts[1] = t + stride;
		} else if (layout == 2) {
			parts[1] = t + 1;
			parts[2] = t + stride;
			parts[3] = t + stride + 1;
		}
		int ink = 0;
		bool ok = true;
		memset(bits, 0, sizeof(bits));
		for (int k = 0; k < 4 && ok; ++k) {
			if (parts[k] < 0) {
				continue;
			}
			if (parts[k] >= nt) {
				ok = false;
				break;
			}
			decode(fmt, data + (size_t) parts[k] * tb, px);
			ink += inkOf(px, a);
			int ox = (layout == 2 && (k == 1 || k == 3)) ? 8 : 0;
			int oy = (layout == 1 && k == 1) || (layout == 2 && k >= 2) ? 8 : 0;
			for (int y = 0; y < 8; ++y) {
				memcpy(bits + (oy + y) * w + ox, a + y * 8, 8);
			}
		}
		if (!ok) {
			continue;
		}
		int area = w * h;
		if (ink == 0) {
			r->blank = true;
			continue;
		}
		if (layout == 0 && voicingMark(bits, r)) {
			continue;
		}
		/* too little or too much ink to be a letter */
		if (ink < area / 16 || ink > area * 3 / 4) {
			continue;
		}
		readBits(o, cache, bits, w, h, r);
	}
}

static bool hasHit(const struct LingoFontScan* s, uint32_t addr) {
	for (int i = 0; i < s->n; ++i) {
		if (s->hits[i].addr == addr) {
			return true;
		}
	}
	return false;
}

/* The other tiles of a big letter: on screen they sit next to and under its
 * first tile, and read as nothing (U+200B) so the letter counts once. */
static void addParts(struct LingoFontScan* s, const struct Sheet* g, int t, int nt, int tb) {
	int parts[3] = { -1, -1, -1 };
	if (g->layout == 1) {
		parts[0] = t + g->part;
	} else if (g->layout == 2) {
		parts[0] = t + 1;
		parts[1] = t + g->part;
		parts[2] = t + g->part + 1;
	}
	for (int k = 0; k < 3; ++k) {
		if (parts[k] >= 0 && parts[k] < nt) {
			bool taken = false;
			for (int i = 0; i < s->n && !taken; ++i) {
				taken = s->hits[i].addr == (uint32_t) parts[k] * (uint32_t) tb;
			}
			if (!taken) {
				addHit(s, (uint32_t) parts[k] * (uint32_t) tb, 0x200B, 1.0f, true);
			}
		}
	}
}

int lingoTilesScan(const struct LingoOcr* o, const uint8_t* data, uint32_t size, enum LingoTileFormat fmt,
                   struct LingoFontScan* out) {
	memset(out, 0, sizeof(*out));
	out->fmt = fmt;
	int tb = lingoTileBytes(fmt);
	int nt = (int) (size / (uint32_t) tb);
	if (!o || nt < 16) {
		return 0;
	}
	struct TileRead* rd = calloc((size_t) nt, sizeof(*rd));
	struct Shape* cache = calloc(SHAPES, sizeof(*cache));
	uint32_t orders[NORDERS][160];
	int olen[NORDERS];
	for (int k = 0; k < NORDERS; ++k) {
		olen[k] = utf32(ORDERS[k], orders[k], 160);
	}
	if (!rd || !cache) {
		free(rd);
		free(cache);
		return 0;
	}
	/* layouts to try, smallest first; stop at the first that a kana order fits */
	static const struct Sheet LAYOUT[] = {
		{ 0, 0, 1, 0, 0 },    /* 8x8 */
		{ 1, 16, 1, 16, 32 }, /* 8x16, halves a sheet row apart (16 or 32 tiles wide) */
		{ 1, 32, 1, 32, 64 },
		{ 1, 1, 2, 0, 0 },    /* 8x16, halves next to each other */
		{ 2, 16, 2, 16, 32 }, /* 16x16 in a sheet 16 or 32 tiles wide */
		{ 2, 32, 2, 32, 64 },
	};
	int bestLayout = -1;
	for (size_t li = 0; li < sizeof(LAYOUT) / sizeof(LAYOUT[0]); ++li) {
		const struct Sheet* g = &LAYOUT[li];
		int layout = g->layout, stride = g->part;
		readLayout(o, cache, data, nt, tb, fmt, layout, stride, rd);
		/* fit the orders: the one matching the most letters claims its tiles,
		 * then the next among what is left (an order with the small and voiced
		 * letters beats the plain one when the font has them) */
		bool* claimed = calloc((size_t) nt, 1);
		bool used[NORDERS] = { false };
		int fitted = 0, anchor = -1;
		while (claimed) {
			int bk = -1, bestFirst = -1, bestM = 0;
			for (int k = 0; k < NORDERS; ++k) {
				if (used[k]) {
					continue;
				}
				for (int first = 0; first < nt; ++first) {
					if (claimed[first] || !rd[first].n) {
						continue;
					}
					int m;
					fitAt(rd, nt, g, first, orders[k], olen[k], &m);
					/* believable: at least 15 letters, and over 40% of the order */
					if (m > bestM && m >= 15 && m * 5 >= olen[k] * 2) {
						bool overlap = false;
						for (int i = 0; i < olen[k] && letterTile(g, first, i) < nt && !overlap; ++i) {
							overlap = claimed[letterTile(g, first, i)];
						}
						if (!overlap) {
							bestM = m;
							bestFirst = first;
							bk = k;
						}
					}
				}
			}
			if (bk < 0) {
				break;
			}
			int k = bk;
			/* the same letters in another order (plain gojūon inside the full one) can't fit twice */
			for (int j = 0; j < NORDERS; ++j) {
				if ((j & 1) != (k & 1)) {
					continue;
				}
				if (ORDER_VOICED_ONLY(k) ? ORDER_HAS_VOICED(j) : (!ORDER_VOICED_ONLY(j) || ORDER_HAS_VOICED(k))) {
					used[j] = true;
				}
			}
			++fitted;
			if (anchor < 0) {
				anchor = bestFirst;
			}
#ifdef LINGO_DEBUG
			fprintf(stderr, "    fmt %d layout %d: order %d fits at tile %d (%d of %d letters)\n", fmt, (int) li, k, bestFirst, bestM, olen[k]);
#endif
			/* which letters of the order the pictures agree with */
			bool agree[160] = { false };
			int lastAgree = -1;
			for (int i = 0; i < olen[k]; ++i) {
				int t = letterTile(g, bestFirst, i);
				if (t >= nt) {
					break;
				}
				const struct TileRead* r = &rd[t];
				uint32_t cp = orders[k][i];
				agree[i] = r->n && (r->cp[0] == cp || (r->n > 1 && r->cp[1] == cp) || (r->n > 2 && r->cp[2] == cp));
				if (agree[i]) {
					lastAgree = i;
				}
			}
			for (int i = 0; i <= lastAgree; ++i) {
				int t = letterTile(g, bestFirst, i);
				const struct TileRead* r = &rd[t];
				if (!r->n) {
					continue; /* blank or not a letter: the font skips one */
				}
				if (r->n == 1 && (r->cp[0] == 0x309B || r->cp[0] == 0x309C)) {
					continue; /* a voicing mark, found by its shape */
				}
				uint32_t cp = orders[k][i];
				bool agrees = agree[i];
				if (!agrees) {
					/* the order fills a misread letter only between letters it got right */
					bool near = (i > 0 && agree[i - 1]) || (i > 1 && agree[i - 2]);
					bool after = agree[i + 1] || (i + 2 <= lastAgree && agree[i + 2]);
					if (!near || !after) {
						continue;
					}
					/* and not when the picture clearly says otherwise */
					if (r->sc[0] >= 0.8f) {
						cp = r->cp[0];
					}
				}
				claimed[t] = true;
				addHit(out, (uint32_t) t * (uint32_t) tb, cp, agrees ? 1.0f : r->sc[0], true);
				addParts(out, g, t, nt, tb);
			}
		}
		if (fitted) {
			/* the rest of the font: punctuation, digits, letters out of order */
			for (int t = 0; t < nt; ++t) {
				const struct TileRead* r = &rd[t];
				if (claimed[t] || !r->n || hasHit(out, (uint32_t) t * (uint32_t) tb)) {
					continue;
				}
				/* big letters keep the font's grid */
				if (g->w == 2 && (t - anchor) % 2) {
					continue;
				}
				if (g->S && ((t / g->S - anchor / g->S) % (g->R / g->S))) {
					continue;
				}
				bool kana = isKana(r->cp[0]);
				float need = kana ? 0.82f : 0.86f;
				if (r->sc[0] >= need && r->sc[0] - (r->n > 1 ? r->sc[1] : 0) > 0.02f) {
					addHit(out, (uint32_t) t * (uint32_t) tb, r->cp[0], r->sc[0], false);
					addParts(out, g, t, nt, tb);
				}
			}
			out->orderRuns = fitted;
			bestLayout = (int) li;
		}
		free(claimed);
		if (bestLayout >= 0) {
			break;
		}
	}
	/* ゛ drawn so small its strokes touch can't be told from a speck, but
	 * fonts keep it next to ゜: a small mark beside a ゜ is a ゛ */
	for (int i = 0; i < out->n; ++i) {
		if (out->hits[i].cp != 0x309C) {
			continue;
		}
		int t = (int) (out->hits[i].addr / (uint32_t) tb);
		for (int d = -1; d <= 1; d += 2) {
			int u = t + d;
			if (u < 0 || u >= nt || hasHit(out, (uint32_t) u * (uint32_t) tb)) {
				continue;
			}
			uint8_t px[64], bits[64];
			decode(fmt, data + (size_t) u * tb, px);
			int ink = inkOf(px, bits), x0 = 8, y0 = 8, x1 = -1, y1 = -1;
			for (int k = 0; k < 64; ++k) {
				if (bits[k]) {
					x0 = k % 8 < x0 ? k % 8 : x0;
					x1 = k % 8 > x1 ? k % 8 : x1;
					y0 = k / 8 < y0 ? k / 8 : y0;
					y1 = k / 8 > y1 ? k / 8 : y1;
				}
			}
			if (ink >= 2 && ink <= 8 && x1 - x0 <= 4 && y1 - y0 <= 2) {
				addHit(out, (uint32_t) u * (uint32_t) tb, 0x309B, 0.9f, false);
				break;
			}
		}
		break;
	}
	/* blank tiles next to the font are its space */
	if (out->n) {
		uint32_t lo = UINT32_MAX, hi = 0;
		for (int i = 0; i < out->n; ++i) {
			lo = out->hits[i].addr < lo ? out->hits[i].addr : lo;
			hi = out->hits[i].addr > hi ? out->hits[i].addr : hi;
		}
		readLayout(o, cache, data, nt, tb, fmt, 0, 0, rd);
		uint32_t from = lo >= (uint32_t) tb * 64 ? lo - (uint32_t) tb * 64 : 0;
		for (uint32_t a = from; a <= hi + (uint32_t) tb * 64 && a / tb < (uint32_t) nt && out->nBlank < 64; a += (uint32_t) tb) {
			if (rd[a / tb].blank) {
				out->blank[out->nBlank++] = a;
			}
		}
	}
	for (int i = 0; i < out->n; ++i) {
		out->kana += isKana(out->hits[i].cp);
	}
	free(rd);
	free(cache);
	return out->n;
}

void lingoTilesFree(struct LingoFontScan* s) {
	free(s->hits);
	memset(s, 0, sizeof(*s));
}

int lingoTilesTable(const struct LingoFontScan* s, uint32_t base, enum LingoCodeMode mode, uint16_t mask, uint16_t stride,
                    struct LingoTable* t) {
	lingoTableInit(t, mode, mask);
	t->stride = stride;
	int tb = lingoTileBytes(s->fmt);
	int limit = mode == LCM_BYTE ? 256 : mask + 1;
	int n = 0;
	for (int i = 0; i < s->n + s->nBlank; ++i) {
		uint32_t addr = i < s->n ? s->hits[i].addr : s->blank[i - s->n];
		uint32_t cp = i < s->n ? s->hits[i].cp : 0x3000;
		long d = ((long) addr - (long) base) / tb;
		/* one byte of tile number may count from the middle of a block (the
		 * Game Boy's second tile mode), so -128..-1 wrap to 128..255 */
		if (mode == LCM_BYTE ? (d < -128 || d >= limit) : (d < 0 || d >= limit)) {
			continue;
		}
		uint16_t code = (uint16_t) (mode == LCM_BYTE ? (d & 0xFF) : d);
		if (cp == 0x309B || cp == 0xFF9E) {
			t->dakuten = code;
			continue;
		}
		if (cp == 0x309C || cp == 0xFF9F) {
			t->handakuten = code;
			continue;
		}
		if (t->map[code]) {
			continue;
		}
		lingoTableSet(t, code, cp);
		n += cp != 0x3000 && cp != 0x200B;
	}
	snprintf(t->source, sizeof(t->source), "read from its font");
	return n;
}

/* ---- judging a reading ---- */

/* Letters of cp[0..n) in dictionary words (particles count alone). */
static int knownLetters(const struct LingoDict* d, const uint32_t* cp, int n) {
	static const uint32_t PARTICLES[] = { 0x306F, 0x304C, 0x3092, 0x306B, 0x3067, 0x306E, 0x3068, 0x3082, 0x3078, 0x3084,
		                                  0x304B, 0x3088, 0x306D, 0x306A, 0x3055, 0x305E };
	static struct LingoToken tok[LINGO_MAX_TOKENS];
	int k = lingoTokenize(d, cp, n, tok, LINGO_MAX_TOKENS);
	int known = 0;
	for (int i = 0; i < k; ++i) {
		if (!tok[i].nIds) {
			continue;
		}
		if (tok[i].len >= 2) {
			known += tok[i].len;
		} else {
			for (size_t p = 0; p < sizeof(PARTICLES) / sizeof(PARTICLES[0]); ++p) {
				if (cp[tok[i].start] == PARTICLES[p]) {
					known += 1;
					break;
				}
			}
		}
	}
	return known;
}

/* Is this phrase (no blanks in it) believable Japanese? */
static bool phraseOk(const struct LingoDict* d, const uint32_t* cp, int n, int* known) {
	*known = knownLetters(d, cp, n);
	if (n < 3) {
		return *known == n;
	}
	if (!lingoJudgeText(cp, n, true)) {
		return false;
	}
	/* which letters follow which: real lines average -4 or better in the
	 * bigram table (1/8 bits), random kana -15 and worse */
	int bsum = 0, bpairs = 0, bprev = -1;
	for (int i = 0; i < n; ++i) {
		int b = lingoBigramIndex(cp[i]);
		if (b >= 0 && bprev >= 0) {
			bsum += LINGO_BIGRAM[bprev][b];
			++bpairs;
		}
		bprev = b;
	}
	if (bpairs >= 5 && bsum < -8 * bpairs) {
		return false;
	}
	/* hiragana and katakana come in stretches; random codes flip between them */
	int flips = 0, script = 0;
	for (int i = 0; i < n; ++i) {
		int sc = cp[i] >= 0x3041 && cp[i] <= 0x309F ? 1 : cp[i] >= 0x30A1 && cp[i] <= 0x30FA ? 2 : 0;
		if (sc && script && sc != script) {
			++flips;
		}
		if (sc) {
			script = sc;
		}
	}
	if (flips * 4 > n) {
		return false;
	}
	/* graphics read as text: every letter twice (both bit planes the same) */
	int pairs = 0;
	for (int i = 0; i + 1 < n; ++i) {
		pairs += cp[i] == cp[i + 1];
	}
	if (n >= 4 && pairs * 10 >= n * 3) {
		return false;
	}
	/* one letter over and over isn't a sentence */
	for (int i = 0; i < n && n >= 6; ++i) {
		int same = 0;
		for (int j = 0; j < n; ++j) {
			same += cp[j] == cp[i];
		}
		if (same * 10 >= n * 4) {
			return false;
		}
	}
	/* mostly words: random kana make a scattering of short words but plenty of letters in none */
	return *known * 10 >= n * 7;
}

int lingoTilesWordScore(const struct LingoDict* d, const uint32_t* cp, int n) {
	if (!d) {
		return 0;
	}
	/* phrase by phrase: a text box pads its lines with blanks, and the row
	 * under a line may be anything */
	int total = 0, shortKnown = 0;
	bool any = false;
	for (int i = 0; i < n;) {
		while (i < n && (cp[i] == 0x3000 || cp[i] == '\n' || cp[i] == ' ')) {
			++i;
		}
		int j = i;
		while (j < n && cp[j] != 0x3000 && cp[j] != '\n' && cp[j] != ' ') {
			++j;
		}
		if (j > i) {
			int known;
			if (phraseOk(d, cp + i, j - i, &known)) {
				if (j - i >= 3) {
					total += known;
					any = true;
				} else {
					shortKnown += known;
				}
			}
		}
		i = j;
	}
	return any ? total + shortKnown : 0;
}

/* ---- per console ---- */

static const struct LingoRegion* regionNamed(const struct LingoRegions* rs, const char* name) {
	for (int i = 0; i < rs->n; ++i) {
		if (!strcmp(rs->r[i].name, name) && rs->r[i].data && rs->r[i].size) {
			return &rs->r[i];
		}
	}
	return NULL;
}

struct Finder {
	const struct LingoRegions* rs;
	LingoTilesScorer scorer;
	void* user;
	struct LingoTable* best;
	struct LingoTable* scratch;
	int bestScore;
	char what[96];
};

static void topAdd(int* top, int v) {
	for (int k = 0; k < 3; ++k) {
		if (v > top[k]) {
			int tmp = top[k];
			top[k] = v;
			v = tmp;
		}
	}
}

/* Try the table in scratch on memory as it is now; keep it if it reads best.
 * Memory is read in slices: through a wrong-ish table, graphics make plenty
 * of short "lines", and one pass over a whole region would run out of room
 * before reaching the text. */
static void tryTable(struct Finder* f, const char* what) {
	static struct LingoText lines[48];
	/* the three best lines: a dialogue box holds a few long lines of real
	 * words, while random bytes read through a wrong table make scraps */
	int top[3] = { 0, 0, 0 };
	int nLines = 0;
	const uint32_t SLICE = 0x2000, OVER = 0x400;
	for (int ri = 0; ri < f->rs->n; ++ri) {
		const struct LingoRegion* r = &f->rs->r[ri];
		if (f->scratch->region[0] && strcmp(f->scratch->region, r->name)) {
			continue;
		}
		for (uint32_t off = 0; off < r->size; off += SLICE) {
			struct LingoRegions one;
			lingoRegionsClear(&one);
			uint32_t len = r->size - off < SLICE + OVER ? r->size - off : SLICE + OVER;
			lingoRegionsAdd(&one, r->name, r->data + off, len);
			int n = lingoReaderFindAll(f->scratch, &one, lines, 48);
			nLines += n;
			for (int i = 0; i < n; ++i) {
				int v = f->scorer(lines[i].cp, lines[i].len, f->user);
#ifdef LINGO_DEBUG
				if (getenv("TILES_LINES")) {
					char u[400];
					lingoUtf8FromCodepoints(lines[i].cp, lines[i].len, u, sizeof(u));
					fprintf(stderr, "      %s+%X: %d %s\n", r->name, off, v, u);
				}
#endif
				if (v >= 6) {
					topAdd(top, v * v);
				}
			}
		}
	}
	int score = top[0] + top[1] + top[2];
#ifdef LINGO_DEBUG
	fprintf(stderr, "    candidate %-32s mode %d stride %d: %d lines, score %d (dakuten %X)\n", what, f->scratch->mode,
	        f->scratch->stride, nLines, score, f->scratch->dakuten);
#endif
	if (score > f->bestScore) {
		f->bestScore = score;
		memcpy(f->best, f->scratch, sizeof(*f->best));
		snprintf(f->what, sizeof(f->what), "%s", what);
	}
}

int lingoTilesReadAll(const struct LingoTable* t, const struct LingoRegions* rs, struct LingoText* out, int max) {
	const uint32_t SLICE = 0x2000, OVER = 0x400;
	int k = 0;
	for (int ri = 0; ri < rs->n && k < max; ++ri) {
		const struct LingoRegion* r = &rs->r[ri];
		if (t->region[0] && strcmp(t->region, r->name)) {
			continue;
		}
		for (uint32_t off = 0; off < r->size && k < max; off += SLICE) {
			struct LingoRegions one;
			lingoRegionsClear(&one);
			uint32_t len = r->size - off < SLICE + OVER ? r->size - off : SLICE + OVER;
			lingoRegionsAdd(&one, r->name, r->data + off, len);
			k += lingoReaderFindAll(t, &one, out + k, max - k);
		}
	}
	return k;
}

/* A byte table two ways: on all memory as plain text (no rows), and on video
 * memory as a tile map `stride` tiles wide (marks above letters, rows). */
static void tryByte(struct Finder* f, const struct LingoFontScan* s, uint32_t base, uint16_t stride, const char* what) {
	char w[80];
	if (lingoTilesTable(s, base, LCM_BYTE, 0xFFFF, 0, f->scratch) < 15) {
		return;
	}
	snprintf(w, sizeof(w), "bytes, %s", what);
	tryTable(f, w);
	lingoTilesTable(s, base, LCM_BYTE, 0xFFFF, stride, f->scratch);
	snprintf(f->scratch->region, sizeof(f->scratch->region), "vram");
	snprintf(w, sizeof(w), "the screen's tile map, %s", what);
	tryTable(f, w);
}

/* Candidates for a font scanned from video memory: tile numbers counted from
 * each possible start of the tile area, read as tile-map entries and as bytes. */
static void tryBases(struct Finder* f, const struct LingoFontScan* s, uint32_t step, uint32_t span, uint16_t mapMask,
                     enum LingoCodeMode mapMode, uint16_t stride) {
	if (!s->n) {
		return;
	}
	uint32_t lo = UINT32_MAX, hi = 0;
	for (int i = 0; i < s->n; ++i) {
		lo = s->hits[i].addr < lo ? s->hits[i].addr : lo;
		hi = s->hits[i].addr > hi ? s->hits[i].addr : hi;
	}
	for (uint32_t base = 0; base <= lo; base += step) {
		if (hi - base >= span && step) {
			continue; /* this base can't number the whole font */
		}
		char what[64];
		if (mapMode != LCM_BYTE) {
			lingoTilesTable(s, base, mapMode, mapMask, stride, f->scratch);
			snprintf(what, sizeof(what), "tile map, tiles from %X", base);
			tryTable(f, what);
		}
		snprintf(what, sizeof(what), "tiles from %X", base);
		tryByte(f, s, base, stride, what);
		if (!step) {
			break;
		}
	}
}

int lingoTilesFind(const struct LingoOcr* o, const char* system, const struct LingoRegions* rs, const uint8_t* rom,
                   size_t romSize, LingoTilesScorer scorer, void* user, struct LingoTable* best,
                   struct LingoTable* scratch, char* why, size_t whyLen) {
	struct Finder f = { rs, scorer, user, best, scratch, 0, "" };
	struct LingoFontScan s;
	memset(&s, 0, sizeof(s));
	const struct LingoRegion* vram = regionNamed(rs, "vram");
	int letters = 0;
	if (!strcmp(system, "fc")) {
		if (vram && vram->size >= 0x2000) {
			/* the pattern tables as banked now (Lingo's patched NES core) */
			if (lingoTilesScan(o, vram->data, 0x2000, LTF_NES, &s)) {
				letters += s.n;
				tryByte(&f, &s, 0, 32, "pattern table 0");
				tryByte(&f, &s, 0x1000, 32, "pattern table 1");
			}
			lingoTilesFree(&s);
		}
		if (!f.bestScore && rom && romSize > 16 && !memcmp(rom, "NES\x1a", 4) && rom[5]) {
			/* CHR ROM in the cartridge, 4 KB banks */
			size_t chr = 16 + ((rom[6] & 4) ? 512 : 0) + (size_t) rom[4] * 16384;
			size_t len = (size_t) rom[5] * 8192;
			for (size_t b = 0; chr + b + 0x1000 <= romSize && b < len; b += 0x1000) {
				if (lingoTilesScan(o, rom + chr + b, 0x1000, LTF_NES, &s)) {
					letters += s.n;
					tryByte(&f, &s, 0, 32, "a CHR ROM bank");
				}
				lingoTilesFree(&s);
			}
		}
	} else if (!strcmp(system, "gb") && vram) {
		if (lingoTilesScan(o, vram->data, vram->size < 0x1800 ? vram->size : 0x1800, LTF_2BPP, &s)) {
			letters += s.n;
			tryByte(&f, &s, 0, 32, "tiles from 8000");
			tryByte(&f, &s, 0x1000, 32, "tiles from 9000");
		}
		lingoTilesFree(&s);
	} else if (!strcmp(system, "sfc") && vram) {
		static const enum LingoTileFormat F[] = { LTF_2BPP, LTF_4BPP_PLANAR };
		for (int i = 0; i < 2; ++i) {
			if (lingoTilesScan(o, vram->data, vram->size, F[i], &s)) {
				letters += s.n;
				tryBases(&f, &s, 0x2000, 1024u * (uint32_t) lingoTileBytes(F[i]), 0x3FF, LCM_LE16, 32);
			}
			lingoTilesFree(&s);
		}
	} else if (!strcmp(system, "pce") && vram) {
		if (lingoTilesScan(o, vram->data, vram->size, LTF_4BPP_PLANAR, &s)) {
			letters += s.n;
			tryBases(&f, &s, 0, 0, 0xFFF, LCM_LE16, 32);
			lingoTilesTable(&s, 0, LCM_LE16, 0xFFF, 64, scratch);
			tryTable(&f, "tile map, 64 wide");
		}
		lingoTilesFree(&s);
	} else if (!strcmp(system, "md") && vram) {
		static const enum LingoTileFormat F[] = { LTF_4BPP_MD_SWAP, LTF_4BPP_MD };
		for (int i = 0; i < 2; ++i) {
			if (lingoTilesScan(o, vram->data, vram->size, F[i], &s)) {
				letters += s.n;
				tryBases(&f, &s, 0, 0, 0x7FF, F[i] == LTF_4BPP_MD_SWAP ? LCM_LE16 : LCM_BE16, 64);
				lingoTilesTable(&s, 0, F[i] == LTF_4BPP_MD_SWAP ? LCM_LE16 : LCM_BE16, 0x7FF, 32, scratch);
				tryTable(&f, "tile map, 32 wide");
				lingoTilesFree(&s);
				break;
			}
			lingoTilesFree(&s);
		}
	} else if (!strcmp(system, "gba") && vram) {
		if (lingoTilesScan(o, vram->data, vram->size < 0x10000 ? vram->size : 0x10000, LTF_4BPP_GBA, &s)) {
			letters += s.n;
			tryBases(&f, &s, 0x4000, 1024u * 32, 0x3FF, LCM_LE16, 32);
		}
		lingoTilesFree(&s);
	} else if (!strcmp(system, "ws")) {
		const struct LingoRegion* ram = regionNamed(rs, "ram");
		if (ram && ram->size >= 0x4000 && lingoTilesScan(o, ram->data + 0x2000, 0x2000, LTF_2BPP, &s)) {
			letters += s.n;
			for (int i = 0; i < s.n; ++i) {
				s.hits[i].addr += 0x2000;
			}
			for (int i = 0; i < s.nBlank; ++i) {
				s.blank[i] += 0x2000;
			}
			tryBases(&f, &s, 0x2000, 512u * 16, 0x1FF, LCM_LE16, 32);
		}
		lingoTilesFree(&s);
	} else if (!strcmp(system, "msx") && vram) {
		if (lingoTilesScan(o, vram->data, vram->size < 0x4000 ? vram->size : 0x4000, LTF_1BPP, &s)) {
			letters += s.n;
			tryBases(&f, &s, 0x800, 256u * 8, 0xFF, LCM_BYTE, 32);
		}
		lingoTilesFree(&s);
	} else {
		snprintf(why, whyLen, "This console's fonts aren't read yet.");
		return 0;
	}
	if (!letters) {
		snprintf(why, whyLen, "No font found in video memory yet.");
		return 0;
	}
	if (!f.bestScore) {
		snprintf(why, whyLen, "Found a font (%d letters) but no text written with it yet.", letters);
		return 0;
	}
	snprintf(why, whyLen, "Read the game's font (%d letters): its text is %s.", letters, f.what);
	return f.bestScore;
}
