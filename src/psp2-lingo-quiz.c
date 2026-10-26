/* Lingo checkpoints: word book, checkpoint rules and test building. */
#include "psp2-lingo-quiz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lingoBookInit(struct LingoBook* b) {
	memset(b, 0, sizeof(*b));
}

void lingoBookFree(struct LingoBook* b) {
	free(b->items);
	memset(b, 0, sizeof(*b));
}

struct BookHeader {
	char magic[4];
	int32_t n;
	float playSince;
	int32_t wordsSince;
	int32_t passed;
	int32_t pending;
};

bool lingoBookLoad(struct LingoBook* b, const char* path) {
	lingoBookFree(b);
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	struct BookHeader h;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "LBK1", 4) && h.n >= 0 && h.n < 100000;
	if (ok) {
		b->cap = h.n + 64;
		b->items = calloc(b->cap, sizeof(*b->items));
		b->n = b->items ? (int) fread(b->items, sizeof(*b->items), h.n, f) : 0;
		b->playSince = h.playSince;
		b->wordsSince = h.wordsSince;
		b->passed = h.passed;
		b->pending = h.pending != 0;
	}
	fclose(f);
	return ok;
}

bool lingoBookSave(const struct LingoBook* b, const char* path) {
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	struct BookHeader h = { { 'L', 'B', 'K', '1' }, b->n, b->playSince, b->wordsSince, b->passed, b->pending };
	bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && (int) fwrite(b->items, sizeof(*b->items), b->n, f) == b->n;
	fclose(f);
	if (ok) {
		remove(path);
		ok = rename(tmp, path) == 0;
	}
	return ok;
}

static bool isParticleLike(const struct LingoEntry* e, int surfaceLen) {
	if (surfaceLen <= 1) {
		return true;
	}
	if ((e->pos & LPOS_PRT) && !(e->pos & (LPOS_NOUN | LPOS_V1 | LPOS_V5 | LPOS_ADJI))) {
		return true;
	}
	return false;
}

int lingoBookMeet(struct LingoBook* b, const struct LingoDict* d, const struct LingoText* t,
                  const struct LingoToken* tok, int nTok) {
	int fresh = 0;
	char line[LINGO_CTX_BYTES];
	lingoUtf8FromCodepoints(t->cp, t->len, line, sizeof(line));
	for (char* p = line; *p; ++p) {
		if (*p == '\n') {
			*p = ' ';
		}
	}
	for (int i = 0; i < nTok; ++i) {
		if (!tok[i].nIds) {
			continue;
		}
		struct LingoEntry e;
		if (!lingoDictEntry(d, tok[i].ids[0], &e) || isParticleLike(&e, tok[i].len)) {
			continue;
		}
		struct LingoMet* m = NULL;
		for (int k = 0; k < b->n; ++k) {
			if (b->items[k].id == tok[i].ids[0]) {
				m = &b->items[k];
				break;
			}
		}
		if (m) {
			if (m->seen < 0xFFFF) {
				m->seen++;
			}
			continue;
		}
		if (b->n == b->cap) {
			int cap = b->cap ? b->cap * 2 : 128;
			struct LingoMet* it = realloc(b->items, cap * sizeof(*it));
			if (!it) {
				return fresh;
			}
			b->items = it;
			b->cap = cap;
		}
		m = &b->items[b->n++];
		memset(m, 0, sizeof(*m));
		m->id = tok[i].ids[0];
		lingoUtf8FromCodepoints(t->cp + tok[i].start, tok[i].len, m->surface, sizeof(m->surface));
		snprintf(m->ctx, sizeof(m->ctx), "%s", line);
		m->seen = 1;
		m->fresh = 1;
		++fresh;
	}
	b->wordsSince += fresh;
	return fresh;
}

void lingoCheckpointTick(struct LingoBook* b, float dt) {
	if (!b->pending) {
		b->playSince += dt;
	}
}

void lingoCheckpointSaved(struct LingoBook* b) {
	if (b->playSince >= LINGO_CP_MIN_PLAY || b->wordsSince >= LINGO_CP_MIN_WORDS) {
		b->pending = true;
	}
}

void lingoCheckpointPassed(struct LingoBook* b) {
	b->pending = false;
	b->playSince = 0;
	b->wordsSince = 0;
	b->passed++;
	for (int i = 0; i < b->n; ++i) {
		b->items[i].fresh = 0;
	}
}

/* ---- tests ---- */

void lingoShortGloss(const char* gloss, char* out, size_t n) {
	const char* p = gloss;
	if (*p == '[') {
		const char* q = strchr(p, ']');
		if (q) {
			p = q + 1;
		}
	}
	while (*p == ' ') {
		++p;
	}
	if (p[0] == '1' && p[1] == '.') {
		p += 2;
	}
	while (*p == ' ') {
		++p;
	}
	size_t k = 0;
	while (p[k] && p[k] != ';' && !(p[k] == ' ' && p[k + 1] == ' ') && k + 1 < n) {
		++k;
	}
	memcpy(out, p, k);
	out[k] = 0;
}

static unsigned sSeed;
static int rnd(int n) {
	sSeed = sSeed * 1664525 + 1013904223;
	return n > 0 ? (int) ((sSeed >> 8) % (unsigned) n) : 0;
}

static bool hasKanji(const char* s) {
	uint32_t cps[64];
	int n = lingoUtf8Decode(s, cps, 64);
	for (int i = 0; i < n; ++i) {
		if (cps[i] >= 0x4E00 && cps[i] <= 0x9FFF) {
			return true;
		}
	}
	return false;
}

struct Pool {
	char word[64], reading[64], meaning[128], surface[32], ctx[LINGO_CTX_BYTES];
	int met;
};

static bool inOptions(char opts[4][128], int n, const char* s) {
	for (int i = 0; i < n; ++i) {
		if (!strcmp(opts[i], s)) {
			return true;
		}
	}
	return false;
}

/* fill options 1..3 from the pool (field chosen by kind), then random common dictionary words */
static int fillOptions(const struct LingoDict* d, struct Pool* pool, int np, int self, int kind, char opts[4][128]) {
	int n = 1;
	for (int tries = 0; tries < np * 3 && n < 4; ++tries) {
		int j = rnd(np);
		if (j == self) {
			continue;
		}
		const char* s = kind == LQ_MEANING ? pool[j].meaning : kind == LQ_READING ? pool[j].reading : pool[j].surface;
		if (s[0] && !inOptions(opts, n, s) && strcmp(s, opts[0])) {
			snprintf(opts[n++], 128, "%s", s);
		}
	}
	size_t count = lingoDictEntryCount(d);
	for (int tries = 0; tries < 400 && n < 4 && kind != LQ_CLOZE; ++tries) {
		struct LingoEntry e;
		if (!lingoDictEntry(d, (uint32_t) rnd((int) count), &e) || e.rank > 4) {
			continue;
		}
		char s[128];
		if (kind == LQ_MEANING) {
			lingoShortGloss(e.gloss, s, sizeof(s));
		} else {
			if (!hasKanji(e.headword)) {
				continue;
			}
			snprintf(s, sizeof(s), "%s", e.reading);
		}
		if (s[0] && !inOptions(opts, n, s)) {
			snprintf(opts[n++], 128, "%s", s);
		}
	}
	return n;
}

int lingoBuildTest(const struct LingoDict* d, struct LingoBook* b, const struct LingoExtra* extra, int nExtra,
                   unsigned seed, struct LingoQuestion* out, int max) {
	sSeed = seed * 2654435761u + 7;
	if (!d) {
		return 0;
	}
	/* everything that could be asked about */
	int cap = b->n + nExtra;
	if (cap == 0) {
		return 0;
	}
	struct Pool* pool = calloc(cap, sizeof(*pool));
	if (!pool) {
		return 0;
	}
	int np = 0;
	for (int i = 0; i < b->n; ++i) {
		struct LingoEntry e;
		if (!lingoDictEntry(d, b->items[i].id, &e)) {
			continue;
		}
		struct Pool* p = &pool[np++];
		snprintf(p->word, sizeof(p->word), "%s", e.headword);
		snprintf(p->reading, sizeof(p->reading), "%s", e.reading);
		lingoShortGloss(e.gloss, p->meaning, sizeof(p->meaning));
		snprintf(p->surface, sizeof(p->surface), "%s", b->items[i].surface);
		snprintf(p->ctx, sizeof(p->ctx), "%s", b->items[i].ctx);
		p->met = i;
	}
	for (int i = 0; i < nExtra; ++i) {
		struct Pool* p = &pool[np++];
		snprintf(p->word, sizeof(p->word), "%s", extra[i].head);
		snprintf(p->reading, sizeof(p->reading), "%s", extra[i].reading);
		lingoShortGloss(extra[i].gloss, p->meaning, sizeof(p->meaning));
		snprintf(p->surface, sizeof(p->surface), "%s", extra[i].head);
		snprintf(p->ctx, sizeof(p->ctx), "%s", extra[i].ctx ? extra[i].ctx : "");
		p->met = -1;
	}
	/* choose: words met since the last checkpoint (newest first), then the
	 * ones answered wrong before, then saved words, then anything */
	int* order = malloc(np * sizeof(int));
	int no = 0;
	bool* used = calloc(np, sizeof(bool));
	for (int pass = 0; pass < 4 && no < max; ++pass) {
		for (int k = np - 1; k >= 0 && no < max; --k) {
			if (used[k]) {
				continue;
			}
			struct Pool* p = &pool[k];
			bool take = false;
			if (pass == 0) {
				take = p->met >= 0 && b->items[p->met].fresh;
			} else if (pass == 1) {
				take = p->met >= 0 && b->items[p->met].wrong > b->items[p->met].right;
			} else if (pass == 2) {
				take = p->met < 0;
			} else {
				take = true;
			}
			if (pass == 0 && take && rnd(3) == 0 && np > max * 2) {
				take = false; /* some variety when there is plenty */
			}
			if (take) {
				used[k] = true;
				order[no++] = k;
			}
		}
	}
	int nq = 0;
	for (int qi = 0; qi < no; ++qi) {
		struct Pool* p = &pool[order[qi]];
		struct LingoQuestion* q = &out[nq];
		memset(q, 0, sizeof(*q));
		q->met = p->met;
		snprintf(q->word, sizeof(q->word), "%s", p->word);
		snprintf(q->reading, sizeof(q->reading), "%s", p->reading);
		snprintf(q->meaning, sizeof(q->meaning), "%s", p->meaning);
		snprintf(q->context, sizeof(q->context), "%s", p->ctx);
		/* pick a kind that fits this word */
		enum LingoQKind kind = LQ_MEANING;
		char* gap = p->ctx[0] && p->surface[0] ? strstr(p->ctx, p->surface) : NULL;
		if (qi % 3 == 1 && gap && np >= 4) {
			kind = LQ_CLOZE;
		} else if (qi % 3 == 2 && hasKanji(p->word) && strcmp(p->word, p->reading)) {
			kind = LQ_READING;
		}
		q->kind = kind;
		char opts[4][128];
		memset(opts, 0, sizeof(opts));
		switch (kind) {
		case LQ_MEANING:
			snprintf(q->prompt, sizeof(q->prompt), "%s", p->word);
			if (strcmp(p->word, p->reading)) {
				snprintf(q->sub, sizeof(q->sub), "%s", p->reading);
			}
			snprintf(opts[0], 128, "%s", p->meaning);
			break;
		case LQ_READING:
			snprintf(q->prompt, sizeof(q->prompt), "%s", p->word);
			snprintf(q->sub, sizeof(q->sub), "%s", p->meaning);
			snprintf(opts[0], 128, "%s", p->reading);
			break;
		case LQ_CLOZE: {
			size_t before = gap - p->ctx;
			snprintf(q->prompt, sizeof(q->prompt), "%.*s＿＿＿%s", (int) before, p->ctx, gap + strlen(p->surface));
			snprintf(q->sub, sizeof(q->sub), "%s", p->meaning);
			snprintf(opts[0], 128, "%s", p->surface);
			break;
		}
		}
		if (!opts[0][0]) {
			continue;
		}
		int n = fillOptions(d, pool, np, order[qi], kind, opts);
		if (n < 4 && kind == LQ_CLOZE) {
			/* not enough other words seen yet: ask the meaning instead */
			q->kind = LQ_MEANING;
			snprintf(q->prompt, sizeof(q->prompt), "%s", p->word);
			q->sub[0] = 0;
			if (strcmp(p->word, p->reading)) {
				snprintf(q->sub, sizeof(q->sub), "%s", p->reading);
			}
			memset(opts, 0, sizeof(opts));
			snprintf(opts[0], 128, "%s", p->meaning);
			n = fillOptions(d, pool, np, order[qi], LQ_MEANING, opts);
		}
		if (n < 4) {
			continue;
		}
		/* shuffle with the answer tracked */
		int idx[4] = { 0, 1, 2, 3 };
		for (int i = 3; i > 0; --i) {
			int j = rnd(i + 1);
			int t = idx[i];
			idx[i] = idx[j];
			idx[j] = t;
		}
		for (int i = 0; i < 4; ++i) {
			snprintf(q->options[i], 128, "%s", opts[idx[i]]);
			if (idx[i] == 0) {
				q->answer = i;
			}
		}
		++nq;
	}
	free(order);
	free(used);
	free(pool);
	return nq;
}
