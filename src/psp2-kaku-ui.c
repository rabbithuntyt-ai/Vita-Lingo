/* Kaku for Vita: the practice screens. Writing on the touchscreen with
 * stroke-by-stroke checking, reading quizzes, and saved-word review. */
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>

enum Set { SET_HIRAGANA, SET_KATAKANA, SET_KANJI };

static float sLeniency = KAKU_LENIENCY_NORMAL;
static int sAlwaysTrace = 0;

static bool isHiragana(const char* s) {
	uint32_t cp;
	lingoUtf8Decode(s, &cp, 1);
	return cp < 0x30A0;
}

static int setSize(enum Set s) {
	if (s == SET_KANJI) {
		return KAKU_KANJI_COUNT;
	}
	int n = 0;
	for (int i = 0; i < KAKU_KANA_COUNT; ++i) {
		if (isHiragana(KAKU_KANA[i].ch) == (s == SET_HIRAGANA)) {
			++n;
		}
	}
	return n;
}

/* index of the i-th item of a set in its table */
static int setItem(enum Set s, int i) {
	if (s == SET_KANJI) {
		return i;
	}
	for (int k = 0; k < KAKU_KANA_COUNT; ++k) {
		if (isHiragana(KAKU_KANA[k].ch) == (s == SET_HIRAGANA) && i-- == 0) {
			return k;
		}
	}
	return 0;
}

static const char* itemChar(enum Set s, int idx) {
	return s == SET_KANJI ? KAKU_KANJI[idx].ch : KAKU_KANA[idx].ch;
}

static void itemKey(enum Set s, int idx, char* key, size_t n) {
	snprintf(key, n, "%s%s", s == SET_KANJI ? "j:" : "k:", itemChar(s, idx));
}

static int learnedCount(enum Set s) {
	int n = 0;
	for (int i = 0; i < setSize(s); ++i) {
		char key[32];
		itemKey(s, setItem(s, i), key, sizeof(key));
		struct KakuCard* c = storeGet(key, false);
		if (c && c->reps > 0) {
			++n;
		}
	}
	return n;
}

static int dueCount(enum Set s, uint32_t today) {
	int n = 0;
	for (int i = 0; i < setSize(s); ++i) {
		char key[32];
		itemKey(s, setItem(s, i), key, sizeof(key));
		struct KakuCard* c = storeGet(key, false);
		if (c && c->due <= today) {
			++n;
		}
	}
	return n;
}

static unsigned sRand = 2024;
static unsigned rnd(unsigned n) {
	sRand = sRand * 1664525 + 1013904223;
	return n ? (sRand >> 8) % n : 0;
}

/* Due items first (oldest first), then up to five new ones in table order. */
static int buildQueue(enum Set s, int* q, int max) {
	uint32_t today = uiToday();
	int n = 0;
	int size = setSize(s);
	for (int pass = 0; pass < 2 && n < max; ++pass) {
		int fresh = 0;
		for (int i = 0; i < size && n < max; ++i) {
			int idx = setItem(s, i);
			char key[32];
			itemKey(s, idx, key, sizeof(key));
			struct KakuCard* c = storeGet(key, false);
			if (pass == 0 && c && c->due <= today) {
				q[n++] = idx;
			} else if (pass == 1 && !c && fresh < 5) {
				q[n++] = idx;
				++fresh;
			}
		}
	}
	if (n == 0) {
		/* everything is learned and nothing is due: free practice */
		for (int i = 0; i < max && i < size; ++i) {
			q[n++] = setItem(s, rnd(size));
		}
	}
	return n;
}

/* ---- the hanamaru: a teacher's red flower circle ---- */

static void drawHanamaru(float cx, float cy, float R, float progress) {
	struct KakuPt pts[400];
	int n = 0;
	/* inner spiral */
	for (int i = 0; i <= 120; ++i) {
		float t = i / 120.0f;
		float a = -M_PI / 2 + t * 4.2f * M_PI;
		float r = R * (0.08f + 0.42f * t);
		pts[n++] = (struct KakuPt) { cx + r * cosf(a), cy + r * sinf(a) };
	}
	/* petals around it */
	for (int i = 0; i <= 240; ++i) {
		float t = i / 240.0f;
		float a = -M_PI / 2 + 4.2f * M_PI + t * 2 * M_PI;
		float r = R * (0.62f + 0.3f * fabsf(sinf(t * 5 * M_PI)));
		pts[n++] = (struct KakuPt) { cx + r * cosf(a), cy + r * sinf(a) };
	}
	int shown = (int) (n * (progress > 1 ? 1 : progress));
	if (shown > 1) {
		uiThickLine(pts, shown, 1, 0, 0, 6, uiAlpha(C_HANAMARU, 0.92f));
	}
}

/* ---- writing ---- */

#define BOARD_X 452
#define BOARD_Y 28
#define BOARD_S 470
#define MAX_PTS 1500

struct Writer {
	struct KakuChar* ch;
	int nStrokes;
	int cur;
	int mistakes, missesHere;
	bool usedHelp;
	struct KakuPt user[MAX_PTS]; /* screen coordinates */
	int nUser;
	bool drawing;
	struct KakuPt failed[MAX_PTS];
	int nFailed;
	double failedAt;
	char msg[96];
	double msgAt;
	double demoAt;  /* "show me" animation start, 0 when off */
	double hintAt;  /* hint for the current stroke */
	double doneAt;
	bool trace;
};

static float boardScale(void) {
	return (BOARD_S - 40) / 109.0f;
}

static void drawBoard(void) {
	uiRound(BOARD_X + 4, BOARD_Y + 6, BOARD_S, BOARD_S, 18, RGBA(0, 0, 0, 0x55));
	uiRound(BOARD_X, BOARD_Y, BOARD_S, BOARD_S, 18, C_SHEET);
	uiRoundOutline(BOARD_X, BOARD_Y, BOARD_S, BOARD_S, 18, 2, C_LINE);
	/* dashed guide cross, like practice paper */
	float mx = BOARD_X + BOARD_S / 2.0f, my = BOARD_Y + BOARD_S / 2.0f;
	for (float d = BOARD_X + 16; d < BOARD_X + BOARD_S - 16; d += 22) {
		uiSegment(d, my, d + 10, my, 2, C_LINE_SOFT);
	}
	for (float d = BOARD_Y + 16; d < BOARD_Y + BOARD_S - 16; d += 22) {
		uiSegment(mx, d, mx, d + 10, 2, C_LINE_SOFT);
	}
}

static void refStroke(struct Writer* w, int i, uint32_t color, float width, float upto) {
	int n;
	const struct KakuPt* p = kakuCharStroke(w->ch, i, &n);
	int shown = (int) (n * upto);
	if (shown < 2) {
		return;
	}
	float s = boardScale();
	uiThickLine(p, shown, s, BOARD_X + 20, BOARD_Y + 20, width, color);
}

static void strokeNumber(struct Writer* w, int i, uint32_t color) {
	int n;
	const struct KakuPt* p = kakuCharStroke(w->ch, i, &n);
	float s = boardScale();
	char num[8];
	snprintf(num, sizeof(num), "%d", i + 1);
	float x = BOARD_X + 20 + p[0].x * s, y = BOARD_Y + 20 + p[0].y * s;
	uiCircle(x - 14, y - 14, 12, uiAlpha(C_BOARD, 0.85f));
	uiTextCentered(x - 14, y - 8, 15, color, num);
}

/* returns 1 when the character is finished, -1 to quit, 0 otherwise */
static int writerStep(struct Writer* w, double now) {
	float s = boardScale();
	bool inBoard = IN.x >= BOARD_X && IN.x < BOARD_X + BOARD_S && IN.y >= BOARD_Y && IN.y < BOARD_Y + BOARD_S;
	if (w->doneAt == 0 && w->demoAt == 0) {
		if (IN.pressed && inBoard) {
			w->drawing = true;
			w->nUser = 0;
		}
		if (w->drawing && IN.down && w->nUser < MAX_PTS) {
			struct KakuPt p = { IN.x, IN.y };
			if (!w->nUser || fabsf(p.x - w->user[w->nUser - 1].x) + fabsf(p.y - w->user[w->nUser - 1].y) > 1.5f) {
				w->user[w->nUser++] = p;
			}
		}
		if (w->drawing && IN.released) {
			w->drawing = false;
			struct KakuPt kv[MAX_PTS];
			for (int i = 0; i < w->nUser; ++i) {
				kv[i] = (struct KakuPt) { (w->user[i].x - BOARD_X - 20) / s, (w->user[i].y - BOARD_Y - 20) / s };
			}
			struct KakuResult r = kakuCheck(w->ch, kv, w->nUser, w->cur, sLeniency, w->trace);
			if (r.tap) {
				w->nUser = 0;
			} else if (r.isMatch) {
				struct KakuPt end = w->user[w->nUser - 1];
				uiDust(end.x, end.y, C_CHALK, 10);
				w->cur++;
				w->missesHere = 0;
				w->hintAt = 0;
				w->nUser = 0;
				if (w->cur >= w->nStrokes) {
					w->doneAt = now;
					uiDust(BOARD_X + BOARD_S / 2, BOARD_Y + BOARD_S / 2, C_YELLOW, 30);
				}
			} else {
				w->mistakes++;
				w->missesHere++;
				memcpy(w->failed, w->user, w->nUser * sizeof(struct KakuPt));
				w->nFailed = w->nUser;
				w->failedAt = now;
				w->nUser = 0;
				if (r.backwards) {
					snprintf(w->msg, sizeof(w->msg), "Right shape, wrong direction. Start from the other end.");
				} else if (r.wrongOrder >= 0) {
					snprintf(w->msg, sizeof(w->msg), "That's stroke %d. Stroke %d comes first.", r.wrongOrder + 1, w->cur + 1);
				} else {
					snprintf(w->msg, sizeof(w->msg), "Not quite. Try stroke %d again.", w->cur + 1);
				}
				w->msgAt = now;
				if (w->missesHere >= 2) {
					w->hintAt = now;
					w->usedHelp = true;
				}
			}
		}
	}

	/* drawing */
	drawBoard();
	if (w->trace) {
		for (int i = 0; i < w->nStrokes; ++i) {
			refStroke(w, i, uiAlpha(C_LINE, 0.55f), 12, 1);
		}
	}
	for (int i = 0; i < w->cur && i < w->nStrokes; ++i) {
		refStroke(w, i, C_CHALK, 13, 1);
	}
	if (w->hintAt && w->cur < w->nStrokes) {
		float p = (float) fmod((now - w->hintAt) * 0.8, 1.4);
		refStroke(w, w->cur, uiAlpha(C_GREEN, 0.7f), 12, p > 1 ? 1 : p);
		strokeNumber(w, w->cur, C_GREEN);
	}
	if (w->nFailed && now - w->failedAt < 0.9) {
		float a = (float) (1 - (now - w->failedAt) / 0.9);
		float shake = sinf((float) (now - w->failedAt) * 60) * 5 * a;
		uiThickLine(w->failed, w->nFailed, 1, shake, 0, 11, uiAlpha(C_RED, a));
	}
	if (w->nUser > 1) {
		uiThickLine(w->user, w->nUser, 1, 0, 0, 12, uiAlpha(C_CHALK, 0.9f));
	}
	if (w->demoAt) {
		float t = (float) (now - w->demoAt) * 1.3f;
		for (int i = 0; i < w->nStrokes; ++i) {
			float p = t - i;
			if (p <= 0) {
				break;
			}
			refStroke(w, i, C_YELLOW, 13, p > 1 ? 1 : p);
			strokeNumber(w, i, C_YELLOW);
		}
		if (t > w->nStrokes + 0.8f || IN.pressed) {
			w->demoAt = 0;
		}
	}
	if (w->doneAt) {
		float p = (float) (now - w->doneAt) * 1.6f;
		if (w->mistakes == 0 && !w->usedHelp) {
			drawHanamaru(BOARD_X + BOARD_S - 70, BOARD_Y + 70, 52, p);
		} else {
			struct KakuPt tick[3] = { { BOARD_X + BOARD_S - 110, BOARD_Y + 70 },
				                      { BOARD_X + BOARD_S - 88, BOARD_Y + 96 },
				                      { BOARD_X + BOARD_S - 40, BOARD_Y + 36 } };
			uiThickLine(tick, p > 1 ? 3 : 2, 1, 0, 0, 7, uiAlpha(C_HANAMARU, p));
		}
		if (now - w->doneAt > 1.1 || (now - w->doneAt > 0.3 && (IN.released || uiOk()))) {
			return 1;
		}
	}
	if (w->msg[0] && now - w->msgAt < 2.5 && !w->doneAt) {
		float a = (float) (2.5 - (now - w->msgAt));
		uiTextCentered(BOARD_X + BOARD_S / 2, BOARD_Y + BOARD_S - 18, 16, uiAlpha(C_RED, a), w->msg);
	}
	return uiBack() ? -1 : 0;
}

static void drawProgress(int i, int n, float x, float y) {
	for (int k = 0; k < n; ++k) {
		uint32_t c = k < i ? C_GREEN : (k == i ? C_YELLOW : C_LINE_SOFT);
		uiCircle(x + k * 18, y, k == i ? 6 : 5, c);
	}
}

static enum KakuGrade gradeFor(const struct Writer* w) {
	if (w->usedHelp || w->mistakes >= 3) {
		return KAKU_AGAIN;
	}
	if (w->mistakes > 0) {
		return KAKU_HARD;
	}
	return KAKU_GOOD;
}

static void summary(const char* title, int good, int total) {
	double start = uiTime();
	while (true) {
		uiPollInput();
		double now = uiTime();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(480, 150, 30, C_YELLOW, title);
		char buf[96];
		snprintf(buf, sizeof(buf), "%d of %d without help", good, total);
		uiTextCentered(480, 200, 22, C_CHALK, buf);
		if (good == total && total > 0) {
			drawHanamaru(480, 330, 90, (float) (now - start) * 1.2f);
			uiJpCentered(480, 470, 26, C_HANAMARU, "よくできました！");
		} else {
			uiJpCentered(480, 330, 40, C_GREEN, "がんばって！");
			uiTextCentered(480, 380, 18, C_SOFT, "The ones you missed come back sooner.");
		}
		uiTextCentered(480, 520, 16, C_SOFT, "Tap to continue");
		uiFrameEnd();
		if (now - start > 0.5 && (IN.released || uiOk() || uiBack())) {
			return;
		}
	}
}

static void sessionWrite(enum Set set) {
	int queue[10];
	int n = buildQueue(set, queue, 10);
	int clean = 0;
	for (int qi = 0; qi < n; ++qi) {
		int idx = queue[qi];
		struct Writer* w = calloc(1, sizeof(*w));
		const char* ch = itemChar(set, idx);
		char key[32];
		itemKey(set, idx, key, sizeof(key));
		struct KakuCard* card = storeGet(key, false);
		bool fresh = !card || (card->reps == 0 && card->lapses == 0);
		w->trace = sAlwaysTrace || fresh;
		if (set == SET_KANJI) {
			w->ch = kakuCharLoad(KAKU_KANJI[idx].firstStroke, KAKU_KANJI[idx].nStrokes);
		} else {
			w->ch = kakuCharLoad(KAKU_KANA[idx].firstStroke, KAKU_KANA[idx].nStrokes);
		}
		w->nStrokes = kakuCharStrokes(w->ch);
		int result = 0;
		while (result == 0) {
			uiPollInput();
			double now = uiTime();
			static double last;
			float dt = last ? (float) (now - last) : 0;
			last = now;
			uiDustUpdate(dt > 0.1f ? 0.1f : dt);
			uiFrameBegin();
			uiChalkBackground();
			/* prompt */
			drawProgress(qi, n, 40, 40);
			if (set == SET_KANJI) {
				const struct KakuKanji* k = &KAKU_KANJI[idx];
				uiText(36, 96, 15, C_SOFT, "Write the kanji for");
				uiTextWrap(36, 140, 380, 30, 38, C_CHALK, k->meaning, 3);
				if (k->kun[0]) {
					uiText(36, 268, 14, C_SOFT, "kun");
					uiJp(80, 270, 22, C_YELLOW, k->kun);
				}
				if (k->on[0]) {
					uiText(36, 306, 14, C_SOFT, "on");
					uiJp(80, 308, 22, C_GREEN, k->on);
				}
			} else {
				const struct KakuKana* k = &KAKU_KANA[idx];
				uiText(36, 96, 15, C_SOFT, isHiragana(k->ch) ? "Write in hiragana" : "Write in katakana");
				uiText(36, 200, 84, C_CHALK, k->romaji);
			}
			char buf[64];
			snprintf(buf, sizeof(buf), "stroke %d of %d", w->cur < w->nStrokes ? w->cur + 1 : w->nStrokes, w->nStrokes);
			uiText(36, 350, 17, C_MUTED, buf);
			if (fresh && w->cur == 0 && !w->doneAt) {
				uiTextWrap(36, 380, 380, 15, 21, C_GREEN, "New one! Trace the outline, then it comes back without it.", 2);
			}
			bool demo = uiButton(36, 420, 170, 54, "Show me", NULL, C_YELLOW, false) || (IN.buttonsPressed & SCE_CTRL_TRIANGLE);
			bool skip = uiButton(222, 420, 120, 54, "Skip", NULL, C_SOFT, false);
			uiText(36, 510, 14, C_SOFT, "△ Show me    ○ Back");
			if (demo && !w->doneAt) {
				w->demoAt = now;
				w->usedHelp = true;
			}
			result = writerStep(w, now);
			if (skip && !w->doneAt) {
				w->usedHelp = true;
				result = 1;
			}
			uiDustDraw();
			/* the finished character, written properly, beside the prompt */
			if (w->doneAt) {
				float a = (float) (now - w->doneAt) * 3;
				uiJp(250, 300, 110, uiAlpha(C_CHALK, a), ch);
			}
			uiFrameEnd();
		}
		if (result == 1) {
			card = storeGet(key, true);
			enum KakuGrade g = gradeFor(w);
			if (g == KAKU_GOOD) {
				++clean;
			}
			kakuCardGrade(card, g, uiToday());
			storeSave();
		}
		kakuCharFree(w->ch);
		free(w);
		if (result == -1) {
			return;
		}
	}
	summary("Practice done", clean, n);
}

/* ---- reading quiz ---- */

static int pickDistinct(int* out, int have, int candidate) {
	for (int i = 0; i < have; ++i) {
		if (out[i] == candidate) {
			return have;
		}
	}
	out[have] = candidate;
	return have + 1;
}

static int kanaIndexOf(const char* ch) {
	for (int i = 0; i < KAKU_KANA_COUNT; ++i) {
		if (!strcmp(KAKU_KANA[i].ch, ch)) {
			return i;
		}
	}
	return -1;
}

static void sessionRead(enum Set set) {
	int size = setSize(set);
	int total = 12, good = 0;
	for (int q = 0; q < total; ++q) {
		int idx = setItem(set, rnd(size));
		int opts[4];
		int no = 0;
		no = pickDistinct(opts, no, idx);
		bool reverse = set != SET_KANJI && (q % 2 == 1); /* show the sound, pick the character */
		if (set == SET_KANJI) {
			int guard = 0;
			while (no < 4 && guard++ < 200) {
				int c = rnd(KAKU_KANJI_COUNT);
				if (KAKU_KANJI[c].group == KAKU_KANJI[idx].group || guard > 60) {
					no = pickDistinct(opts, no, c);
				}
			}
		} else {
			/* lookalikes make the wrong answers tempting */
			const char* look = KAKU_KANA[idx].lookalikes;
			uint32_t cps[16];
			int nl = lingoUtf8Decode(look, cps, 16);
			for (int i = 0; i < nl && no < 4; ++i) {
				char ch[8] = { 0 };
				lingoUtf8Encode(cps[i], ch);
				int k = kanaIndexOf(ch);
				if (k >= 0 && strcmp(KAKU_KANA[k].romaji, KAKU_KANA[idx].romaji)) {
					no = pickDistinct(opts, no, k);
				}
			}
			while (no < 4) {
				no = pickDistinct(opts, no, setItem(set, rnd(size)));
			}
		}
		/* shuffle */
		for (int i = 3; i > 0; --i) {
			int j = rnd(i + 1);
			int t = opts[i];
			opts[i] = opts[j];
			opts[j] = t;
		}
		int chosen = -1;
		double chosenAt = 0;
		while (true) {
			uiPollInput();
			double now = uiTime();
			static double last;
			float dt = last ? (float) (now - last) : 0;
			last = now;
			uiDustUpdate(dt > 0.1f ? 0.1f : dt);
			uiFrameBegin();
			uiChalkBackground();
			drawProgress(q, total, 40, 40);
			const char* prompt;
			if (set == SET_KANJI) {
				uiText(40, 100, 16, C_SOFT, "What does this kanji mean?");
				prompt = KAKU_KANJI[idx].ch;
				uiJpCentered(250, 360, 170, C_CHALK, prompt);
			} else if (reverse) {
				uiText(40, 100, 16, C_SOFT, isHiragana(KAKU_KANA[idx].ch) ? "Which hiragana is this?" : "Which katakana is this?");
				uiTextCentered(250, 330, 96, C_CHALK, KAKU_KANA[idx].romaji);
			} else {
				uiText(40, 100, 16, C_SOFT, "How does this read?");
				uiJpCentered(250, 360, 170, C_CHALK, KAKU_KANA[idx].ch);
			}
			for (int i = 0; i < 4; ++i) {
				int o = opts[i];
				float bx = 500, by = 90 + i * 104, bw = 420, bh = 86;
				uint32_t edge = C_LINE;
				if (chosen >= 0) {
					if (o == idx) {
						edge = C_GREEN;
					} else if (o == chosen) {
						edge = C_RED;
					}
				}
				float shake = 0;
				if (chosen >= 0 && o == chosen && o != idx) {
					shake = sinf((float) (now - chosenAt) * 50) * 6 * fmaxf(0, 1 - (float) (now - chosenAt) * 2);
				}
				uiRound(bx + shake, by, bw, bh, 16, chosen >= 0 && o == idx ? uiAlpha(C_GREEN, 0.18f) : C_SHEET);
				uiRoundOutline(bx + shake, by, bw, bh, 16, 3, edge);
				if (set == SET_KANJI) {
					uiTextWrap(bx + 24 + shake, by + 37, bw - 48, 19, 25, C_CHALK, KAKU_KANJI[o].meaning, 2);
				} else if (reverse) {
					uiJpCentered(bx + bw / 2 + shake, by + 66, 50, C_CHALK, KAKU_KANA[o].ch);
				} else {
					uiTextCentered(bx + bw / 2 + shake, by + 58, 34, C_CHALK, KAKU_KANA[o].romaji);
				}
				if (chosen < 0 && uiTapped(bx, by, bw, bh)) {
					chosen = o;
					chosenAt = now;
					if (o == idx) {
						++good;
						uiDust(bx + bw / 2, by + bh / 2, C_GREEN, 18);
					}
				}
			}
			uiText(40, 510, 14, C_SOFT, "○ Back");
			uiDustDraw();
			uiFrameEnd();
			if (uiBack()) {
				return;
			}
			if (chosen >= 0 && now - chosenAt > (chosen == idx ? 0.6 : 1.4)) {
				break;
			}
		}
	}
	summary("Quiz done", good, total);
}

/* ---- saved words ---- */

void kakuReviewWords(void) {
	uiInit();
	uint32_t today = uiToday();
	int order[512];
	int n = 0;
	for (int i = 0; i < deckCount() && n < 512; ++i) {
		const struct SavedWord* w = deckGet(i);
		char key[60];
		snprintf(key, sizeof(key), "w:%s|%s", w->head, w->reading);
		struct KakuCard* c = storeGet(key, false);
		if (!c || c->due <= today) {
			order[n++] = i;
		}
	}
	bool practiceAll = false;
	if (n == 0) {
		for (int i = 0; i < deckCount() && n < 512; ++i) {
			order[n++] = i;
		}
		practiceAll = true;
	}
	if (n == 0) {
		while (true) {
			uiPollInput();
			uiFrameBegin();
			uiChalkBackground();
			uiJpCentered(480, 220, 60, uiAlpha(C_CHALK, 0.3f), "語");
			uiTextCentered(480, 290, 22, C_CHALK, "No saved words yet.");
			uiTextCentered(480, 330, 17, C_SOFT, "While playing, tap a word in the study panel and press ☆ Save.");
			uiTextCentered(480, 520, 16, C_SOFT, "Tap to go back");
			uiFrameEnd();
			if (IN.released || uiBack() || uiOk()) {
				return;
			}
		}
	}
	for (int i = n - 1; i > 0; --i) {
		int j = rnd(i + 1);
		int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	if (n > 20) {
		n = 20;
	}
	int good = 0;
	for (int qi = 0; qi < n; ++qi) {
		const struct SavedWord* w = deckGet(order[qi]);
		char key[60];
		snprintf(key, sizeof(key), "w:%s|%s", w->head, w->reading);
		bool revealed = false;
		double revealAt = 0;
		int grade = -1;
		while (grade < 0) {
			uiPollInput();
			double now = uiTime();
			uiFrameBegin();
			uiChalkBackground();
			drawProgress(qi, n, 40, 34);
			if (practiceAll) {
				uiText(40, 70, 14, C_SOFT, "Nothing due, so here is a free round.");
			}
			uiRound(60, 90, 840, 320, 22, C_SHEET);
			uiRoundOutline(60, 90, 840, 320, 22, 2, C_LINE);
			uiJpCentered(480, 200, 64, C_CHALK, w->head);
			if (!revealed) {
				uiTextCentered(480, 300, 18, C_SOFT, "Say it, then tap to check");
				if (IN.released || uiOk()) {
					revealed = true;
					revealAt = now;
				}
			} else {
				float a = (float) (now - revealAt) * 4;
				if (strcmp(w->head, w->reading)) {
					uiJpCentered(480, 250, 26, uiAlpha(C_YELLOW, a), w->reading);
				}
				uiTextWrap(100, 296, 760, 18, 26, uiAlpha(C_CHALK, a), w->gloss, 2);
				if (w->context[0]) {
					uiText(100, 368, 13, uiAlpha(C_SOFT, a), "from the game");
					uiJp(100, 396, 17, uiAlpha(C_MUTED, a), w->context);
				}
				struct KakuCard* c = storeGet(key, true);
				static const char* const names[4] = { "Again", "Hard", "Good", "Easy" };
				static const uint32_t cols[4] = { C_RED, C_YELLOW, C_GREEN, C_BLUE };
				for (int g = 0; g < 4; ++g) {
					int days = kakuCardPreview(c, (enum KakuGrade) g);
					char label[48];
					if (days == 0) {
						snprintf(label, sizeof(label), "%s · today", names[g]);
					} else {
						snprintf(label, sizeof(label), "%s · %dd", names[g], days);
					}
					if (uiButton(60 + g * 214, 436, 198, 60, label, NULL, cols[g], false)) {
						grade = g;
					}
				}
			}
			uiText(40, 528, 14, C_SOFT, "○ Back");
			uiFrameEnd();
			if (uiBack()) {
				storeSave();
				return;
			}
		}
		struct KakuCard* c = storeGet(key, true);
		kakuCardGrade(c, (enum KakuGrade) grade, today);
		if (grade >= KAKU_GOOD) {
			++good;
		}
	}
	storeSave();
	summary("Review done", good, n);
}

/* ---- hub ---- */

void kakuRun(void) {
	uiInit();
	double start = uiTime();
	int focus = 0;
	while (true) {
		uiPollInput();
		double now = uiTime();
		uint32_t today = uiToday();
		uiFrameBegin();
		uiChalkBackground();
		uiJp(36, 74, 52, C_CHALK, "書く");
		uiText(150, 64, 26, C_YELLOW, "Kaku");
		uiText(152, 88, 15, C_SOFT, "Write it, read it, keep it.");
		/* settings chips */
		static const char* const len[3] = { "Relaxed", "Normal", "Strict" };
		static const float lv[3] = { KAKU_LENIENCY_RELAXED, KAKU_LENIENCY_NORMAL, KAKU_LENIENCY_STRICT };
		for (int i = 0; i < 3; ++i) {
			bool on = sLeniency == lv[i];
			float x = 560 + i * 102;
			uiRound(x, 30, 94, 34, 12, on ? uiAlpha(C_YELLOW, 0.25f) : C_SHEET);
			uiTextCentered(x + 47, 53, 15, on ? C_YELLOW : C_SOFT, len[i]);
			if (uiTapped(x, 30, 94, 34)) {
				sLeniency = lv[i];
			}
		}
		{
			float x = 560, y = 72;
			uiRound(x, y, 298, 30, 12, sAlwaysTrace ? uiAlpha(C_GREEN, 0.25f) : C_SHEET);
			uiTextCentered(x + 149, y + 21, 14, sAlwaysTrace ? C_GREEN : C_SOFT,
			               sAlwaysTrace ? "Outline: always shown" : "Outline: new characters only");
			if (uiTapped(x, y, 298, 30)) {
				sAlwaysTrace = !sAlwaysTrace;
			}
		}
		struct Tile {
			const char* jp;
			const char* label;
			enum Set set;
			bool write;
			uint32_t col;
		} tiles[6] = {
			{ "あ", "Hiragana · write", SET_HIRAGANA, true, C_CHALK },
			{ "あ", "Hiragana · read", SET_HIRAGANA, false, C_SOFT },
			{ "ア", "Katakana · write", SET_KATAKANA, true, C_BLUE },
			{ "ア", "Katakana · read", SET_KATAKANA, false, C_SOFT },
			{ "日", "Kanji · write", SET_KANJI, true, C_YELLOW },
			{ "日", "Kanji · meaning", SET_KANJI, false, C_SOFT },
		};
		if (IN.buttonsPressed & SCE_CTRL_RIGHT) {
			focus = (focus + 1) % 7;
		}
		if (IN.buttonsPressed & SCE_CTRL_LEFT) {
			focus = (focus + 6) % 7;
		}
		if (IN.buttonsPressed & SCE_CTRL_DOWN) {
			focus = focus < 4 ? focus + 2 : 6;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			focus = focus == 6 ? 4 : (focus >= 2 ? focus - 2 : focus);
		}
		int pick = -1;
		for (int i = 0; i < 6; ++i) {
			float x = 36 + (i % 2) * 450, y = 128 + (i / 2) * 104;
			float pop = fminf(1, (float) (now - start) * 4 - i * 0.15f);
			if (pop <= 0) {
				continue;
			}
			char buf[64];
			if (tiles[i].write) {
				int due = dueCount(tiles[i].set, today);
				snprintf(buf, sizeof(buf), "%s   %d/%d%s", tiles[i].label, learnedCount(tiles[i].set), setSize(tiles[i].set),
				         due ? "  ·  due" : "");
			} else {
				snprintf(buf, sizeof(buf), "%s", tiles[i].label);
			}
			if (uiButton(x, y + (1 - pop) * 20, 430, 86, buf, tiles[i].jp, tiles[i].col, focus == i)) {
				pick = i;
			}
		}
		char wbuf[64];
		int dueW = storeDueCount("w:", today);
		snprintf(wbuf, sizeof(wbuf), "Saved words from games  ·  %d%s", deckCount(), dueW ? "  ·  due" : "");
		if (uiButton(36, 440, 880, 70, wbuf, "語", C_PINK, focus == 6)) {
			pick = 6;
		}
		uiText(36, 534, 14, C_SOFT, "○ Back");
		uiDustDraw();
		uiFrameEnd();
		if (uiBack()) {
			storeSave();
			return;
		}
		if (pick >= 0 && pick < 6) {
			if (tiles[pick].write) {
				sessionWrite(tiles[pick].set);
			} else {
				sessionRead(tiles[pick].set);
			}
			start = uiTime();
		} else if (pick == 6) {
			kakuReviewWords();
			start = uiTime();
		}
	}
}
