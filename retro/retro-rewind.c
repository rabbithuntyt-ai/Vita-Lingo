/* Lingo libretro front end: rewind (see retro-rewind.h).
 *
 * The ring holds deltas: delta[k] = state[k] XOR state[k-1], packed as runs
 * of (zero count, literal count, literals). "cur" is the newest state;
 * popping XORs the newest delta into it to get the state before. */
#include "retro-rewind.h"

#include <stdlib.h>
#include <string.h>

struct Entry {
	uint32_t off; /* in ring */
	uint32_t len;
};

#define MAX_ENTRIES 4096

struct RetroRewind {
	size_t size;
	uint8_t* cur;    /* newest state */
	uint8_t* scratch;
	bool have;
	uint8_t* ring;
	size_t cap;
	struct Entry e[MAX_ENTRIES];
	int head, count; /* e[(head - 1)] is the newest delta */
	size_t used;     /* bytes between oldest and newest entry */
};

struct RetroRewind* rwNew(size_t stateSize, size_t budget) {
	if (!stateSize || budget < stateSize * 3) {
		return NULL;
	}
	struct RetroRewind* r = calloc(1, sizeof(*r));
	if (!r) {
		return NULL;
	}
	r->size = stateSize;
	r->cur = malloc(stateSize);
	r->scratch = malloc(stateSize * 2 + 16);
	r->cap = budget - stateSize * 3;
	r->ring = malloc(r->cap);
	if (!r->cur || !r->scratch || !r->ring) {
		rwFree(r);
		return NULL;
	}
	return r;
}

void rwFree(struct RetroRewind* r) {
	if (!r) {
		return;
	}
	free(r->cur);
	free(r->scratch);
	free(r->ring);
	free(r);
}

int rwCount(const struct RetroRewind* r) {
	return r ? r->count : 0;
}

/* packs a XOR b into out; returns length */
static size_t pack(const uint8_t* a, const uint8_t* b, size_t n, uint8_t* out) {
	size_t o = 0, i = 0;
	while (i < n) {
		uint32_t zeros = 0;
		while (i < n && a[i] == b[i] && zeros < 0xFFFFFF) {
			++zeros;
			++i;
		}
		uint32_t lit = 0;
		size_t s = i;
		while (i < n && lit < 0xFFFFFF) {
			/* end the literal run at 8 equal bytes, they pack better as zeros */
			if (a[i] == b[i]) {
				size_t k = i;
				while (k < n && k - i < 8 && a[k] == b[k]) {
					++k;
				}
				if (k - i >= 8 || k == n) {
					break;
				}
			}
			++lit;
			++i;
		}
		memcpy(out + o, &zeros, 3);
		memcpy(out + o + 3, &lit, 3);
		o += 6;
		for (uint32_t k = 0; k < lit; ++k) {
			out[o++] = a[s + k] ^ b[s + k];
		}
	}
	return o;
}

/* XORs a packed delta into state */
static void apply(uint8_t* state, size_t n, const uint8_t* d, size_t len) {
	size_t i = 0, p = 0;
	while (p + 6 <= len && i <= n) {
		uint32_t zeros = 0, lit = 0;
		memcpy(&zeros, d + p, 3);
		memcpy(&lit, d + p + 3, 3);
		p += 6;
		i += zeros;
		for (uint32_t k = 0; k < lit && i < n && p < len; ++k) {
			state[i++] ^= d[p++];
		}
	}
}

static void dropOldest(struct RetroRewind* r) {
	int oldest = (r->head - r->count + MAX_ENTRIES) % MAX_ENTRIES;
	r->used -= r->e[oldest].len;
	r->count--;
}

void rwPush(struct RetroRewind* r, const uint8_t* state) {
	if (!r) {
		return;
	}
	if (!r->have) {
		memcpy(r->cur, state, r->size);
		r->have = true;
		return;
	}
	/* the delta that takes the new state back to the old one */
	size_t len = pack(state, r->cur, r->size, r->scratch);
	if (len > r->cap / 4) {
		/* too big to be worth it (a scene change); start over from here */
		r->count = 0;
		r->used = 0;
		memcpy(r->cur, state, r->size);
		return;
	}
	/* make room */
	while (r->count && (r->used + len > r->cap || r->count >= MAX_ENTRIES)) {
		dropOldest(r);
	}
	uint32_t off = 0;
	if (r->count) {
		int newest = (r->head - 1 + MAX_ENTRIES) % MAX_ENTRIES;
		off = r->e[newest].off + r->e[newest].len;
		if (off + len > r->cap) {
			off = 0; /* wrap */
		}
		/* drop entries the new one would overwrite */
		while (r->count) {
			int oldest = (r->head - r->count + MAX_ENTRIES) % MAX_ENTRIES;
			uint32_t o = r->e[oldest].off, l = r->e[oldest].len;
			bool overlap = !(off + len <= o || o + l <= off);
			if (!overlap) {
				break;
			}
			dropOldest(r);
		}
	}
	memcpy(r->ring + off, r->scratch, len);
	r->e[r->head].off = off;
	r->e[r->head].len = (uint32_t) len;
	r->head = (r->head + 1) % MAX_ENTRIES;
	r->count++;
	r->used += len;
	memcpy(r->cur, state, r->size);
}

bool rwPop(struct RetroRewind* r, uint8_t* out) {
	if (!r || !r->count) {
		return false;
	}
	r->head = (r->head - 1 + MAX_ENTRIES) % MAX_ENTRIES;
	struct Entry* e = &r->e[r->head];
	apply(r->cur, r->size, r->ring + e->off, e->len);
	r->used -= e->len;
	r->count--;
	memcpy(out, r->cur, r->size);
	return true;
}
