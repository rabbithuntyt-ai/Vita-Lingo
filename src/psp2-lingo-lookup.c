/* Lingo lookup: type what you see with a gojūon keyboard and read the
 * dictionary entry. Works with any game on any console; looked-up words
 * join the game's checkpoint words. */
#include "psp2-lingo-kana.h"
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>

#define MAXIN 24

/* gojūon by column (あ か さ た な は ま や ら わ), five rows each; 0 = blank */
static const char* const COLS[10] = {
	"あいうえお", "かきくけこ", "さしすせそ", "たちつてと", "なにぬねの",
	"はひふへほ", "まみむめも", "や　ゆ　よ", "らりるれろ", "わをんー　",
};

static uint32_t sIn[MAXIN];
static int sN;
static bool sKata;
static struct LingoToken sTok[MAXIN];
static int sNTok;
static int sSel, sSelEntry;
static double sChangedAt;

struct Recent {
	char word[64], reading[64], meaning[128];
};
static struct Recent sRecent[8];
static int sRecentN;

int lingoRecentLookups(const char** words, const char** meanings, int max) {
	int n = 0;
	for (int i = sRecentN - 1; i >= 0 && n < max; --i) {
		words[n] = sRecent[i].word;
		meanings[n] = sRecent[i].meaning;
		++n;
	}
	return n;
}

static void remember(const struct LingoEntry* e) {
	for (int i = 0; i < sRecentN; ++i) {
		if (!strcmp(sRecent[i].word, e->headword)) {
			return;
		}
	}
	if (sRecentN == 8) {
		memmove(sRecent, sRecent + 1, sizeof(sRecent[0]) * 7);
		--sRecentN;
	}
	struct Recent* r = &sRecent[sRecentN++];
	snprintf(r->word, sizeof(r->word), "%s", e->headword);
	snprintf(r->reading, sizeof(r->reading), "%s", e->reading);
	snprintf(r->meaning, sizeof(r->meaning), "%s", e->gloss);
}

static void retokenize(void) {
	const struct LingoDict* d = lingoDictGet();
	sNTok = d && sN ? lingoTokenize(d, sIn, sN, sTok, MAXIN) : 0;
	sSel = -1;
	for (int i = 0; i < sNTok; ++i) {
		if (sTok[i].nIds) {
			sSel = i;
			break;
		}
	}
	sSelEntry = 0;
	sChangedAt = uiTime();
}

static bool key(float x, float y, float w, float h, const char* label, uint32_t col, bool jp, float px) {
	bool held = uiHeld(x, y, w, h);
	uiRound(x, y + (held ? 2 : 0), w, h, 10, held ? C_SHEET_HI : C_SHEET);
	uiRoundOutline(x, y + (held ? 2 : 0), w, h, 10, 1.5f, uiAlpha(col, 0.5f));
	if (jp) {
		uiJpCentered(x + w / 2, y + h / 2 + px * 0.45f + (held ? 2 : 0), px, col, label);
	} else {
		uiTextCentered(x + w / 2, y + h / 2 + px * 0.4f + (held ? 2 : 0), px, col, label);
	}
	return uiTapped(x, y, w, h);
}

/* The gojūon keyboard shared by Look up and Teach. Edits buf in place;
 * returns true when Done is tapped. With space, a Space key replaces half of Clear. */
bool lingoKeyboard(float ky, uint32_t* buf, int* n, int max, bool* kata, bool space) {
	float kx = 24, kw = 62, kh = 58, gap = 4;
	/* columns read right to left, like a Japanese chart */
	for (int c = 0; c < 10; ++c) {
		uint32_t cps[5];
		lingoUtf8Decode(COLS[c], cps, 5);
		for (int r = 0; r < 5; ++r) {
			if (cps[r] == 0x3000) {
				continue;
			}
			float x = kx + (9 - c) * (kw + gap), y = ky + r * (kh + gap);
			char lab[8] = { 0 };
			lingoUtf8Encode(*kata ? lingoKanaSwapScript(cps[r]) : cps[r], lab);
			if (key(x, y, kw, kh, lab, C_CHALK, true, 26) && *n < max) {
				buf[(*n)++] = *kata ? lingoKanaSwapScript(cps[r]) : cps[r];
			}
		}
	}
	float fx = kx + 10 * (kw + gap) + 10, fw = SCREEN_W - fx - 24;
	float hw2 = (fw - gap) / 2;
	if (key(fx, ky, hw2, kh, "゛゜", C_YELLOW, true, 24) && *n) {
		buf[*n - 1] = lingoKanaCycleMarks(buf[*n - 1]);
	}
	if (key(fx + hw2 + gap, ky, hw2, kh, "小", C_YELLOW, true, 24) && *n) {
		buf[*n - 1] = lingoKanaToggleSmall(buf[*n - 1]);
	}
	if (key(fx, ky + (kh + gap), hw2, kh, *kata ? "かな" : "カナ", C_BLUE, true, 22)) {
		*kata = !*kata;
	}
	if ((key(fx + hw2 + gap, ky + (kh + gap), hw2, kh, "←", C_RED, false, 22) || (IN.buttonsPressed & SCE_CTRL_SQUARE)) && *n) {
		--*n;
	}
	if (space) {
		if (key(fx, ky + 2 * (kh + gap), hw2, kh, "Space", C_SOFT, false, 15) && *n < max) {
			buf[(*n)++] = 0x3000;
		}
		if (key(fx + hw2 + gap, ky + 2 * (kh + gap), hw2, kh, "Clear", C_SOFT, false, 15)) {
			*n = 0;
		}
	} else if (key(fx, ky + 2 * (kh + gap), fw, kh, "Clear", C_SOFT, false, 17)) {
		*n = 0;
	}
	return key(fx, ky + 3 * (kh + gap), fw, kh * 2 + gap, "Done", C_GREEN, false, 19);
}

static void drawResult(float x, float y, float w, float h) {
	const struct LingoDict* d = lingoDictGet();
	uiRound(x, y, w, h, 16, C_SHEET);
	uiRoundOutline(x, y, w, h, 16, 2, C_LINE_SOFT);
	if (!d) {
		uiText(x + 20, y + 40, 16, C_RED, "Dictionary file missing from the app folder.");
		return;
	}
	if (!sN) {
		uiTextWrap(x + 20, y + 40, w - 40, 16, 23, C_SOFT,
		           "Type a word you see on screen. Verbs and adjectives can be typed exactly as they appear: "
		           "たべなかった finds たべる.", 4);
		return;
	}
	if (sSel < 0) {
		uiTextWrap(x + 20, y + 40, w - 40, 16, 23, C_SOFT, "Nothing yet. Keep typing, or check small kana and dakuten.", 3);
		return;
	}
	const struct LingoToken* t = &sTok[sSel];
	if (sSelEntry >= t->nIds) {
		sSelEntry = 0;
	}
	struct LingoEntry e;
	lingoDictEntry(d, t->ids[sSelEntry], &e);
	uiJp(x + 20, y + 52, 36, C_CHALK, e.headword);
	float hw = uiJpW(36, e.headword);
	if (strcmp(e.headword, e.reading)) {
		uiJp(x + 30 + hw, y + 50, 20, C_YELLOW, e.reading);
	}
	if (t->reason[sSelEntry][0]) {
		char buf[80];
		snprintf(buf, sizeof(buf), "← %s", t->reason[sSelEntry]);
		uiText(x + 20, y + 80, 14, C_GREEN, buf);
	}
	uiTextWrap(x + 20, y + 106, w - 150, 15, 21, C_CHALK, e.gloss, 3);
	/* other readings */
	float ox = x + 20;
	for (int k = 0; k < t->nIds && t->nIds > 1; ++k) {
		struct LingoEntry o;
		lingoDictEntry(d, t->ids[k], &o);
		float ow = uiJpW(15, o.headword) + 16;
		if (ox + ow > x + w - 140) {
			break;
		}
		uiRound(ox, y + h - 38, ow, 26, 9, k == sSelEntry ? uiAlpha(C_YELLOW, 0.25f) : C_SHEET_HI);
		uiJp(ox + 8, y + h - 19, 15, k == sSelEntry ? C_YELLOW : C_SOFT, o.headword);
		if (uiTapped(ox, y + h - 38, ow, 26)) {
			sSelEntry = k;
		}
		ox += ow + 6;
	}
	bool saved = deckHas(e.headword, e.reading);
	float bx = x + w - 118, by = y + h - 46;
	uiRound(bx, by, 104, 36, 12, saved ? uiAlpha(C_YELLOW, 0.3f) : C_SHEET_HI);
	uiRoundOutline(bx, by, 104, 36, 12, 2, C_YELLOW);
	uiText(bx + 14, by + 25, 16, C_YELLOW, saved ? "★ Saved" : "☆ Save");
	if (!saved && uiTapped(bx, by, 104, 36)) {
		struct SavedWord sw;
		memset(&sw, 0, sizeof(sw));
		snprintf(sw.head, sizeof(sw.head), "%s", e.headword);
		snprintf(sw.reading, sizeof(sw.reading), "%s", e.reading);
		snprintf(sw.gloss, sizeof(sw.gloss), "%s", e.gloss);
		snprintf(sw.context, sizeof(sw.context), "looked up while playing");
		deckAdd(&sw);
		uiDust(bx + 52, by + 18, C_YELLOW, 16);
	}
}

void lingoLookupRun(void) {
	uiInit();
	if (!sN) {
		retokenize();
	}
	while (true) {
		uiPollInput();
		static double last;
		double now = uiTime();
		float dt = last ? (float) (now - last) : 0;
		last = now;
		uiDustUpdate(dt > 0.1f ? 0.1f : dt);
		uiFrameBegin();
		uiChalkBackground();
		/* input line */
		uiText(24, 30, 14, C_SOFT, "LOOK UP  ·  type what you see");
		uiRound(24, 42, 452, 66, 14, C_SHEET);
		char typed[MAXIN * 4 + 1];
		lingoUtf8FromCodepoints(sIn, sN, typed, sizeof(typed));
		float tw = uiJpW(32, typed);
		uiJp(40, 88, 32, C_CHALK, typed);
		if (fmod(now, 1.0) < 0.6) {
			uiRect(42 + tw, 58, 3, 36, C_YELLOW);
		}
		/* tokens under the line: tap to pick which word */
		float cx = 24;
		for (int i = 0; i < sNTok; ++i) {
			char s[64];
			lingoUtf8FromCodepoints(sIn + sTok[i].start, sTok[i].len, s, sizeof(s));
			float w = uiJpW(17, s) + 16;
			bool on = i == sSel;
			uiRound(cx, 116, w, 28, 9, on ? uiAlpha(C_YELLOW, 0.25f) : (sTok[i].nIds ? C_SHEET : C_BOARD));
			uiJp(cx + 8, 136, 17, on ? C_YELLOW : (sTok[i].nIds ? C_CHALK : C_SOFT), s);
			if (sTok[i].nIds && uiTapped(cx, 116, w, 28)) {
				sSel = i;
				sSelEntry = 0;
			}
			cx += w + 6;
		}
		drawResult(492, 24, 444, 196);
		int before = sN;
		uint32_t lastCh = sN ? sIn[sN - 1] : 0;
		bool done = lingoKeyboard(230, sIn, &sN, MAXIN, &sKata, false);
		if (sN != before || (sN && sIn[sN - 1] != lastCh)) {
			retokenize();
		}
		uiDustDraw();
		uiFrameEnd();
		/* a word that has been on screen a moment counts as met in this game */
		if (sSel >= 0 && now - sChangedAt > 1.2 && sChangedAt > 0) {
			lingoNoteLookup(sIn, sN, &sTok[sSel]);
			struct LingoEntry e;
			if (lingoDictGet() && lingoDictEntry(lingoDictGet(), sTok[sSel].ids[0], &e)) {
				remember(&e);
			}
			sChangedAt = 0;
		}
		if (done || uiBack()) {
			storeSave();
			return;
		}
	}
}
