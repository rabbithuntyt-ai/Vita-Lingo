/* Lingo: Gen 3 Japanese text decoding and the text-pointer scanner. */
#include "psp2-lingo.h"

#include <string.h>

/* ---- Character set ---- */

static const char* const HIRA =
	"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん"
	"ぁぃぅぇぉゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽっ";
static const char* const KATA =
	"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン"
	"ァィゥェォャュョガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポッ";
/* 0xA1..0xB0 */
static const char* const PUNCT1 = "０１２３４５６７８９！？。ー・‥";
/* 0xB1..0xBA */
static const char* const PUNCT2 = "『』「」♂♀円、×／";

static uint32_t sTable[256];
static bool sTableReady;

static int utf8Next(const char** s, uint32_t* cp) {
	const unsigned char* p = (const unsigned char*) *s;
	if (!*p) {
		return 0;
	}
	if (p[0] < 0x80) {
		*cp = p[0];
		*s += 1;
	} else if ((p[0] & 0xE0) == 0xC0) {
		*cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
		*s += 2;
	} else if ((p[0] & 0xF0) == 0xE0) {
		*cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
		*s += 3;
	} else {
		*cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
		*s += 4;
	}
	return 1;
}

static void fillRun(const char* s, int first) {
	uint32_t cp;
	int i = first;
	while (utf8Next(&s, &cp)) {
		sTable[i++] = cp;
	}
}

static void buildTable(void) {
	memset(sTable, 0, sizeof(sTable));
	sTable[0x00] = 0x3000; /* ideographic space */
	fillRun(HIRA, 0x01);
	fillRun(KATA, 0x51);
	fillRun(PUNCT1, 0xA1);
	fillRun(PUNCT2, 0xB1);
	for (int i = 0; i < 26; ++i) {
		sTable[0xBB + i] = 0xFF21 + i; /* Ａ */
		sTable[0xD5 + i] = 0xFF41 + i; /* ａ */
	}
	sTable[0xEF] = 0x25B6; /* ▶ */
	sTable[0xF0] = 0xFF1A; /* ： */
	sTableReady = true;
}

uint32_t lingoGen3Char(uint8_t b) {
	if (!sTableReady) {
		buildTable();
	}
	return sTable[b];
}

bool lingoIsKana(uint32_t cp) {
	return (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || cp == 0x30FC;
}

uint32_t lingoKataToHira(uint32_t cp) {
	if (cp >= 0x30A1 && cp <= 0x30F6) {
		return cp - 0x60;
	}
	return cp;
}

/* Number of argument bytes after an FC control code. */
static int fcArgs(uint8_t code) {
	switch (code) {
	case 0x04:
		return 3;
	case 0x0B:
	case 0x10:
		return 2;
	case 0x07:
	case 0x09:
	case 0x0A:
	case 0x0F:
	case 0x15:
	case 0x16:
	case 0x17:
	case 0x18:
		return 0;
	default:
		return code <= 0x18 ? 1 : -1;
	}
}

int lingoDecodeGen3(const uint8_t* src, size_t max, uint32_t* out, int outMax) {
	int n = 0;
	int kana = 0;
	size_t i = 0;
	while (i < max) {
		uint8_t b = src[i++];
		uint32_t cp = 0;
		switch (b) {
		case 0xFF:
			while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == 0x3000)) {
				--n; /* trailing breaks and spaces */
			}
			return kana >= 2 ? n : -1;
		case 0xFE:
		case 0xFB:
		case 0xFA:
			cp = '\n';
			break;
		case 0xFC: {
			if (i >= max) {
				return -1;
			}
			int args = fcArgs(src[i++]);
			if (args < 0) {
				return -1;
			}
			i += args;
			continue;
		}
		case 0xFD:
			++i;
			cp = 0xFF0A; /* ＊ for an unexpanded name */
			break;
		default:
			cp = lingoGen3Char(b);
			if (!cp) {
				return -1;
			}
			if (lingoIsKana(cp)) {
				++kana;
			}
			break;
		}
		if (n == 0 && (cp == 0x3000 || cp == '\n')) {
			continue; /* leading padding */
		}
		if (cp == '\n' && n > 0 && out[n - 1] == '\n') {
			continue;
		}
		if (n >= outMax) {
			return -1;
		}
		out[n++] = cp;
	}
	return -1;
}

/* ---- Memory ---- */

const uint8_t* lingoResolve(const struct LingoMem* mem, uint32_t addr, size_t* avail) {
	uint32_t region = addr >> 24;
	uint32_t off;
	switch (region) {
	case 0x02:
		off = addr & 0x00FFFFFF;
		if (!mem->ewram || off >= 0x40000) {
			return NULL;
		}
		*avail = 0x40000 - off;
		return mem->ewram + off;
	case 0x03:
		off = addr & 0x00FFFFFF;
		if (!mem->iwram || off >= 0x8000) {
			return NULL;
		}
		*avail = 0x8000 - off;
		return mem->iwram + off;
	case 0x08:
	case 0x09:
		off = addr - 0x08000000;
		if (!mem->rom || off >= mem->romSize) {
			return NULL;
		}
		*avail = mem->romSize - off;
		return mem->rom + off;
	default:
		return NULL;
	}
}

static bool isTextByte(uint8_t b) {
	return b == 0xFE || b == 0xFD || b == 0xFC || b == 0xFB || b == 0xFA || lingoGen3Char(b) != 0;
}

static uint32_t hashText(const struct LingoText* t) {
	uint32_t h = 2166136261u;
	for (int i = 0; i < t->len; ++i) {
		h = (h ^ t->cp[i]) * 16777619u;
	}
	return h;
}

/* Walks back from addr to the byte after the previous terminator. Stops at
 * long runs of zero bytes, which are cleared buffer rather than text. */
static uint32_t findStart(const struct LingoMem* mem, uint32_t addr) {
	size_t avail;
	uint32_t start = addr;
	int zeros = 0;
	for (int back = 0; back < LINGO_MAX_SCAN_BYTES; ++back) {
		uint32_t a = start - 1;
		const uint8_t* p = lingoResolve(mem, a, &avail);
		if (!p || *p == 0xFF) {
			break;
		}
		if (*p == 0x00) {
			if (++zeros >= 4) {
				break;
			}
		} else {
			zeros = 0;
		}
		start = a;
	}
	return start + (zeros > 0 && zeros < 4 ? 0 : 0);
}

bool lingoExtractMessage(const struct LingoMem* mem, uint32_t addr, uint32_t knownStart, struct LingoText* out) {
	uint32_t start = (knownStart && knownStart <= addr && addr - knownStart < LINGO_MAX_SCAN_BYTES)
		? knownStart : findStart(mem, addr);
	size_t avail;
	const uint8_t* p = lingoResolve(mem, start, &avail);
	if (!p) {
		return false;
	}
	if (avail > LINGO_MAX_SCAN_BYTES) {
		avail = LINGO_MAX_SCAN_BYTES;
	}
	int n = lingoDecodeGen3(p, avail, out->cp, LINGO_MAX_TEXT);
	if (n <= 0) {
		return false;
	}
	/* Text, not bytecode: most visible characters should be kana. */
	int kana = 0, visible = 0;
	for (int i = 0; i < n; ++i) {
		uint32_t c = out->cp[i];
		if (c == '\n' || c == 0x3000) {
			continue;
		}
		++visible;
		if (lingoIsKana(c)) {
			++kana;
		}
	}
	if (kana < 3 || kana * 10 < visible * 6) {
		return false;
	}
	out->len = n;
	out->start = start;
	out->hash = hashText(out);
	return true;
}

/* ---- Scanner ---- */

void lingoScannerReset(struct LingoScanner* s) {
	memset(s, 0, sizeof(*s));
}

static struct LingoCandidate* findCandidate(struct LingoScanner* s, uint32_t addr, bool create) {
	struct LingoCandidate* oldest = &s->cand[0];
	for (int i = 0; i < LINGO_CANDIDATES; ++i) {
		if (s->cand[i].addr == addr) {
			return &s->cand[i];
		}
		if (s->cand[i].lastFrame < oldest->lastFrame) {
			oldest = &s->cand[i];
		}
	}
	if (!create) {
		return NULL;
	}
	memset(oldest, 0, sizeof(*oldest));
	oldest->addr = addr;
	return oldest;
}

static bool plausiblePointer(const struct LingoMem* mem, uint32_t v) {
	size_t avail;
	const uint8_t* p = lingoResolve(mem, v - 1, &avail);
	return p && isTextByte(*p);
}

static bool scanRegion(struct LingoScanner* s, const struct LingoMem* mem, const uint32_t* cur, uint32_t* prev,
                       size_t words, uint32_t base, struct LingoText* out) {
	bool found = false;
	for (size_t i = 0; i < words; ++i) {
		uint32_t v = cur[i];
		uint32_t p = prev[i];
		if (v == p) {
			continue;
		}
		prev[i] = v;
		if (!s->primed) {
			continue;
		}
		uint32_t addr = base + (uint32_t) (i * 4);
		uint32_t delta = v - p;
		if (delta >= 1 && delta <= 4 && (v >> 24) == (p >> 24) && plausiblePointer(mem, v)) {
			struct LingoCandidate* c = findCandidate(s, addr, true);
			if (s->frame - c->lastFrame > 45) {
				c->steps = 0;
				c->ones = 0;
			}
			c->steps++;
			if (delta == 1) {
				c->ones++;
			}
			c->value = v;
			c->lastFrame = s->frame;
			if (!found && c->steps >= 3 && c->ones * 2 >= c->steps) {
				struct LingoText t;
				if (lingoExtractMessage(mem, v - 1, c->msgStart, &t) &&
				    (t.start != s->currentStart || t.hash != s->currentHash)) {
					*out = t;
					s->currentStart = t.start;
					s->currentHash = t.hash;
					found = true;
				}
			}
		} else {
			/* A jump: if this variable was a text pointer, a new message starts here. */
			struct LingoCandidate* c = findCandidate(s, addr, false);
			if (c) {
				c->msgStart = v;
				c->value = v;
				c->steps = 0;
				c->ones = 0;
			}
		}
	}
	return found;
}

bool lingoScannerStep(struct LingoScanner* s, const struct LingoMem* mem, struct LingoText* out) {
	s->frame++;
	bool found = false;
	if (mem->ewram) {
		found = scanRegion(s, mem, (const uint32_t*) mem->ewram, s->prevEwram, 0x40000 / 4, 0x02000000, out);
	}
	if (mem->iwram && !found) {
		found = scanRegion(s, mem, (const uint32_t*) mem->iwram, s->prevIwram, 0x8000 / 4, 0x03000000, out);
	} else if (mem->iwram) {
		/* keep the snapshot current */
		memcpy(s->prevIwram, mem->iwram, sizeof(s->prevIwram));
	}
	s->primed = true;
	return found;
}

/* ---- UTF-8 helpers ---- */

int lingoUtf8Encode(uint32_t cp, char* out) {
	if (cp < 0x80) {
		out[0] = (char) cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char) (0xC0 | (cp >> 6));
		out[1] = (char) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char) (0xE0 | (cp >> 12));
		out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char) (0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char) (0xF0 | (cp >> 18));
	out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char) (0x80 | (cp & 0x3F));
	return 4;
}

int lingoUtf8FromCodepoints(const uint32_t* cp, int len, char* out, size_t outSize) {
	size_t n = 0;
	for (int i = 0; i < len; ++i) {
		char buf[4];
		int k = lingoUtf8Encode(cp[i], buf);
		if (n + k + 1 > outSize) {
			break;
		}
		memcpy(out + n, buf, k);
		n += k;
	}
	out[n] = 0;
	return (int) n;
}

int lingoUtf8Decode(const char* s, uint32_t* out, int outMax) {
	int n = 0;
	uint32_t cp;
	while (n < outMax && utf8Next(&s, &cp)) {
		out[n++] = cp;
	}
	return n;
}
