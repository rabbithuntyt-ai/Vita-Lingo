/* Lingo: offline dictionary, deinflection and tokenizing. */
#include "psp2-lingo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct LingoDict {
	uint8_t* data;
	size_t size;
	bool owned;
	uint32_t nEntries;
	uint32_t nKeys;
	const uint8_t* entries;
	const uint8_t* keys;
	const uint8_t* ids;
	const char* strings;
};

static uint32_t rd32(const uint8_t* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t rd16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

struct LingoDict* lingoDictFromMemory(uint8_t* data, size_t size, bool owned) {
	if (size < 32 || memcmp(data, "LDIC", 4) != 0 || rd32(data + 4) != 1) {
		return NULL;
	}
	struct LingoDict* d = calloc(1, sizeof(*d));
	if (!d) {
		return NULL;
	}
	d->data = data;
	d->size = size;
	d->owned = owned;
	d->nEntries = rd32(data + 8);
	d->nKeys = rd32(data + 12);
	uint32_t offE = rd32(data + 16), offK = rd32(data + 20), offI = rd32(data + 24), offS = rd32(data + 28);
	if (offE > size || offK > size || offI > size || offS > size ||
	    (uint64_t) offE + (uint64_t) d->nEntries * 16 > offK || (uint64_t) offK + (uint64_t) d->nKeys * 12 > offI) {
		free(d);
		return NULL;
	}
	d->entries = data + offE;
	d->keys = data + offK;
	d->ids = data + offI;
	d->strings = (const char*) data + offS;
	return d;
}

struct LingoDict* lingoDictLoad(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t* data = malloc(size + 1);
	if (!data) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(data, 1, size, f);
	fclose(f);
	data[size] = 0;
	if (got != (size_t) size) {
		free(data);
		return NULL;
	}
	struct LingoDict* d = lingoDictFromMemory(data, size, true);
	if (!d) {
		free(data);
	}
	return d;
}

void lingoDictFree(struct LingoDict* d) {
	if (!d) {
		return;
	}
	if (d->owned) {
		free(d->data);
	}
	free(d);
}

size_t lingoDictEntryCount(const struct LingoDict* d) {
	return d ? d->nEntries : 0;
}

bool lingoDictEntry(const struct LingoDict* d, uint32_t id, struct LingoEntry* e) {
	if (!d || id >= d->nEntries) {
		return false;
	}
	const uint8_t* p = d->entries + id * 16;
	e->headword = d->strings + rd32(p);
	e->reading = d->strings + rd32(p + 4);
	e->gloss = d->strings + rd32(p + 8);
	e->pos = rd16(p + 12);
	e->rank = rd16(p + 14);
	return true;
}

static int lookupUtf8(const struct LingoDict* d, const char* key, uint32_t* ids, int max) {
	uint32_t lo = 0, hi = d->nKeys;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		const uint8_t* k = d->keys + mid * 12;
		int c = strcmp(d->strings + rd32(k), key);
		if (c == 0) {
			uint32_t first = rd32(k + 4);
			int count = rd16(k + 8);
			int n = 0;
			for (int i = 0; i < count && n < max; ++i) {
				ids[n++] = rd32(d->ids + (first + i) * 4);
			}
			return n;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return 0;
}

int lingoDictLookup(const struct LingoDict* d, const uint32_t* key, int keyLen, uint32_t* ids, int max) {
	if (!d || keyLen <= 0) {
		return 0;
	}
	char buf[128];
	size_t n = 0;
	for (int i = 0; i < keyLen; ++i) {
		if (n + 5 >= sizeof(buf)) {
			return 0;
		}
		n += lingoUtf8Encode(lingoKataToHira(key[i]), buf + n);
	}
	buf[n] = 0;
	return lookupUtf8(d, buf, ids, max);
}

/* ---- Deinflection ---- */

enum {
	T_INIT = 1 << 11, /* the surface form: any part of speech may match */
	T_TE = 1 << 12,   /* a bare te-form left after removing an auxiliary */
	T_V1 = LPOS_V1,
	T_V5 = LPOS_V5,
	T_A = LPOS_ADJI,
	T_K = LPOS_VK,
	T_S = LPOS_VS,
};

struct Rule {
	const char* from;
	const char* to;
	uint16_t in;
	uint16_t out;
	const char* reason;
	uint32_t fromCp[8];
	uint32_t toCp[8];
	int fromLen;
	int toLen;
};

#define MAX_RULES 512
static struct Rule sRules[MAX_RULES];
static int sNRules;
static char sRulePool[16384];
static size_t sPoolUsed;

static const char* pooled(const char* a, const char* b) {
	size_t la = strlen(a), lb = strlen(b);
	if (sPoolUsed + la + lb + 1 > sizeof(sRulePool)) {
		return "";
	}
	char* p = sRulePool + sPoolUsed;
	memcpy(p, a, la);
	memcpy(p + la, b, lb);
	p[la + lb] = 0;
	sPoolUsed += la + lb + 1;
	return p;
}

static void add(const char* from, const char* to, uint16_t in, uint16_t out, const char* reason) {
	if (sNRules < MAX_RULES) {
		struct Rule* r = &sRules[sNRules++];
		r->from = from;
		r->to = to;
		r->in = in;
		r->out = out;
		r->reason = reason;
		r->fromLen = lingoUtf8Decode(from, r->fromCp, 8);
		r->toLen = lingoUtf8Decode(to, r->toCp, 8);
	}
}

/* Godan endings: dictionary form, i-stem, a-stem, e-stem, o-stem (volitional), past, te */
struct Godan {
	const char *u, *i, *a, *e, *o, *ta, *te;
};
static const struct Godan GODAN[] = {
	{ "う", "い", "わ", "え", "おう", "った", "って" },
	{ "く", "き", "か", "け", "こう", "いた", "いて" },
	{ "ぐ", "ぎ", "が", "げ", "ごう", "いだ", "いで" },
	{ "す", "し", "さ", "せ", "そう", "した", "して" },
	{ "つ", "ち", "た", "て", "とう", "った", "って" },
	{ "ぬ", "に", "な", "ね", "のう", "んだ", "んで" },
	{ "ぶ", "び", "ば", "べ", "ぼう", "んだ", "んで" },
	{ "む", "み", "ま", "め", "もう", "んだ", "んで" },
	{ "る", "り", "ら", "れ", "ろう", "った", "って" },
};

static const struct {
	const char* suffix;
	const char* reason;
	uint16_t in;
	uint16_t outExtra; /* the form itself inflects further as this type */
} STEM_SUFFIXES[] = {
	{ "ます", "polite", T_INIT, 0 },
	{ "ました", "polite past", T_INIT, 0 },
	{ "ません", "polite negative", T_INIT, 0 },
	{ "ませんでした", "polite negative past", T_INIT, 0 },
	{ "ましょう", "polite volitional", T_INIT, 0 },
	{ "ましょうか", "shall we?", T_INIT, 0 },
	{ "たい", "want to", T_INIT | T_A, 0 },
	{ "ながら", "while", T_INIT, 0 },
	{ "なさい", "command", T_INIT, 0 },
};

static void buildRules(void) {
	if (sNRules) {
		return;
	}
	/* i-adjectives (also negative ない and desiderative たい, which inflect the same way) */
	const uint16_t AIN = T_INIT | T_A;
	add("くない", "い", AIN, T_A, "negative");
	add("かった", "い", AIN, T_A, "past");
	add("くなかった", "い", AIN, T_A, "negative past");
	add("くて", "い", AIN, T_A, "te-form");
	add("ければ", "い", AIN, T_A, "if");
	add("かったら", "い", AIN, T_A, "if / when");
	add("く", "い", AIN, T_A, "adverb");
	add("そう", "い", T_INIT, T_A, "looks like");
	add("すぎる", "い", T_INIT | T_V1, T_A, "too much");
	add("くなる", "い", T_INIT | T_V5, T_A, "becomes");

	/* ichidan */
	add("ない", "る", AIN, T_V1, "negative");
	add("た", "る", T_INIT, T_V1, "past");
	add("て", "る", T_INIT | T_TE, T_V1, "te-form");
	add("たら", "る", T_INIT, T_V1, "if / when");
	add("たり", "る", T_INIT, T_V1, "-tari");
	add("れば", "る", T_INIT, T_V1, "if");
	add("よう", "る", T_INIT, T_V1, "let's");
	add("ろ", "る", T_INIT, T_V1, "command");
	add("られる", "る", T_INIT | T_V1, T_V1, "potential / passive");
	add("させる", "る", T_INIT | T_V1, T_V1, "causative");
	add("ず", "る", T_INIT, T_V1, "without");
	add("ずに", "る", T_INIT, T_V1, "without");

	/* godan */
	for (size_t g = 0; g < sizeof(GODAN) / sizeof(GODAN[0]); ++g) {
		const struct Godan* G = &GODAN[g];
		add(pooled(G->a, "ない"), G->u, AIN, T_V5, "negative");
		add(pooled(G->a, "ず"), G->u, T_INIT, T_V5, "without");
		add(pooled(G->a, "れる"), G->u, T_INIT | T_V1, T_V5, "passive");
		add(pooled(G->a, "せる"), G->u, T_INIT | T_V1, T_V5, "causative");
		add(pooled(G->e, "る"), G->u, T_INIT | T_V1, T_V5, "can");
		add(pooled(G->e, "ば"), G->u, T_INIT, T_V5, "if");
		add(G->e, G->u, T_INIT, T_V5, "command");
		add(G->o, G->u, T_INIT, T_V5, "let's");
		add(G->ta, G->u, T_INIT, T_V5, "past");
		add(G->te, G->u, T_INIT | T_TE, T_V5, "te-form");
		add(pooled(G->ta, "ら"), G->u, T_INIT, T_V5, "if / when");
		add(pooled(G->ta, "り"), G->u, T_INIT, T_V5, "-tari");
	}
	/* iku is irregular in the past and te-form */
	add("いった", "いく", T_INIT, T_V5, "past");
	add("いって", "いく", T_INIT | T_TE, T_V5, "te-form");
	add("いったら", "いく", T_INIT, T_V5, "if / when");

	/* kuru and suru */
	add("こない", "くる", AIN, T_K, "negative");
	add("きた", "くる", T_INIT, T_K, "past");
	add("きて", "くる", T_INIT | T_TE, T_K, "te-form");
	add("きたら", "くる", T_INIT, T_K, "if / when");
	add("くれば", "くる", T_INIT, T_K, "if");
	add("こよう", "くる", T_INIT, T_K, "let's");
	add("こい", "くる", T_INIT, T_K, "come!");
	add("こられる", "くる", T_INIT | T_V1, T_K, "can come");
	add("しない", "する", AIN, T_S, "negative");
	add("した", "する", T_INIT, T_S, "past");
	add("して", "する", T_INIT | T_TE, T_S, "te-form");
	add("したら", "する", T_INIT, T_S, "if / when");
	add("すれば", "する", T_INIT, T_S, "if");
	add("しよう", "する", T_INIT, T_S, "let's");
	add("しろ", "する", T_INIT, T_S, "command");
	add("できる", "する", T_INIT | T_V1, T_S, "can");
	add("される", "する", T_INIT | T_V1, T_S, "passive");
	add("させる", "する", T_INIT | T_V1, T_S, "causative");

	/* masu-stem auxiliaries */
	for (size_t s = 0; s < sizeof(STEM_SUFFIXES) / sizeof(STEM_SUFFIXES[0]); ++s) {
		const char* x = STEM_SUFFIXES[s].suffix;
		const char* why = STEM_SUFFIXES[s].reason;
		uint16_t in = STEM_SUFFIXES[s].in;
		add(x, "る", in, T_V1, why);
		for (size_t g = 0; g < sizeof(GODAN) / sizeof(GODAN[0]); ++g) {
			add(pooled(GODAN[g].i, x), GODAN[g].u, in, T_V5, why);
		}
		add(pooled("き", x), "くる", in, T_K, why);
		add(pooled("し", x), "する", in, T_S, why);
	}

	/* te-form auxiliaries leave a bare te-form for the rules above */
	add("ている", "て", T_INIT | T_V1, T_TE, "-ing");
	add("ていた", "て", T_INIT, T_TE, "was -ing");
	add("てる", "て", T_INIT | T_V1, T_TE, "-ing");
	add("でいる", "で", T_INIT | T_V1, T_TE, "-ing");
	add("てください", "て", T_INIT, T_TE, "please");
	add("でください", "で", T_INIT, T_TE, "please");
	add("てしまう", "て", T_INIT | T_V5, T_TE, "completely");
	add("ちゃう", "て", T_INIT | T_V5, T_TE, "completely");
	add("じゃう", "で", T_INIT | T_V5, T_TE, "completely");
	add("ておく", "て", T_INIT | T_V5, T_TE, "in advance");
	add("てみる", "て", T_INIT | T_V1, T_TE, "try");
	add("てくる", "て", T_INIT | T_K, T_TE, "comes to");
	add("ていく", "て", T_INIT | T_V5, T_TE, "goes on");
	add("てあげる", "て", T_INIT | T_V1, T_TE, "for someone");
	add("てくれる", "て", T_INIT | T_V1, T_TE, "for me");
	add("てもらう", "て", T_INIT | T_V5, T_TE, "have done");
	add("ても", "て", T_INIT, T_TE, "even if");
	add("でも", "で", T_INIT, T_TE, "even if");
	add("ん", "ない", T_INIT, T_A, "negative (casual)");
	add("ねえ", "ない", T_INIT, T_A, "negative (rough)");
}

struct Cand {
	uint32_t cp[24];
	int len;
	uint16_t type;
	char reason[LINGO_MAX_REASON];
};

static bool endsWith(const uint32_t* cp, int len, const uint32_t* suf, int slen) {
	if (slen > len) {
		return false;
	}
	for (int i = 0; i < slen; ++i) {
		if (cp[len - slen + i] != suf[i]) {
			return false;
		}
	}
	return true;
}

/* Expands word into its possible dictionary forms. Returns the number of
 * candidates; cands[0] is always the word itself. */
static int deinflect(const uint32_t* word, int len, struct Cand* cands, int max) {
	buildRules();
	int n = 0;
	if (len > 24) {
		return 0;
	}
	memcpy(cands[0].cp, word, len * sizeof(uint32_t));
	cands[0].len = len;
	cands[0].type = T_INIT;
	cands[0].reason[0] = 0;
	n = 1;
	for (int c = 0; c < n; ++c) {
		for (int r = 0; r < sNRules && n < max; ++r) {
			const struct Rule* R = &sRules[r];
			if (!(cands[c].type & R->in)) {
				continue;
			}
			const uint32_t* from = R->fromCp;
			const uint32_t* to = R->toCp;
			int fl = R->fromLen;
			int tl = R->toLen;
			if (fl > cands[c].len || from[fl - 1] != cands[c].cp[cands[c].len - 1] ||
			    !endsWith(cands[c].cp, cands[c].len, from, fl)) {
				continue;
			}
			int nl = cands[c].len - fl + tl;
			if (nl > 24 || nl < 1) {
				continue;
			}
			struct Cand* x = &cands[n];
			memcpy(x->cp, cands[c].cp, (cands[c].len - fl) * sizeof(uint32_t));
			memcpy(x->cp + cands[c].len - fl, to, tl * sizeof(uint32_t));
			x->len = nl;
			x->type = R->out;
			bool dup = false;
			for (int k = 0; k < n; ++k) {
				if (cands[k].len == nl && cands[k].type == x->type && !memcmp(cands[k].cp, x->cp, nl * sizeof(uint32_t))) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			if (cands[c].reason[0]) {
				snprintf(x->reason, sizeof(x->reason), "%s, %s", R->reason, cands[c].reason);
			} else {
				snprintf(x->reason, sizeof(x->reason), "%s", R->reason);
			}
			++n;
		}
	}
	return n;
}

static bool posFits(uint16_t candType, uint16_t pos) {
	if (candType & T_INIT) {
		return true;
	}
	if (candType & T_TE) {
		return false;
	}
	return (candType & pos) != 0;
}

/* ---- Tokenizer ---- */

static bool isBreak(uint32_t cp) {
	return cp == '\n' || cp == 0x3000 || cp == ' ' || cp == 0x3002 || cp == 0x3001 || cp == 0xFF01 || cp == 0xFF1F ||
	       cp == 0x300C || cp == 0x300D || cp == 0x300E || cp == 0x300F || cp == 0x2025 || cp == 0x30FB ||
	       cp == 0xFF0A || cp == 0x25B6 || cp == 0xFF1A;
}

#define MAX_CANDS 160

int lingoTokenize(const struct LingoDict* d, const uint32_t* cp, int len, struct LingoToken* tokens, int maxTokens) {
	static struct Cand cands[MAX_CANDS];
	int nt = 0;
	int i = 0;
	while (i < len && nt < maxTokens) {
		struct LingoToken* t = &tokens[nt];
		memset(t, 0, sizeof(*t));
		t->start = i;
		if (isBreak(cp[i])) {
			t->len = 1;
			++nt;
			++i;
			continue;
		}
		int chunkEnd = i;
		while (chunkEnd < len && !isBreak(cp[chunkEnd])) {
			++chunkEnd;
		}
		int maxLen = chunkEnd - i;
		if (maxLen > 16) {
			maxLen = 16;
		}
		bool matched = false;
		for (int L = maxLen; L >= 1 && !matched; --L) {
			uint32_t hira[24];
			for (int k = 0; k < L; ++k) {
				hira[k] = lingoKataToHira(cp[i + k]);
			}
			int nc = deinflect(hira, L, cands, MAX_CANDS);
			/* Gather every reading of this span, then keep the most common. */
			struct { uint32_t id; uint16_t rank; int cand; } pool[24];
			int np = 0;
			for (int c = 0; c < nc; ++c) {
				uint32_t ids[8];
				int found = lingoDictLookup(d, cands[c].cp, cands[c].len, ids, 8);
				for (int f = 0; f < found && np < 24; ++f) {
					struct LingoEntry e;
					if (!lingoDictEntry(d, ids[f], &e) || !posFits(cands[c].type, e.pos)) {
						continue;
					}
					bool dup = false;
					for (int q = 0; q < np; ++q) {
						if (pool[q].id == ids[f]) {
							dup = true;
						}
					}
					if (!dup) {
						pool[np].id = ids[f];
						pool[np].rank = e.rank;
						pool[np].cand = c;
						++np;
					}
				}
			}
			if (!np) {
				continue;
			}
			/* stable sort by rank: surface forms win ties */
			for (int a = 1; a < np; ++a) {
				for (int b = a; b > 0 && pool[b].rank < pool[b - 1].rank; --b) {
					__typeof__(pool[0]) tmp = pool[b];
					pool[b] = pool[b - 1];
					pool[b - 1] = tmp;
				}
			}
			int kept = np < LINGO_MAX_IDS ? np : LINGO_MAX_IDS;
			for (int k = 0; k < kept; ++k) {
				t->ids[k] = pool[k].id;
				snprintf(t->reason[k], LINGO_MAX_REASON, "%s", cands[pool[k].cand].reason);
			}
			t->nIds = kept;
			t->len = L;
			t->isWord = true;
			matched = true;
		}
		if (!matched) {
			/* Unknown: swallow a run of the same script (often a name) */
			int L = 1;
			bool kata = cp[i] >= 0x30A0 && cp[i] <= 0x30FF;
			while (i + L < chunkEnd && kata && cp[i + L] >= 0x30A0 && cp[i + L] <= 0x30FF) {
				++L;
			}
			t->len = L;
			t->isWord = kata && L > 1;
		}
		i += t->len;
		++nt;
	}
	return nt;
}
