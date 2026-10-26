/* Lingo: live text reading for any game (see psp2-lingo-reader.h). */
#include "psp2-lingo-reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- kana helpers ---- */

static const char* const BASIC_H = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん";
static const char* const BASIC_K = "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン";
#define BASIC_N 46

/* What fonts commonly put straight after ん (hiragana; katakana fonts mirror it). */
static const char* const EXT_H[] = {
	"ぁぃぅぇぉゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽっ", /* Pokémon Gen 3 */
	"っゃゅょ",                                                               /* Pokémon Gen 1/2 */
	"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽぁぃぅぇぉっゃゅょ",
	"ぁぃぅぇぉっゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ",
	"っゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ",
	"ゃゅょっがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ",
	"ぁぃぅぇぉっゃゅょ",
	"っゃゅょぁぃぅぇぉ",
};
#define EXT_N ((int) (sizeof(EXT_H) / sizeof(EXT_H[0])))

static int utf8Next(const char** s, uint32_t* cp) {
	const unsigned char* p = (const unsigned char*) *s;
	if (!*p) {
		return 0;
	}
	if (p[0] < 0x80) {
		*cp = p[0];
		*s += 1;
	} else if ((p[0] & 0xE0) == 0xC0) {
		*cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
		*s += 2;
	} else if ((p[0] & 0xF0) == 0xE0) {
		*cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
		*s += 3;
	} else {
		*cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
		*s += 4;
	}
	return 1;
}

static int toCodepoints(const char* s, uint32_t* out, int max) {
	int n = 0;
	uint32_t cp;
	while (n < max && utf8Next(&s, &cp)) {
		out[n++] = cp;
	}
	return n;
}

int lingoBigramIndex(uint32_t cp) {
	if (cp == 0x30FC) {
		return LINGO_BIGRAM_N - 1;
	}
	if (cp >= 0x30A1 && cp <= 0x30F3) {
		cp -= 0x60;
	}
	if (cp >= 0x3041 && cp <= 0x3093) {
		return (int) (cp - 0x3041);
	}
	return -1;
}

static bool isKanji(uint32_t cp) {
	return (cp >= 0x4E00 && cp <= 0x9FFF) || cp == 0x3005;
}

static bool isJpPunct(uint32_t cp) {
	return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF5E) || cp == 0x30FB || cp == 0x2026 ||
	       cp == 0x22EF || cp == 0x2025;
}

/* か -> が, は -> ば; 0 if the letter takes no mark */
static uint32_t voiced(uint32_t cp) {
	uint32_t base = cp >= 0x30A1 ? cp - 0x60 : cp;
	uint32_t off = cp >= 0x30A1 ? 0x60 : 0;
	if (base == 0x3046) {
		return off ? 0x30F4 : 0x3094; /* ゔ */
	}
	if (base >= 0x304B && base <= 0x3062 && (base - 0x304B) % 2 == 0) {
		return cp + 1; /* か..ぢ */
	}
	if (base >= 0x3064 && base <= 0x3069 && (base == 0x3064 || base == 0x3066 || base == 0x3068)) {
		return cp + 1; /* つ て と */
	}
	if (base >= 0x306F && base <= 0x307B && (base - 0x306F) % 3 == 0) {
		return cp + 1; /* は ひ ふ へ ほ */
	}
	(void) off;
	return 0;
}

static uint32_t semiVoiced(uint32_t cp) {
	uint32_t base = cp >= 0x30A1 ? cp - 0x60 : cp;
	if (base >= 0x306F && base <= 0x307B && (base - 0x306F) % 3 == 0) {
		return cp + 2;
	}
	return 0;
}

/* が -> か (and the mark it lost: 1 dakuten, 2 handakuten) */
static uint32_t unvoice(uint32_t cp, int* mark) {
	*mark = 0;
	uint32_t base = cp >= 0x30A1 ? cp - 0x60 : cp;
	uint32_t off = cp >= 0x30A1 ? 0x60 : 0;
	if (base == 0x3094) {
		*mark = 1;
		return 0x3046 + off;
	}
	if (base >= 0x3041 && base <= 0x3093) {
		for (int d = 1; d <= 2; ++d) {
			uint32_t b = cp - (uint32_t) d;
			if (d == 1 && voiced(b) == cp) {
				*mark = 1;
				return b;
			}
			if (d == 2 && semiVoiced(b) == cp) {
				*mark = 2;
				return b;
			}
		}
	}
	return cp;
}

/* ---- Shift-JIS ---- */

uint32_t lingoSjis(uint8_t lead, uint8_t trail) {
	int row;
	if (lead >= 0x81 && lead <= 0x9F) {
		row = lead - 0x81;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		row = 31 + lead - 0xE0;
	} else {
		return 0;
	}
	if (trail < 0x40 || trail > 0xFC || trail == 0x7F) {
		return 0;
	}
	return LINGO_SJIS[row * 189 + (trail - 0x40)];
}

static const char* const HALFKANA = /* 0xA1..0xDF as full width */
	"。「」、・ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン゛゜";
static uint32_t sHalf[64];
static bool sHalfReady;

static uint32_t halfKana(uint8_t b) {
	if (!sHalfReady) {
		toCodepoints(HALFKANA, sHalf, 63);
		sHalfReady = true;
	}
	return b >= 0xA1 && b <= 0xDF ? sHalf[b - 0xA1] : 0;
}

/* ---- regions ---- */

void lingoRegionsClear(struct LingoRegions* rs) {
	memset(rs, 0, sizeof(*rs));
}

void lingoRegionsAdd(struct LingoRegions* rs, const char* name, const void* data, size_t size) {
	if (!data || !size || rs->n >= LR_MAX_REGIONS) {
		return;
	}
	for (int i = 0; i < rs->n; ++i) {
		if (rs->r[i].data == data) {
			return; /* the same memory reached two ways */
		}
	}
	struct LingoRegion* r = &rs->r[rs->n++];
	r->data = data;
	r->size = (uint32_t) (size > 0x1000000 ? 0x1000000 : size);
	snprintf(r->name, sizeof(r->name), "%s", name);
}

/* ---- tables ---- */

void lingoTableInit(struct LingoTable* t, enum LingoCodeMode mode, uint16_t mask) {
	memset(t, 0, sizeof(*t));
	t->mode = (uint8_t) mode;
	t->mask = mode == LCM_BYTE ? 0xFF : (mask ? mask : 0xFFFF);
	t->dakuten = t->handakuten = LT_NONE;
}

void lingoTableSet(struct LingoTable* t, uint16_t code, uint32_t cp) {
	if (cp > 0xFFFF) {
		return;
	}
	if (!t->map[code] && cp) {
		t->nMapped++;
	} else if (t->map[code] && !cp) {
		t->nMapped--;
	}
	t->map[code] = (uint16_t) cp;
}

void lingoTableSetRun(struct LingoTable* t, uint16_t first, const char* utf8) {
	uint32_t cp;
	uint32_t code = first;
	while (utf8Next(&utf8, &cp) && code <= 0xFFFF) {
		lingoTableSet(t, (uint16_t) code++, cp);
	}
}

static const char* modeName(int m) {
	switch (m) {
	case LCM_LE16:
		return "le16";
	case LCM_BE16:
		return "be16";
	case LCM_SJIS:
		return "sjis";
	default:
		return "byte";
	}
}

static void parseHeader(struct LingoTable* t, const char* line) {
	char key[32], val[24];
	const char* p = line;
	while (*p) {
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		int k = 0, v = 0;
		while (*p && *p != '=' && *p != ' ' && k < 31) {
			key[k++] = *p++;
		}
		key[k] = 0;
		if (*p != '=') {
			while (*p && *p != ' ') {
				++p;
			}
			continue;
		}
		++p;
		while (*p && *p != ' ' && *p != '\r' && *p != '\n' && v < 23) {
			val[v++] = *p++;
		}
		val[v] = 0;
		unsigned long n = strtoul(val, NULL, 16);
		if (!strcmp(key, "mode")) {
			int m = !strcmp(val, "le16") ? LCM_LE16 : !strcmp(val, "be16") ? LCM_BE16 : !strcmp(val, "sjis") ? LCM_SJIS : LCM_BYTE;
			t->mode = (uint8_t) m;
			if (m == LCM_BYTE) {
				t->mask = 0xFF;
			}
		} else if (!strcmp(key, "mask")) {
			t->mask = (uint16_t) n;
		} else if (!strcmp(key, "region")) {
			snprintf(t->region, sizeof(t->region), "%.15s", val);
		} else if (!strcmp(key, "stride")) {
			t->stride = (uint16_t) strtoul(val, NULL, 10);
		} else if (!strcmp(key, "dakuten")) {
			t->dakuten = (uint16_t) n;
		} else if (!strcmp(key, "handakuten")) {
			t->handakuten = (uint16_t) n;
		} else if (!strcmp(key, "source")) {
			snprintf(t->source, sizeof(t->source), "%s", val);
			for (char* s = t->source; *s; ++s) {
				if (*s == '_') {
					*s = ' ';
				}
			}
		}
	}
}

bool lingoTableParse(struct LingoTable* t, const char* text) {
	lingoTableInit(t, LCM_BYTE, 0xFF);
	snprintf(t->source, sizeof(t->source), "table file");
	const char* p = text;
	bool any = false;
	while (*p) {
		const char* e = p;
		while (*e && *e != '\n') {
			++e;
		}
		char line[256];
		size_t n = (size_t) (e - p) < sizeof(line) - 1 ? (size_t) (e - p) : sizeof(line) - 1;
		memcpy(line, p, n);
		line[n] = 0;
		while (n && line[n - 1] == '\r') {
			line[--n] = 0;
		}
		p = *e ? e + 1 : e;
		if (!strncmp(line, ";lingo", 6)) {
			parseHeader(t, line + 6);
			any = true;
			continue;
		}
		if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '@') {
			continue;
		}
		if (line[0] == '*' || line[0] == '/') {
			/* *XX: line break; /XX: end of string (treated as not-a-character) */
			unsigned long code = strtoul(line + 1, NULL, 16);
			if (line[0] == '*' && t->nNewline < 4) {
				t->newline[t->nNewline++] = (uint16_t) code;
			}
			any = true;
			continue;
		}
		char* eq = strchr(line, '=');
		if (!eq || eq == line) {
			continue;
		}
		*eq = 0;
		size_t hexLen = strlen(line);
		unsigned long code = strtoul(line, NULL, 16);
		if (hexLen > 4) {
			continue; /* multi-byte sequences beyond one unit are not supported */
		}
		if (hexLen > 2 && t->mode == LCM_BYTE) {
			continue;
		}
		const char* val = eq + 1;
		uint32_t cp;
		if (!*val) {
			continue;
		}
		const char* q = val;
		utf8Next(&q, &cp);
		if (*q) {
			/* "nn=ポケモン" style dictionary entries: keep the first character only
			 * when the rest is a lone dakuten, otherwise skip */
			uint32_t next;
			const char* r = q;
			utf8Next(&r, &next);
			if (!*r && (next == 0x309B || next == 0xFF9E) && voiced(cp)) {
				cp = voiced(cp);
			} else if (!*r && (next == 0x309C || next == 0xFF9F) && semiVoiced(cp)) {
				cp = semiVoiced(cp);
			} else {
				continue;
			}
		}
		if (cp == ' ') {
			cp = 0x3000;
		}
		lingoTableSet(t, (uint16_t) code, cp);
		any = true;
	}
	return any && (t->nMapped > 0 || t->mode == LCM_SJIS);
}

bool lingoTableLoad(struct LingoTable* t, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 4 * 1024 * 1024) {
		fclose(f);
		return false;
	}
	char* buf = malloc((size_t) n + 1);
	bool ok = false;
	if (buf && fread(buf, 1, (size_t) n, f) == (size_t) n) {
		buf[n] = 0;
		/* skip a UTF-8 byte-order mark */
		ok = lingoTableParse(t, (unsigned char) buf[0] == 0xEF ? buf + 3 : buf);
	}
	free(buf);
	fclose(f);
	return ok;
}

bool lingoTableSave(const struct LingoTable* t, const char* path) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	char src[32];
	snprintf(src, sizeof(src), "%s", t->source);
	for (char* s = src; *s; ++s) {
		if (*s == ' ') {
			*s = '_';
		}
	}
	fprintf(f, "; Character table written by Lingo. Atlas/Thingy format: CODE=character.\n");
	fprintf(f, ";lingo mode=%s mask=%x stride=%u", modeName(t->mode), t->mask, t->stride);
	if (t->region[0]) {
		fprintf(f, " region=%s", t->region);
	}
	if (t->dakuten != LT_NONE) {
		fprintf(f, " dakuten=%x", t->dakuten);
	}
	if (t->handakuten != LT_NONE) {
		fprintf(f, " handakuten=%x", t->handakuten);
	}
	fprintf(f, " source=%s\n", src[0] ? src : "saved");
	int digits = t->mode == LCM_BYTE ? 2 : 4;
	for (int i = 0; i < t->nNewline; ++i) {
		fprintf(f, "*%0*X\n", digits, t->newline[i]);
	}
	for (uint32_t c = 0; c <= (t->mode == LCM_BYTE ? 0xFFu : 0xFFFFu); ++c) {
		if (t->map[c]) {
			char u[8];
			int n = lingoUtf8Encode(t->map[c] == 0x3000 ? ' ' : t->map[c], u);
			u[n] = 0;
			fprintf(f, "%0*X=%s\n", digits, c, u);
		}
	}
	bool ok = !ferror(f);
	fclose(f);
	if (ok) {
		remove(path);
		ok = rename(tmp, path) == 0;
	}
	return ok;
}

/* Japanese Pokémon Red/Green/Blue/Yellow and Gold/Silver/Crystal: the font
 * order from pret's disassemblies (constants/charmap.asm). The games write
 * the screen into a 20x18 buffer in work RAM, with ゛ and ゜ on the row above
 * the letter. */
static void presetPokemonGb(struct LingoTable* t) {
	lingoTableInit(t, LCM_BYTE, 0xFF);
	snprintf(t->source, sizeof(t->source), "built in");
	snprintf(t->region, sizeof(t->region), "wram");
	t->stride = 20;
	t->dakuten = 0xE5;
	t->handakuten = 0xE4;
	/* katakana: the font reuses へ and り for ヘ and リ */
	lingoTableSetRun(t, 0x80, "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフホマミムメモヤユヨラルレロワヲン");
	lingoTableSetRun(t, 0xAC, "ッャュョィ");
	lingoTableSetRun(t, 0xB1, BASIC_H);
	lingoTableSetRun(t, 0xDF, "っゃゅょー");
	lingoTableSetRun(t, 0xE6, "？！。ァゥェ");
	lingoTableSetRun(t, 0x70, "「」");
	lingoTableSet(t, 0x73, 0x300F); /* 』 */
	lingoTableSet(t, 0x74, 0x30FB); /* ・ */
	lingoTableSet(t, 0x75, 0x2026); /* … */
	lingoTableSet(t, 0x7F, 0x3000);
	lingoTableSet(t, 0xF0, 0x5186); /* 円 */
	lingoTableSet(t, 0xF4, 0x30A9); /* ォ */
	lingoTableSetRun(t, 0xF6, "０１２３４５６７８９");
}

bool lingoTablePreset(struct LingoTable* t, const char* system, const uint8_t* rom, size_t romSize) {
	if (!system || !rom) {
		return false;
	}
	if ((!strcmp(system, "gb") || !strcmp(system, "gbc")) && romSize > 0x150) {
		const char* title = (const char*) rom + 0x134;
		bool japan = rom[0x14A] == 0x00;
		if (japan && (!strncmp(title, "POKEMON", 7) || !strncmp(title, "PM_CRYSTAL", 10))) {
			presetPokemonGb(t);
			return true;
		}
	}
	return false;
}

/* ---- units and decoding ---- */

uint32_t lingoUnitCount(const struct LingoTable* t, const struct LingoRegion* r) {
	return (t->mode == LCM_LE16 || t->mode == LCM_BE16) ? r->size / 2 : r->size;
}

static inline uint32_t unitRaw(int mode, uint16_t mask, const struct LingoRegion* r, uint32_t i) {
	switch (mode) {
	case LCM_LE16:
		return (uint32_t) (r->data[i * 2] | (r->data[i * 2 + 1] << 8)) & mask;
	case LCM_BE16:
		return (uint32_t) ((r->data[i * 2] << 8) | r->data[i * 2 + 1]) & mask;
	default:
		return r->data[i];
	}
}

uint32_t lingoUnit(const struct LingoTable* t, const struct LingoRegion* r, uint32_t i) {
	return unitRaw(t->mode, t->mask, r, i);
}

static bool isNewline(const struct LingoTable* t, uint32_t u) {
	for (int i = 0; i < t->nNewline; ++i) {
		if (t->newline[i] == u) {
			return true;
		}
	}
	return false;
}

/* One character at unit i: codepoint (0 if none) and units used. */
static uint32_t charAt(const struct LingoTable* t, const struct LingoRegion* r, uint32_t i, uint32_t units, uint32_t* used) {
	*used = 1;
	if (t->mode == LCM_SJIS) {
		uint8_t b = r->data[i];
		if (b == 0x0A) {
			return '\n';
		}
		if (i + 1 < units) {
			uint32_t cp = lingoSjis(b, r->data[i + 1]);
			if (cp) {
				*used = 2;
				return cp == 0x3000 || cp == ' ' ? 0x3000 : cp;
			}
		}
		if (b >= 0xA1 && b <= 0xDF) {
			return halfKana(b);
		}
		if (b >= 0x30 && b <= 0x39) {
			return b; /* digits inside a line */
		}
		if (b == 0x20) {
			return 0x3000;
		}
		return 0;
	}
	uint32_t u = unitRaw(t->mode, t->mask, r, i);
	if (isNewline(t, u)) {
		return '\n';
	}
	uint32_t cp = t->map[u];
	if (!cp) {
		return 0;
	}
	if (t->stride && i >= t->stride && (t->dakuten != LT_NONE || t->handakuten != LT_NONE)) {
		uint32_t above = unitRaw(t->mode, t->mask, r, i - t->stride);
		if (above == t->dakuten && voiced(cp)) {
			cp = voiced(cp);
		} else if (above == t->handakuten && semiVoiced(cp)) {
			cp = semiVoiced(cp);
		}
	}
	return cp;
}

int lingoTableDecode(const struct LingoTable* t, const struct LingoRegion* r, uint32_t start, uint32_t maxUnits,
                     uint32_t* out, int outMax, uint32_t* used) {
	uint32_t units = lingoUnitCount(t, r);
	uint32_t i = start, end = start + maxUnits < units ? start + maxUnits : units;
	int n = 0;
	int spaces = 0;
	uint32_t spaceStart = i;
	while (i < end && n < outMax) {
		uint32_t step;
		uint32_t cp = charAt(t, r, i, units, &step);
		if (!cp) {
			break;
		}
		if (cp == 0x3000) {
			if (!spaces) {
				spaceStart = i;
			}
			if (++spaces >= 2 && t->stride == 0) {
				/* a gap of blanks: whatever follows is something else */
				while (n && out[n - 1] == 0x3000) {
					--n;
				}
				i = spaceStart;
				break;
			}
		} else {
			spaces = 0;
		}
		/* a separate ゛ or ゜ after the letter */
		if ((cp == 0x309B || cp == 0xFF9E) && n && voiced(out[n - 1])) {
			out[n - 1] = voiced(out[n - 1]);
		} else if ((cp == 0x309C || cp == 0xFF9F) && n && semiVoiced(out[n - 1])) {
			out[n - 1] = semiVoiced(out[n - 1]);
		} else {
			out[n++] = cp;
		}
		i += step;
	}
	*used = i - start;
	return n;
}

/* ---- finding lines ---- */

struct Run {
	int region;
	uint32_t start, end; /* units */
	uint32_t cp[LINGO_MAX_TEXT];
	int len;
	int kana;
	uint32_t changed; /* most recent page change */
	bool merged;
};

/* Does this decoded span read like Japanese? */
int lingoJudgeText(const uint32_t* cp, int n, bool strict) {
	int kana = 0, kanji = 0, jp = 0, other = 0, pairs = 0, score = 0;
	int prev = -1;
	for (int i = 0; i < n; ++i) {
		uint32_t c = cp[i];
		int bi = lingoBigramIndex(c);
		if (bi >= 0) {
			++kana;
			if (prev >= 0) {
				score += LINGO_BIGRAM[prev][bi];
				++pairs;
			}
		} else if (isKanji(c)) {
			++kanji;
		} else if (c == 0x3000 || c == '\n') {
			bi = -1;
		} else if (isJpPunct(c) || lingoIsKana(c)) {
			++jp;
		} else {
			++other;
		}
		prev = bi;
	}
	int real = kana + kanji;
	if (real < 3) {
		return 0;
	}
	if ((real + jp) * 10 < (real + jp + other) * 6) {
		return 0;
	}
	if (strict && kana < 2) {
		return 0; /* Japanese text almost always has some kana */
	}
	if (pairs >= 2) {
		/* in 1/8 bits per pair: real lines sit near zero or above, random
		 * kana well below (see tools/make_bigrams.py) */
		int avg = score / pairs;
		if (avg < (strict ? -12 : -20)) {
			return 0;
		}
	}
	return real;
}

static void trimRun(struct Run* r) {
	int a = 0, b = r->len;
	while (a < b && (r->cp[a] == 0x3000 || r->cp[a] == '\n')) {
		++a;
	}
	while (b > a && (r->cp[b - 1] == 0x3000 || r->cp[b - 1] == '\n')) {
		--b;
	}
	if (a) {
		memmove(r->cp, r->cp + a, (size_t) (b - a) * sizeof(uint32_t));
	}
	r->len = b - a;
}

static bool regionWanted(const struct LingoTable* t, const struct LingoRegion* r) {
	return !t->region[0] || !strcmp(t->region, r->name);
}

/* Collects lines in units [from, to) of a region. Returns the new run count. */
static int collectRuns(const struct LingoTable* t, const struct LingoRegions* rs, int ri, uint32_t from, uint32_t to,
                       struct Run* runs, int nRuns, int maxRuns) {
	const struct LingoRegion* r = &rs->r[ri];
	uint32_t units = lingoUnitCount(t, r);
	if (to > units) {
		to = units;
	}
	bool strict = t->mode == LCM_SJIS;
	uint32_t i = from;
	while (i < to && nRuns < maxRuns) {
		uint32_t step;
		uint32_t first = charAt(t, r, i, units, &step);
		if (!first || first == 0x3000 || first == '\n') {
			++i; /* runs start on a letter */
			continue;
		}
		/* walk back to where this run really starts */
		uint32_t s = i;
		if (t->mode != LCM_SJIS) {
			int blanks = 0;
			while (s > 0 && i - s < 256) {
				uint32_t st;
				uint32_t c = charAt(t, r, s - 1, units, &st);
				if (!c) {
					break;
				}
				blanks = c == 0x3000 ? blanks + 1 : 0;
				if (blanks >= 2 && t->stride == 0) {
					s += 1; /* stop after the gap */
					break;
				}
				--s;
			}
		}
		if (nRuns && runs[nRuns - 1].region == ri && s < runs[nRuns - 1].end) {
			s = runs[nRuns - 1].end;
		}
		struct Run* run = &runs[nRuns];
		uint32_t used;
		run->len = lingoTableDecode(t, r, s, 1024, run->cp, LINGO_MAX_TEXT, &used);
		run->region = ri;
		run->start = s;
		run->end = s + (used ? used : 1);
		run->merged = false;
		run->changed = 0;
		trimRun(run);
		run->kana = lingoJudgeText(run->cp, run->len, strict);
		if (run->kana) {
			++nRuns;
		}
		uint32_t next = s + (used ? used : 1);
		i = next > i ? next : i + 1;
	}
	return nRuns;
}

/* Rows of one text box sit a screen row apart: join them into one line. */
static void mergeRows(const struct LingoTable* t, struct Run* runs, int n) {
	if (!t->stride) {
		return;
	}
	uint32_t s = t->stride;
	for (int a = 0; a < n; ++a) {
		if (runs[a].merged) {
			continue;
		}
		struct Run* head = &runs[a];
		uint32_t rowStart = head->start;
		for (int rows = 0; rows < 6; ++rows) {
			int best = -1;
			for (int b = a + 1; b < n; ++b) {
				struct Run* r = &runs[b];
				if (r->merged || r->region != head->region || r->start <= rowStart) {
					continue;
				}
				uint32_t d = r->start - rowStart;
				uint32_t m = d % s;
				if (d >= s - 6 && d <= 3 * s + 6 && (m <= 6 || m >= s - 6) && r->end - r->start <= s) {
					best = b;
					break;
				}
			}
			if (best < 0) {
				break;
			}
			struct Run* r = &runs[best];
			if (head->len + 1 + r->len > LINGO_MAX_TEXT) {
				break;
			}
			head->cp[head->len++] = '\n';
			memcpy(head->cp + head->len, r->cp, (size_t) r->len * sizeof(uint32_t));
			head->len += r->len;
			head->kana += r->kana;
			if (r->changed > head->changed) {
				head->changed = r->changed;
			}
			if (r->end > head->end) {
				head->end = r->end;
			}
			r->merged = true;
			rowStart = r->start;
		}
	}
}

static uint32_t hashText(const uint32_t* cp, int n) {
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; ++i) {
		h = (h ^ cp[i]) * 16777619u;
	}
	return h;
}

static void runToText(const struct Run* r, struct LingoText* out) {
	out->len = r->len < LINGO_MAX_TEXT ? r->len : LINGO_MAX_TEXT;
	memcpy(out->cp, r->cp, (size_t) out->len * sizeof(uint32_t));
	out->start = ((uint32_t) r->region << 24) | (r->start & 0xFFFFFF);
	out->hash = hashText(out->cp, out->len);
}

static struct Run* sRuns; /* LR_MAX_RUNS, shared scratch (one reader runs at a time) */

static struct Run* runsBuf(void) {
	if (!sRuns) {
		sRuns = malloc(sizeof(struct Run) * LR_MAX_RUNS);
	}
	return sRuns;
}

int lingoReaderFindAll(const struct LingoTable* t, const struct LingoRegions* rs, struct LingoText* out, int max) {
	struct Run* runs = runsBuf();
	if (!runs) {
		return 0;
	}
	int n = 0;
	for (int ri = 0; ri < rs->n; ++ri) {
		if (!regionWanted(t, &rs->r[ri])) {
			continue;
		}
		n = collectRuns(t, rs, ri, 0, lingoUnitCount(t, &rs->r[ri]), runs, n, LR_MAX_RUNS);
	}
	mergeRows(t, runs, n);
	int k = 0;
	for (int i = 0; i < n && k < max; ++i) {
		if (!runs[i].merged) {
			runToText(&runs[i], &out[k++]);
		}
	}
	return k;
}

/* ---- the live reader ---- */

#define RECENT_SCANS 40 /* about 2.5 s at a scan every 4 frames */

void lingoReaderInit(struct LingoReader* rd, struct LingoTable* table) {
	memset(rd, 0, sizeof(*rd));
	rd->table = table;
}

void lingoReaderFree(struct LingoReader* rd) {
	for (int i = 0; i < LR_MAX_REGIONS; ++i) {
		free(rd->prev[i]);
		free(rd->pageChanged[i]);
	}
	struct LingoTable* t = rd->table;
	memset(rd, 0, sizeof(*rd));
	rd->table = t;
}

bool lingoReaderStep(struct LingoReader* rd, const struct LingoRegions* rs, struct LingoText* out) {
	struct LingoTable* t = rd->table;
	if (!t || (!t->nMapped && t->mode != LCM_SJIS)) {
		return false;
	}
	if (++rd->frame % 4) {
		return false;
	}
	++rd->scan;
	struct Run* runs = runsBuf();
	if (!runs) {
		return false;
	}
	int n = 0;
	unsigned unitBytes = (t->mode == LCM_LE16 || t->mode == LCM_BE16) ? 2 : 1;
	for (int ri = 0; ri < rs->n && ri < LR_MAX_REGIONS; ++ri) {
		const struct LingoRegion* r = &rs->r[ri];
		if (!regionWanted(t, r)) {
			continue;
		}
		uint32_t pages = (r->size + LR_PAGE - 1) / LR_PAGE;
		if (rd->data[ri] != r->data || rd->size[ri] != r->size) {
			free(rd->prev[ri]);
			free(rd->pageChanged[ri]);
			rd->prev[ri] = malloc(r->size);
			rd->pageChanged[ri] = calloc(pages, sizeof(uint32_t));
			rd->data[ri] = r->data;
			rd->size[ri] = r->size;
			if (rd->prev[ri]) {
				memcpy(rd->prev[ri], r->data, r->size);
			}
			continue; /* primed: changes count from the next scan */
		}
		if (!rd->prev[ri] || !rd->pageChanged[ri]) {
			continue;
		}
		for (uint32_t p = 0; p < pages; ++p) {
			uint32_t off = p * LR_PAGE;
			uint32_t len = r->size - off < LR_PAGE ? r->size - off : LR_PAGE;
			if (memcmp(rd->prev[ri] + off, r->data + off, len)) {
				memcpy(rd->prev[ri] + off, r->data + off, len);
				rd->pageChanged[ri][p] = rd->scan;
			}
		}
		/* decode around the pages that changed lately */
		uint32_t p = 0;
		while (p < pages) {
			uint32_t c = rd->pageChanged[ri][p];
			if (!c || rd->scan - c > RECENT_SCANS) {
				++p;
				continue;
			}
			uint32_t q = p;
			while (q + 1 < pages && rd->pageChanged[ri][q + 1] && rd->scan - rd->pageChanged[ri][q + 1] <= RECENT_SCANS) {
				++q;
			}
			/* one page either side: the rest of a text box */
			uint32_t fromB = (p ? p - 1 : 0) * LR_PAGE, toB = (q + 2) * LR_PAGE;
			int before = n;
			n = collectRuns(t, rs, ri, fromB / unitBytes, toB / unitBytes, runs, n, LR_MAX_RUNS);
			for (int k = before; k < n; ++k) {
				uint32_t a = runs[k].start * unitBytes / LR_PAGE, b = (runs[k].end * unitBytes - 1) / LR_PAGE;
				uint32_t best = 0;
				for (uint32_t pg = a; pg <= b && pg < pages; ++pg) {
					if (rd->pageChanged[ri][pg] > best) {
						best = rd->pageChanged[ri][pg];
					}
				}
				runs[k].changed = best;
			}
			p = q + 1;
		}
	}
	mergeRows(t, runs, n);
	int pick = -1;
	for (int i = 0; i < n; ++i) {
		if (runs[i].merged || !runs[i].changed) {
			continue;
		}
		if (pick < 0 || runs[i].changed > runs[pick].changed ||
		    (runs[i].changed == runs[pick].changed && runs[i].kana > runs[pick].kana)) {
			pick = i;
		}
	}
	if (pick < 0) {
		rd->candStable = 0;
		return false;
	}
	struct Run* r = &runs[pick];
	uint32_t h = hashText(r->cp, r->len);
	uint32_t start = ((uint32_t) r->region << 24) | r->start;
	if (h == rd->candHash && start == rd->candStart) {
		++rd->candStable;
	} else {
		rd->candHash = h;
		rd->candStart = start;
		rd->candStable = 0;
	}
	if (rd->candStable >= 2 && h != rd->lastHash) {
		rd->lastHash = h;
		rd->lastKana = r->kana;
		runToText(r, out);
		return true;
	}
	return false;
}

/* ---- pair statistics (for detection and for finishing taught tables) ---- */

#define PAIR_BITS 16
#define PAIR_CAP (1u << PAIR_BITS)

struct Pairs {
	uint32_t key[PAIR_CAP]; /* (a << 16 | b) + 1, 0 = empty */
	uint32_t count[PAIR_CAP];
	uint32_t used;
	uint32_t total;
};

static void pairAdd(struct Pairs* p, uint32_t a, uint32_t b, uint32_t c) {
	if (a == b) {
		return; /* runs of one value are filler, not text */
	}
	uint32_t k = ((a << 16) | b) + 1;
	uint32_t h = (k * 2654435761u) >> (32 - PAIR_BITS);
	for (uint32_t probe = 0; probe < PAIR_CAP; ++probe) {
		uint32_t s = (h + probe) & (PAIR_CAP - 1);
		if (p->key[s] == k) {
			p->count[s] += c;
			p->total += c;
			return;
		}
		if (!p->key[s]) {
			if (p->used > PAIR_CAP * 3 / 4) {
				return; /* full: rare pairs are dropped */
			}
			p->key[s] = k;
			p->count[s] = c;
			p->used++;
			p->total += c;
			return;
		}
	}
}

struct ModeSpec {
	uint8_t mode;
	uint16_t mask;
};
static const struct ModeSpec MODES[] = {
	{ LCM_BYTE, 0xFF },  { LCM_LE16, 0x3FF }, { LCM_LE16, 0x7FF },
	{ LCM_LE16, 0xFFF }, { LCM_BE16, 0x7FF }, { LCM_BE16, 0x3FF },
};
#define NMODES ((int) (sizeof(MODES) / sizeof(MODES[0])))

/* Pairs of neighbouring units that both changed since the last sample: new
 * text, not static data counted again whenever its page changes, nor a lone
 * counter ticking over. */
static void countChanged(struct Pairs* p, int mode, uint16_t mask, const struct LingoRegion* r, const uint8_t* before,
                         uint32_t fromByte, uint32_t toByte) {
	unsigned ub = mode == LCM_BYTE ? 1 : 2;
	uint32_t units = r->size / ub;
	uint32_t a = fromByte / ub, b = (toByte + ub - 1) / ub;
	if (b > units) {
		b = units;
	}
	bool prevChanged = false;
	for (uint32_t u = a; u < b; ++u) {
		bool changed = memcmp(before + u * ub, r->data + u * ub, ub) != 0;
		/* both letters new: text being written, not a counter ticking */
		if (changed && prevChanged) {
			pairAdd(p, unitRaw(mode, mask, r, u - 1), unitRaw(mode, mask, r, u), 1);
		}
		prevChanged = changed;
	}
}

static void countPairs(struct Pairs* p, int mode, uint16_t mask, const struct LingoRegion* r, uint32_t fromByte, uint32_t toByte) {
	unsigned ub = mode == LCM_BYTE ? 1 : 2;
	uint32_t units = r->size / ub;
	uint32_t a = fromByte / ub, b = toByte / ub + 1;
	if (b > units) {
		b = units;
	}
	for (uint32_t i = a; i + 1 < b; ++i) {
		pairAdd(p, unitRaw(mode, mask, r, i), unitRaw(mode, mask, r, i + 1), 1);
	}
}

/* An ordering of kana at consecutive codes. */
struct Order {
	uint32_t cp[100];
	int n;
};

static void orderFrom(struct Order* o, const char* s) {
	o->n = toCodepoints(s, o->cp, 100);
}

static void orderUnicode(struct Order* o, uint32_t first, uint32_t last) {
	o->n = 0;
	for (uint32_t c = first; c <= last && o->n < 100; ++c) {
		o->cp[o->n++] = c;
	}
}

struct Fit {
	int base;
	long score; /* 1/8 bits */
	long pairs;
	long second;
};

/* Scores putting ordering o at every base code. */
static struct Fit fitOrder(const struct Pairs* p, const struct Order* o, uint32_t maxCode) {
	static long S[65536 + 128], N[65536 + 128];
	struct Fit f = { -1, 0, 0, 0 };
	uint32_t L = (uint32_t) o->n;
	if (maxCode + 1 < L) {
		return f;
	}
	uint32_t bases = maxCode + 2 - L;
	memset(S, 0, sizeof(long) * bases);
	memset(N, 0, sizeof(long) * bases);
	int idx[100];
	for (int i = 0; i < o->n; ++i) {
		idx[i] = lingoBigramIndex(o->cp[i]);
	}
	for (uint32_t s = 0; s < PAIR_CAP; ++s) {
		if (!p->key[s]) {
			continue;
		}
		uint32_t k = p->key[s] - 1;
		uint32_t x = k >> 16, y = k & 0xFFFF;
		uint32_t c = p->count[s];
		uint32_t hi = x < y ? x : y;
		uint32_t mx = x > y ? x : y;
		uint32_t lo = mx >= L - 1 ? mx - (L - 1) : 0;
		for (uint32_t b = lo; b <= hi && b < bases; ++b) {
			int ix = idx[x - b], iy = idx[y - b];
			if (ix >= 0 && iy >= 0) {
				S[b] += (long) c * LINGO_BIGRAM[ix][iy];
				N[b] += c;
			}
		}
	}
	for (uint32_t b = 0; b < bases; ++b) {
		if (f.base < 0 || S[b] > f.score) {
			f.base = (int) b;
			f.score = S[b];
			f.pairs = N[b];
		}
	}
	for (uint32_t b = 0; b < bases; ++b) {
		if ((int) b != f.base && S[b] > f.second) {
			f.second = S[b];
		}
	}
	return f;
}

static bool fitGood(const struct Fit* f, long minPairs, long minScore) {
	return f->base >= 0 && f->pairs >= minPairs && f->score >= minScore && f->score >= f->pairs * 3 &&
	       f->second * 10 <= f->score * 6;
}

/* Score of every pair in the statistics that the table can read. */
static long tableScore(const struct Pairs* p, const struct LingoTable* t, uint32_t lo, uint32_t hi, long* pairsOut) {
	long score = 0, pairs = 0;
	for (uint32_t s = 0; s < PAIR_CAP; ++s) {
		if (!p->key[s]) {
			continue;
		}
		uint32_t k = p->key[s] - 1;
		uint32_t x = k >> 16, y = k & 0xFFFF;
		if (!((x >= lo && x < hi) || (y >= lo && y < hi))) {
			continue;
		}
		int ix = lingoBigramIndex(t->map[x]), iy = lingoBigramIndex(t->map[y]);
		if (ix >= 0 && iy >= 0) {
			score += (long) p->count[s] * LINGO_BIGRAM[ix][iy];
			pairs += p->count[s];
		}
	}
	*pairsOut = pairs;
	return score;
}

static void toKata(const char* hira, char* out, size_t n) {
	uint32_t cp[100];
	int k = toCodepoints(hira, cp, 100);
	size_t o = 0;
	for (int i = 0; i < k && o + 4 < n; ++i) {
		uint32_t c = cp[i] >= 0x3041 && cp[i] <= 0x3096 ? cp[i] + 0x60 : cp[i];
		o += (size_t) lingoUtf8Encode(c, out + o);
	}
	out[o] = 0;
}

/* After a basic あ..ん run at base, try the usual follow-on layouts and keep
 * the one the statistics like best. Returns the chosen layout or -1. */
static int extendBasic(struct LingoTable* t, const struct Pairs* p, uint32_t base, bool kata, const uint32_t* known,
                       const uint32_t* knownCode, int nKnown) {
	uint32_t at = base + BASIC_N;
	int bestE = -1;
	long bestScore = 0;
	for (int e = 0; e < EXT_N; ++e) {
		char s[400];
		if (kata) {
			toKata(EXT_H[e], s, sizeof(s));
		} else {
			snprintf(s, sizeof(s), "%s", EXT_H[e]);
		}
		uint32_t cp[100];
		int n = toCodepoints(s, cp, 100);
		if (at + (uint32_t) n - 1 > t->mask) {
			continue;
		}
		/* must agree with letters the player taught */
		bool agree = true;
		int agreeing = 0;
		for (int k = 0; k < nKnown; ++k) {
			if (knownCode[k] >= at && knownCode[k] < at + (uint32_t) n) {
				if (cp[knownCode[k] - at] != known[k]) {
					agree = false;
				} else {
					++agreeing;
				}
			} else if (knownCode[k] < base || knownCode[k] >= at) {
				/* a taught letter outside the layout: this layout cannot place it */
				for (int j = 0; j < n; ++j) {
					if (cp[j] == known[k]) {
						agree = false;
					}
				}
			}
		}
		if (!agree) {
			continue;
		}
		uint16_t saved[100];
		for (int i = 0; i < n; ++i) {
			saved[i] = t->map[at + i];
			if (!saved[i]) {
				t->map[at + i] = (uint16_t) cp[i];
			}
		}
		long pairs = 0;
		long score = p ? tableScore(p, t, at, at + (uint32_t) n, &pairs) : 0;
		score += agreeing * 10000L; /* taught letters outweigh statistics */
		for (int i = 0; i < n; ++i) {
			t->map[at + i] = saved[i];
		}
#ifdef LINGO_DEBUG
		fprintf(stderr, "    ext %d: score %ld pairs %ld agree %d\n", e, score, pairs, agreeing);
#endif
		if (score > bestScore && (pairs >= 3 || agreeing)) {
			bestScore = score;
			bestE = e;
		}
	}
	if (bestE >= 0) {
		char s[400];
		if (kata) {
			toKata(EXT_H[bestE], s, sizeof(s));
		} else {
			snprintf(s, sizeof(s), "%s", EXT_H[bestE]);
		}
		uint32_t cp[100];
		int n = toCodepoints(s, cp, 100);
		for (int i = 0; i < n; ++i) {
			if (!t->map[at + i]) {
				lingoTableSet(t, (uint16_t) (at + i), cp[i]);
			}
		}
	}
	return bestE;
}

/* The code most often sitting between two letters: the space. */
static void findSpace(struct LingoTable* t, const struct Pairs* p) {
	static uint32_t left[65536], right[65536];
	uint32_t maxCode = t->mask;
	memset(left, 0, sizeof(uint32_t) * (maxCode + 1));
	memset(right, 0, sizeof(uint32_t) * (maxCode + 1));
	for (uint32_t s = 0; s < PAIR_CAP; ++s) {
		if (!p->key[s]) {
			continue;
		}
		uint32_t k = p->key[s] - 1;
		uint32_t x = k >> 16, y = k & 0xFFFF;
		if (lingoBigramIndex(t->map[x]) >= 0 && !t->map[y]) {
			right[y] += p->count[s]; /* letter then y */
		}
		if (!t->map[x] && lingoBigramIndex(t->map[y]) >= 0) {
			left[x] += p->count[s]; /* x then letter */
		}
	}
	uint32_t best = 0, bestN = 0;
	for (uint32_t c = 0; c <= maxCode; ++c) {
		uint32_t m = left[c] < right[c] ? left[c] : right[c];
		if (m > bestN) {
			bestN = m;
			best = c;
		}
	}
	if (bestN >= 5) {
		lingoTableSet(t, (uint16_t) best, 0x3000);
	}
}

/* ゛ and ゜ drawn a row above the letter (tile maps): look for codes that sit
 * above letters that can take them, and almost nowhere else. */
static void findMarks(struct LingoTable* t, const struct LingoRegions* rs) {
	static const uint16_t STRIDES[] = { 20, 32, 64, 30, 40 };
	static uint32_t above[65536], total[65536], aboveHa[65536];
	uint32_t maxCode = t->mask;
	uint16_t bestStride = 0, bestD = LT_NONE, bestH = LT_NONE;
	uint32_t bestA = 0;
	for (size_t si = 0; si < sizeof(STRIDES) / sizeof(STRIDES[0]); ++si) {
		uint32_t s = STRIDES[si];
		memset(above, 0, sizeof(uint32_t) * (maxCode + 1));
		memset(total, 0, sizeof(uint32_t) * (maxCode + 1));
		memset(aboveHa, 0, sizeof(uint32_t) * (maxCode + 1));
		for (int ri = 0; ri < rs->n; ++ri) {
			const struct LingoRegion* r = &rs->r[ri];
			if (!regionWanted(t, r)) {
				continue;
			}
			uint32_t units = lingoUnitCount(t, r);
			if (units > 0x40000) {
				units = 0x40000;
			}
			for (uint32_t i = 0; i < units; ++i) {
				uint32_t u = unitRaw(t->mode, t->mask, r, i);
				total[u]++;
				if (i + s < units) {
					uint32_t cp = t->map[unitRaw(t->mode, t->mask, r, i + s)];
					if (cp && voiced(cp) && !t->map[u]) {
						above[u]++;
						if (semiVoiced(cp)) {
							aboveHa[u]++;
						}
					}
				}
			}
		}
		uint32_t d = 0, dn = 0;
		for (uint32_t c = 0; c <= maxCode; ++c) {
			if (above[c] >= 2 && above[c] * 10 >= total[c] * 8 && above[c] > dn) {
				dn = above[c];
				d = c;
			}
		}
		if (dn > bestA) {
			bestA = dn;
			bestStride = (uint16_t) s;
			bestD = (uint16_t) d;
			bestH = LT_NONE;
			uint32_t hn = 0;
			for (uint32_t c = 0; c <= maxCode; ++c) {
				if (c != d && aboveHa[c] >= 1 && aboveHa[c] == above[c] && above[c] * 10 >= total[c] * 6 && above[c] > hn) {
					hn = above[c];
					bestH = (uint16_t) c;
				}
			}
		}
	}
	if (bestA >= 2) {
		t->stride = bestStride;
		t->dakuten = bestD;
		t->handakuten = bestH;
	}
}

/* ---- automatic detection ---- */

struct LingoDetect {
	struct Pairs pairs[NMODES];
	uint8_t* prev[LR_MAX_REGIONS];
	const uint8_t* data[LR_MAX_REGIONS];
	uint32_t size[LR_MAX_REGIONS];
	long sjisChars;
	int samples;
	struct LingoTable sjis;
};

struct LingoDetect* lingoDetectNew(void) {
	struct LingoDetect* d = calloc(1, sizeof(*d));
	if (d) {
		lingoTableInit(&d->sjis, LCM_SJIS, 0);
	}
	return d;
}

void lingoDetectFree(struct LingoDetect* d) {
	if (!d) {
		return;
	}
	for (int i = 0; i < LR_MAX_REGIONS; ++i) {
		free(d->prev[i]);
	}
	free(d);
}

void lingoDetectSample(struct LingoDetect* d, const struct LingoRegions* rs) {
	if (!d) {
		return;
	}
	++d->samples;
	struct Run* runs = runsBuf();
	for (int ri = 0; ri < rs->n && ri < LR_MAX_REGIONS; ++ri) {
		const struct LingoRegion* r = &rs->r[ri];
		if (d->data[ri] != r->data || d->size[ri] != r->size) {
			free(d->prev[ri]);
			d->prev[ri] = malloc(r->size);
			d->data[ri] = r->data;
			d->size[ri] = r->size;
			if (d->prev[ri]) {
				memcpy(d->prev[ri], r->data, r->size);
			}
			continue;
		}
		if (!d->prev[ri]) {
			continue;
		}
		uint32_t pages = (r->size + LR_PAGE - 1) / LR_PAGE;
		for (uint32_t p = 0; p < pages; ++p) {
			uint32_t off = p * LR_PAGE;
			uint32_t len = r->size - off < LR_PAGE ? r->size - off : LR_PAGE;
			if (!memcmp(d->prev[ri] + off, r->data + off, len)) {
				continue;
			}
			for (int m = 0; m < NMODES; ++m) {
				countChanged(&d->pairs[m], MODES[m].mode, MODES[m].mask, r, d->prev[ri], off, off + len);
			}
			memcpy(d->prev[ri] + off, r->data + off, len);
			if (runs) {
				int n = collectRuns(&d->sjis, rs, ri, off, off + len, runs, 0, LR_MAX_RUNS);
				for (int k = 0; k < n; ++k) {
					d->sjisChars += runs[k].kana;
				}
			}
		}
	}
}

float lingoDetectConfidence(const struct LingoDetect* d) {
	if (!d) {
		return 0;
	}
	float f = d->pairs[0].total / 6000.0f;
	float s = d->sjisChars / 40.0f;
	if (s > f) {
		f = s;
	}
	return f > 1 ? 1 : f;
}

struct Choice {
	int m;
	bool kata;
	bool uni;
	struct Fit fit;
};

/* Evidence for a fit: score over the square root of the pairs it covers,
 * so a mode that sees more pairs (bytes vs 16-bit units) doesn't win by
 * volume alone. */
static double zOf(const struct Fit* f) {
	if (f->base < 0 || f->pairs <= 0 || f->score <= 0) {
		return 0;
	}
	return f->score / sqrt((double) f->pairs);
}

static void detectInMode(const struct Pairs* p, int m, bool kata, struct Choice* best) {
	struct Order basic, uni;
	orderFrom(&basic, kata ? BASIC_K : BASIC_H);
	if (kata) {
		orderUnicode(&uni, 0x30A1, 0x30F3);
	} else {
		orderUnicode(&uni, 0x3041, 0x3093);
	}
	uint32_t maxCode = MODES[m].mask;
	struct Fit fb = fitOrder(p, &basic, maxCode);
	struct Fit fu = fitOrder(p, &uni, maxCode);
	struct Fit* f = zOf(&fu) > zOf(&fb) ? &fu : &fb;
#ifdef LINGO_DEBUG
	fprintf(stderr, "  mode %d kata %d: basic base %d score %ld pairs %ld (2nd %ld) | uni base %d score %ld pairs %ld | total %u\n", m,
	        kata, fb.base, fb.score, fb.pairs, fb.second, fu.base, fu.score, fu.pairs, p->total);
#endif
	if (best->m < 0 || zOf(f) > zOf(&best->fit)) {
		best->m = m;
		best->kata = kata;
		best->uni = f == &fu;
		best->fit = *f;
	}
}

static void applyChoice(struct LingoTable* t, const struct Choice* c, const struct Pairs* p) {
	if (c->uni) {
		uint32_t first = c->kata ? 0x30A1 : 0x3041, last = c->kata ? 0x30F3 : 0x3093;
		for (uint32_t cp = first; cp <= last; ++cp) {
			lingoTableSet(t, (uint16_t) (c->fit.base + (cp - first)), cp);
		}
	} else {
		lingoTableSetRun(t, (uint16_t) c->fit.base, c->kata ? BASIC_K : BASIC_H);
		extendBasic(t, p, (uint32_t) c->fit.base, c->kata, NULL, NULL, 0);
	}
}

bool lingoDetectFinish(struct LingoDetect* d, const struct LingoRegions* rs, struct LingoTable* out, char* why, size_t whyLen) {
	if (!d) {
		snprintf(why, whyLen, "Out of memory.");
		return false;
	}
	struct Choice hira = { -1, false, false, { -1, 0, 0, 0 } };
	for (int m = 0; m < NMODES; ++m) {
		if (d->pairs[m].total < 40) {
			continue;
		}
		detectInMode(&d->pairs[m], m, false, &hira);
	}
	struct Choice kata = { -1, true, false, { -1, 0, 0, 0 } };
	for (int m = 0; m < NMODES; ++m) {
		if (d->pairs[m].total < 40) {
			continue;
		}
		detectInMode(&d->pairs[m], m, true, &kata);
	}
	/* hiragana and katakana fold to the same statistics, so the best katakana
	 * fit is often the hiragana one again: keep whichever is better as the
	 * main script, then look for the other script in the same mode. */
	struct Choice* prim = zOf(&kata.fit) > zOf(&hira.fit) ? &kata : &hira;
#ifdef LINGO_DEBUG
	fprintf(stderr, "detect: hira m%d base %d score %ld pairs %ld second %ld | kata m%d base %d score %ld pairs %ld | sjis %ld\n",
	        hira.m, hira.fit.base, hira.fit.score, hira.fit.pairs, hira.fit.second, kata.m, kata.fit.base, kata.fit.score,
	        kata.fit.pairs, d->sjisChars);
#endif
	bool kanaGood = prim->m >= 0 && fitGood(&prim->fit, 30, 8 * 60);
	/* Shift-JIS when it found real lines and no font layout explains more */
	if (d->sjisChars >= 12 && (!kanaGood || d->sjisChars * 2 > prim->fit.pairs)) {
		lingoTableInit(out, LCM_SJIS, 0);
		snprintf(out->source, sizeof(out->source), "Shift-JIS");
		snprintf(why, whyLen, "This game keeps its text in Shift-JIS, the standard Japanese encoding. Kanji included.");
		return true;
	}
	if (!kanaGood) {
		snprintf(why, whyLen,
		         "Didn't find text that reads like Japanese yet. Leave a few lines of dialogue printing and try again, "
		         "or teach Lingo a line instead.");
		return false;
	}
	int m = prim->m;
	lingoTableInit(out, (enum LingoCodeMode) MODES[m].mode, MODES[m].mask);
	applyChoice(out, prim, &d->pairs[m]);
	/* the other script, in the same mode, away from the first */
	struct Choice other = { m, !prim->kata, false, { -1, 0, 0, 0 } };
	{
		struct Order basic, uni;
		orderFrom(&basic, other.kata ? BASIC_K : BASIC_H);
		if (other.kata) {
			orderUnicode(&uni, 0x30A1, 0x30F3);
		} else {
			orderUnicode(&uni, 0x3041, 0x3093);
		}
		/* blank out the codes already used so the same run is not found twice */
		struct Pairs* p = &d->pairs[m];
		struct Pairs* q = malloc(sizeof(*q));
		if (q) {
			memset(q, 0, sizeof(*q));
			for (uint32_t s = 0; s < PAIR_CAP; ++s) {
				if (!p->key[s]) {
					continue;
				}
				uint32_t k = p->key[s] - 1;
				uint32_t x = k >> 16, y = k & 0xFFFF;
				if (out->map[x] || out->map[y]) {
					continue;
				}
				pairAdd(q, x, y, p->count[s]);
			}
			struct Fit fb = fitOrder(q, &basic, MODES[m].mask);
			struct Fit fu = fitOrder(q, &uni, MODES[m].mask);
			other.uni = fu.score > fb.score;
			other.fit = other.uni ? fu : fb;
			if (fitGood(&other.fit, 12, 8 * 25)) {
				applyChoice(out, &other, p);
			} else {
				other.fit.base = -1;
			}
			free(q);
		}
	}
	findSpace(out, &d->pairs[m]);
	findMarks(out, rs);
	snprintf(out->source, sizeof(out->source), "found automatically");
	const char* where = MODES[m].mode == LCM_BYTE ? "one byte per letter" : "16-bit tile codes";
	snprintf(why, whyLen, "Found this game's %s%s (%s)%s.", prim->kata ? "katakana" : "hiragana",
	         other.fit.base >= 0 ? (prim->kata ? " and hiragana" : " and katakana") : "", where,
	         out->dakuten != LT_NONE ? ", with ゛ marks drawn above letters" : "");
	return true;
}

/* ---- teaching ---- */

enum { TK_SPACE, TK_LETTER };
struct Tok {
	int kind;
	uint32_t cp;      /* as typed */
	uint32_t base;    /* without ゛ ゜ */
	int mark;         /* 1 ゛, 2 ゜ */
};

struct Hit {
	int region, mode;
	uint16_t mask;
	uint32_t pos; /* units */
	int variant;  /* 0 basic + wildcards, 1 basic with marks above, 2 unicode order */
	bool spaces;
	int baseH, baseK;
	uint32_t wildCp[48], wildCode[48];
	int nWild;
	uint32_t spaceCode;
};

static int basicIndex(uint32_t cp, bool* kata) {
	static uint32_t h[BASIC_N], k[BASIC_N];
	static bool ready;
	if (!ready) {
		toCodepoints(BASIC_H, h, BASIC_N);
		toCodepoints(BASIC_K, k, BASIC_N);
		ready = true;
	}
	for (int i = 0; i < BASIC_N; ++i) {
		if (h[i] == cp) {
			*kata = false;
			return i;
		}
		if (k[i] == cp) {
			*kata = true;
			return i;
		}
	}
	return -1;
}

/* Does the typed line sit at unit pos? Fills hit on success. */
static bool matchAt(const struct Tok* tk, int n, int variant, int mode, uint16_t mask, const struct LingoRegion* r,
                    uint32_t pos, struct Hit* hit) {
	int baseH = -1, baseK = -1;
	int nWild = 0;
	uint32_t spaceCode = 0xFFFFFFFF;
	int anchors = 0;
	for (int i = 0; i < n; ++i) {
		uint32_t u = unitRaw(mode, mask, r, pos + (uint32_t) i);
		if (tk[i].kind == TK_SPACE) {
			if (spaceCode == 0xFFFFFFFF) {
				spaceCode = u;
			} else if (spaceCode != u) {
				return false;
			}
			continue;
		}
		uint32_t cp = variant == 1 ? tk[i].base : tk[i].cp;
		bool kata = false;
		int idx = -1;
		if (variant == 2) {
			uint32_t h = cp >= 0x30A1 && cp <= 0x30F3 ? cp - 0x60 : cp;
			if (h >= 0x3041 && h <= 0x3093) {
				idx = (int) (h - 0x3041);
				kata = cp >= 0x30A1;
			}
		} else {
			idx = basicIndex(cp, &kata);
		}
		if (idx >= 0) {
			int b = (int) u - idx;
			if (b < 0) {
				return false;
			}
			int* base = kata ? &baseK : &baseH;
			if (*base < 0) {
				*base = b;
			} else if (*base != b) {
				return false;
			}
			++anchors;
			continue;
		}
		/* anything else must at least be consistent */
		bool seen = false;
		for (int w = 0; w < nWild; ++w) {
			if (hit->wildCp[w] == cp) {
				if (hit->wildCode[w] != u) {
					return false;
				}
				seen = true;
			} else if (hit->wildCode[w] == u) {
				return false;
			}
		}
		if (!seen && nWild < 48) {
			hit->wildCp[nWild] = cp;
			hit->wildCode[nWild] = u;
			++nWild;
		}
	}
	if (anchors < 3) {
		return false;
	}
	/* wildcards must not land on codes the order gives to other letters */
	for (int w = 0; w < nWild; ++w) {
		int bH = baseH, bK = baseK;
		if (bH >= 0 && hit->wildCode[w] >= (uint32_t) bH && hit->wildCode[w] < (uint32_t) bH + (variant == 2 ? 83u : BASIC_N)) {
			return false;
		}
		if (bK >= 0 && hit->wildCode[w] >= (uint32_t) bK && hit->wildCode[w] < (uint32_t) bK + (variant == 2 ? 83u : BASIC_N)) {
			return false;
		}
	}
	if (spaceCode != 0xFFFFFFFF) {
		for (int w = 0; w < nWild; ++w) {
			if (hit->wildCode[w] == spaceCode) {
				return false;
			}
		}
	}
	hit->baseH = baseH;
	hit->baseK = baseK;
	hit->nWild = nWild;
	hit->spaceCode = spaceCode;
	hit->mode = mode;
	hit->mask = mask;
	hit->pos = pos;
	hit->variant = variant;
	return true;
}

/* Shift-JIS: the typed line, encoded, searched for directly. */
static bool teachSjis(const struct LingoRegions* rs, const struct Tok* tk, int n, struct LingoTable* out, char* why, size_t whyLen) {
	uint8_t pat[256];
	int pn = 0;
	for (int i = 0; i < n && pn + 2 < (int) sizeof(pat); ++i) {
		if (tk[i].kind == TK_SPACE) {
			continue;
		}
		uint32_t cp = tk[i].cp;
		bool found = false;
		for (int row = 0; row < LINGO_SJIS_ROWS && !found; ++row) {
			for (int c = 0; c < 189; ++c) {
				if (LINGO_SJIS[row * 189 + c] == cp) {
					pat[pn++] = (uint8_t) (row < 31 ? 0x81 + row : 0xE0 + row - 31);
					pat[pn++] = (uint8_t) (0x40 + c);
					found = true;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
	}
	if (pn < 6) {
		return false;
	}
	for (int ri = 0; ri < rs->n; ++ri) {
		const struct LingoRegion* r = &rs->r[ri];
		for (uint32_t i = 0; i + (uint32_t) pn <= r->size; ++i) {
			if (r->data[i] == pat[0] && !memcmp(r->data + i, pat, (size_t) pn)) {
				lingoTableInit(out, LCM_SJIS, 0);
				snprintf(out->source, sizeof(out->source), "Shift-JIS");
				snprintf(why, whyLen, "Found that line in %s, written in Shift-JIS. Lingo can read all of this game's text, kanji too.",
				         r->name);
				return true;
			}
		}
	}
	return false;
}

bool lingoTeach(const struct LingoRegions* rs, const uint32_t* typed, int n, struct LingoTable* out, char* why, size_t whyLen) {
	struct Tok tk[128];
	int nt = 0;
	for (int i = 0; i < n && nt < 128; ++i) {
		uint32_t c = typed[i];
		if (c == ' ' || c == 0x3000) {
			if (nt && tk[nt - 1].kind != TK_SPACE) {
				tk[nt].kind = TK_SPACE;
				tk[nt].cp = 0x3000;
				++nt;
			}
			continue;
		}
		if (c == '\n') {
			continue;
		}
		tk[nt].kind = TK_LETTER;
		tk[nt].cp = c;
		tk[nt].base = unvoice(c, &tk[nt].mark);
		++nt;
	}
	while (nt && tk[nt - 1].kind == TK_SPACE) {
		--nt;
	}
	int letters = 0;
	for (int i = 0; i < nt; ++i) {
		letters += tk[i].kind == TK_LETTER;
	}
	if (letters < 4) {
		snprintf(why, whyLen, "Type at least four letters of the line on screen (more is better).");
		return false;
	}
	if (teachSjis(rs, tk, nt, out, why, whyLen)) {
		return true;
	}
	/* the same line with the spaces taken out */
	struct Tok bare[128];
	int nb = 0;
	for (int i = 0; i < nt; ++i) {
		if (tk[i].kind == TK_LETTER) {
			bare[nb++] = tk[i];
		}
	}
	static struct Hit hits[16];
	int nHits = 0;
	for (int pass = 0; pass < 2 && !nHits; ++pass) {
		const struct Tok* pt = pass == 0 ? tk : bare;
		int pn = pass == 0 ? nt : nb;
		if (pass == 1 && nb == nt) {
			break;
		}
		for (int ri = 0; ri < rs->n; ++ri) {
			const struct LingoRegion* r = &rs->r[ri];
			for (int m = 0; m < NMODES; ++m) {
				uint32_t units = MODES[m].mode == LCM_BYTE ? r->size : r->size / 2;
				for (int variant = 0; variant < 3; ++variant) {
					for (uint32_t pos = 0; pos + (uint32_t) pn <= units && nHits < 16; ++pos) {
						struct Hit h;
						h.region = ri;
						h.spaces = pass == 0;
						if (matchAt(pt, pn, variant, MODES[m].mode, MODES[m].mask, r, pos, &h)) {
							hits[nHits++] = h;
						}
					}
				}
			}
		}
	}
	if (!nHits) {
		snprintf(why, whyLen,
		         "Couldn't find that line in the game's memory. Check it was typed exactly (small っ, ゛ marks), "
		         "type a longer line, or try while the text is still on screen.");
		return false;
	}
	/* most hits agree with each other; take the first of the most common kind */
	int best = 0, bestVotes = 0;
	for (int i = 0; i < nHits; ++i) {
		int votes = 0;
		for (int j = 0; j < nHits; ++j) {
			if (hits[j].mode == hits[i].mode && hits[j].mask == hits[i].mask && hits[j].baseH == hits[i].baseH &&
			    hits[j].baseK == hits[i].baseK && hits[j].variant == hits[i].variant) {
				++votes;
			}
		}
		if (votes > bestVotes) {
			bestVotes = votes;
			best = i;
		}
	}
	struct Hit* h = &hits[best];
	const struct LingoRegion* r = &rs->r[h->region];
	lingoTableInit(out, (enum LingoCodeMode) h->mode, h->mask);
	/* statistics from this region, to settle the letters the player did not type */
	struct Pairs* p = calloc(1, sizeof(*p));
	if (p) {
		countPairs(p, h->mode, h->mask, r, 0, r->size > 0x40000 ? 0x40000 : r->size);
	}
	for (int s = 0; s < 2; ++s) {
		int base = s == 0 ? h->baseH : h->baseK;
		if (base < 0) {
			continue;
		}
		bool kata = s == 1;
		if (h->variant == 2) {
			uint32_t first = kata ? 0x30A1 : 0x3041;
			for (uint32_t i = 0; i < 83; ++i) {
				lingoTableSet(out, (uint16_t) (base + (int) i), first + i);
			}
		} else {
			lingoTableSetRun(out, (uint16_t) base, kata ? BASIC_K : BASIC_H);
			/* taught letters of this script beyond the basic run */
			uint32_t kc[48], kcode[48];
			int nk = 0;
			for (int w = 0; w < h->nWild; ++w) {
				bool k = h->wildCp[w] >= 0x30A1 && h->wildCp[w] <= 0x30FA;
				if (k == kata && lingoIsKana(h->wildCp[w])) {
					kc[nk] = h->wildCp[w];
					kcode[nk] = h->wildCode[w];
					++nk;
				}
			}
			extendBasic(out, p, (uint32_t) base, kata, kc, kcode, nk);
		}
	}
	/* the other script, if the player only typed one: from the statistics */
	if ((h->baseH < 0 || h->baseK < 0) && p) {
		bool kata = h->baseK < 0;
		struct Order basic, uni;
		orderFrom(&basic, kata ? BASIC_K : BASIC_H);
		if (kata) {
			orderUnicode(&uni, 0x30A1, 0x30F3);
		} else {
			orderUnicode(&uni, 0x3041, 0x3093);
		}
		struct Pairs* q = calloc(1, sizeof(*q));
		if (q) {
			for (uint32_t s = 0; s < PAIR_CAP; ++s) {
				if (!p->key[s]) {
					continue;
				}
				uint32_t k = p->key[s] - 1;
				if (!out->map[k >> 16] && !out->map[k & 0xFFFF]) {
					pairAdd(q, k >> 16, k & 0xFFFF, p->count[s]);
				}
			}
			struct Fit fb = fitOrder(q, h->variant == 2 ? &uni : &basic, h->mask);
			if (fitGood(&fb, 12, 8 * 25)) {
				struct Choice c = { h->mode, kata, h->variant == 2, fb };
				applyChoice(out, &c, p);
			}
			free(q);
		}
	}
	for (int w = 0; w < h->nWild; ++w) {
		if (!out->map[h->wildCode[w]] && h->wildCp[w] <= 0xFFFF) {
			lingoTableSet(out, (uint16_t) h->wildCode[w], h->wildCp[w]);
		}
	}
	if (h->spaceCode != 0xFFFFFFFF && !out->map[h->spaceCode]) {
		lingoTableSet(out, (uint16_t) h->spaceCode, 0x3000);
	} else if (p) {
		findSpace(out, p);
	}
	/* marks above the letters: learn them from the typed ゛ letters */
	if (h->variant == 1) {
		static const uint16_t STRIDES[] = { 20, 32, 64, 30, 40 };
		const struct Tok* pt = h->spaces ? tk : bare;
		int pn = h->spaces ? nt : nb;
		for (size_t si = 0; si < sizeof(STRIDES) / sizeof(STRIDES[0]); ++si) {
			uint32_t s = STRIDES[si];
			if (h->pos < s) {
				continue;
			}
			uint32_t dCode = 0xFFFFFFFF, hCode = 0xFFFFFFFF;
			bool ok = true;
			int marks = 0;
			for (int i = 0; i < pn && ok; ++i) {
				if (pt[i].kind != TK_LETTER || !pt[i].mark) {
					continue;
				}
				uint32_t u = unitRaw(h->mode, h->mask, r, h->pos + (uint32_t) i - s);
				uint32_t* want = pt[i].mark == 1 ? &dCode : &hCode;
				if (*want == 0xFFFFFFFF) {
					*want = u;
				} else if (*want != u) {
					ok = false;
				}
				++marks;
			}
			if (ok && marks && dCode != hCode) {
				out->stride = (uint16_t) s;
				out->dakuten = dCode == 0xFFFFFFFF ? LT_NONE : (uint16_t) dCode;
				out->handakuten = hCode == 0xFFFFFFFF ? LT_NONE : (uint16_t) hCode;
				break;
			}
		}
		if (!out->stride) {
			findMarks(out, rs);
		}
	} else if (h->mode != LCM_BYTE) {
		findMarks(out, rs);
	}
	free(p);
	snprintf(out->region, sizeof(out->region), "%s", r->name);
	snprintf(out->source, sizeof(out->source), "taught");
	snprintf(why, whyLen, "Found that line in %s. Lingo now knows %d of this game's characters%s.", r->name, out->nMapped,
	         out->dakuten != LT_NONE ? ", and its ゛ marks" : "");
	return true;
}
