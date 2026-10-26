/* Kana input rules for the lookup keyboard. Works on hiragana and katakana. */
#include "psp2-lingo-kana.h"

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
