/* Kana input rules for the lookup keyboard. Works on hiragana and katakana. */
#include "psp2-lingo-kana.h"

#include <stdio.h>
#include <string.h>

#include <stdbool.h>

static bool isKata(uint32_t cp) {
	return cp >= 0x30A1 && cp <= 0x30F6;
}

static uint32_t toHira(uint32_t cp) {
	return isKata(cp) ? cp - 0x60 : cp;
}

static uint32_t back(uint32_t hira, bool kata) {
	return kata ? hira + 0x60 : hira;
}

uint32_t lingoKanaCycleMarks(uint32_t cp) {
	bool kata = isKata(cp);
	uint32_t h = toHira(cp);
	/* か..ぢ, つ..ど: plain and voiced alternate in Unicode (つ has small っ before it) */
	if ((h >= 0x304B && h <= 0x3062) || (h >= 0x3064 && h <= 0x3069)) {
		bool plain = (h <= 0x3062) ? ((h - 0x304B) % 2 == 0) : ((h - 0x3064) % 2 == 0);
		if (h == 0x3063) {
			return cp;
		}
		return back(plain ? h + 1 : h - 1, kata);
	}
	/* は..ぽ: plain, voiced, semi-voiced in threes */
	if (h >= 0x306F && h <= 0x307D) {
		int k = (h - 0x306F) % 3;
		return back(k == 2 ? h - 2 : h + 1, kata);
	}
	if (h == 0x3046) {
		return back(0x3094, kata); /* う → ゔ */
	}
	if (h == 0x3094) {
		return back(0x3046, kata);
	}
	return cp;
}

uint32_t lingoKanaToggleSmall(uint32_t cp) {
	bool kata = isKata(cp);
	uint32_t h = toHira(cp);
	/* あいうえお and やゆよ: small form sits just before the large one */
	static const uint32_t big[] = { 0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x3084, 0x3086, 0x3088, 0x3064, 0x308F };
	static const uint32_t small[] = { 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3083, 0x3085, 0x3087, 0x3063, 0x308E };
	for (unsigned i = 0; i < sizeof(big) / sizeof(big[0]); ++i) {
		if (h == big[i]) {
			return back(small[i], kata);
		}
		if (h == small[i]) {
			return back(big[i], kata);
		}
	}
	return cp;
}

uint32_t lingoKanaSwapScript(uint32_t cp) {
	if (cp >= 0x3041 && cp <= 0x3096) {
		return cp + 0x60;
	}
	if (isKata(cp)) {
		return cp - 0x60;
	}
	return cp;
}

/* ---- romaji ---- */

/* ぁ (0x3041) .. ゖ (0x3096), Hepburn */
static const char* const ROMA[] = {
	"a",  "a",  "i",  "i",  "u",  "u",  "e",  "e",  "o",  "o",  "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
	"sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo", "ta", "da", "chi", "ji", "tsu", "tsu", "zu", "te", "de", "to",
	"do", "na", "ni", "nu", "ne", "no", "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo",
	"po", "ma", "mi", "mu", "me", "mo", "ya", "ya", "yu", "yu", "yo", "yo", "ra", "ri", "ru", "re", "ro", "wa", "wa", "wi",
	"we", "o",  "n",  "vu", "ka", "ke",
};

static uint32_t romaHira(uint32_t c) {
	return (c >= 0x30A1 && c <= 0x30F6) ? c - 0x60 : c;
}

static bool isSmallY(uint32_t c) {
	return c == 0x3083 || c == 0x3085 || c == 0x3087; /* ゃゅょ */
}

static bool isSmallVowel(uint32_t c) {
	return c == 0x3041 || c == 0x3043 || c == 0x3045 || c == 0x3047 || c == 0x3049; /* ぁぃぅぇぉ */
}

int lingoRomaji(const uint32_t* in, int n, bool particle, char* out, size_t size) {
	size_t o = 0;
	char prevVowel = 0;
	bool doubleNext = false;
	for (int i = 0; i < n && o + 8 < size; ++i) {
		uint32_t c = romaHira(in[i]);
		if (c == 0x30FC) { /* ー: hold the vowel */
			if (prevVowel) {
				out[o++] = prevVowel;
			}
			continue;
		}
		if (c == 0x3063) { /* っ: double the next consonant */
			doubleNext = true;
			continue;
		}
		if (c < 0x3041 || c > 0x3096) {
			if (c == 0x3000 || c == ' ') {
				out[o++] = ' ';
			} else if (c < 0x80) {
				out[o++] = (char) c;
			} else if (c == 0xFF01 || c == 0xFF1F) {
				out[o++] = c == 0xFF01 ? '!' : '?';
			} else if (c == 0x3002 || c == 0x3001) {
				out[o++] = c == 0x3002 ? '.' : ',';
			} else if (c >= 0xFF10 && c <= 0xFF19) {
				out[o++] = (char) ('0' + c - 0xFF10);
			}
			prevVowel = 0;
			continue;
		}
		char syl[8];
		const char* r = ROMA[c - 0x3041];
		uint32_t next = i + 1 < n ? romaHira(in[i + 1]) : 0;
		if (particle && n == 1) {
			r = c == 0x306F ? "wa" : c == 0x3078 ? "e" : c == 0x3092 ? "o" : r;
		}
		if (c == 0x3092) {
			r = "o";
		}
		snprintf(syl, sizeof(syl), "%s", r);
		if (isSmallY(next) && strlen(syl) >= 2 && syl[strlen(syl) - 1] == 'i') {
			/* きゃ kya, しゃ sha, ちゃ cha, じゃ ja */
			const char* y = next == 0x3083 ? "a" : next == 0x3085 ? "u" : "o";
			size_t l = strlen(syl);
			if (!strcmp(syl, "shi") || !strcmp(syl, "chi") || !strcmp(syl, "ji")) {
				syl[l - 1] = 0;
				strcat(syl, y);
			} else {
				syl[l - 1] = 'y';
				strcat(syl, y);
			}
			++i;
		} else if (isSmallVowel(next) && (c == 0x3075 || c == 0x3066 || c == 0x3067 || c == 0x3046 || c == 0x3094)) {
			/* ふぁ fa, てぃ ti, でぃ di, うぃ wi, ゔぁ va */
			static const char V[] = "aiueo";
			const char* head = c == 0x3075 ? "f" : c == 0x3066 ? "t" : c == 0x3067 ? "d" : c == 0x3094 ? "v" : "w";
			snprintf(syl, sizeof(syl), "%s%c", head, V[(next - 0x3041) / 2]);
			++i;
		} else if (isSmallVowel(c) || isSmallY(c)) {
			/* a small vowel on its own */
		}
		if (c == 0x3093) { /* ん before a vowel or y: n' */
			uint32_t nx = next;
			bool vowelNext = nx == 0x3042 || nx == 0x3044 || nx == 0x3046 || nx == 0x3048 || nx == 0x304A || nx == 0x3084 ||
			                 nx == 0x3086 || nx == 0x3088;
			snprintf(syl, sizeof(syl), vowelNext ? "n'" : "n");
		}
		if (doubleNext) {
			doubleNext = false;
			if (syl[0] == 'c' && syl[1] == 'h') {
				out[o++] = 't';
			} else if (syl[0] && strchr("aiueon", syl[0]) == NULL) {
				out[o++] = syl[0];
			}
		}
		size_t l = strlen(syl);
		memcpy(out + o, syl, l);
		o += l;
		char last = syl[l ? l - 1 : 0];
		prevVowel = strchr("aiueo", last) ? last : 0;
	}
	out[o] = 0;
	return (int) o;
}
