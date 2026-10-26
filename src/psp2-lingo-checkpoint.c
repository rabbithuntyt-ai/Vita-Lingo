/* Lingo checkpoints on the Vita: the test that stands between a save and
 * playing on. Words met in this game come first; saved words and kana
 * fill in for games the panel cannot read. */
#include "psp2-lingo-quiz.h"
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>

#define TEST_LEN 5
#define PASS_MARK 4

/* Kana questions for when there are not enough words yet. */
static int kanaQuestions(struct LingoQuestion* q, int max, unsigned seed) {
	unsigned s = seed * 747796405u + 1;
	int n = 0;
	for (int tries = 0; tries < 200 && n < max; ++tries) {
		s = s * 1664525 + 1013904223;
		int idx = (s >> 8) % KAKU_KANA_COUNT;
		struct LingoQuestion* x = &q[n];
		memset(x, 0, sizeof(*x));
		x->kind = LQ_READING;
		x->met = -1;
		snprintf(x->prompt, sizeof(x->prompt), "%s", KAKU_KANA[idx].ch);
		snprintf(x->sub, sizeof(x->sub), "How does this read?");
		snprintf(x->word, sizeof(x->word), "%s", KAKU_KANA[idx].ch);
		snprintf(x->reading, sizeof(x->reading), "%s", KAKU_KANA[idx].romaji);
		snprintf(x->meaning, sizeof(x->meaning), "reads %s", KAKU_KANA[idx].romaji);
		const char* opts[4] = { KAKU_KANA[idx].romaji, NULL, NULL, NULL };
		int no = 1;
		for (int t = 0; t < 60 && no < 4; ++t) {
			s = s * 1664525 + 1013904223;
			const char* r = KAKU_KANA[(s >> 8) % KAKU_KANA_COUNT].romaji;
			bool dup = false;
			for (int k = 0; k < no; ++k) {
				if (!strcmp(opts[k], r)) {
					dup = true;
				}
			}
			if (!dup) {
				opts[no++] = r;
			}
		}
		if (no < 4) {
			continue;
		}
		s = s * 1664525 + 1013904223;
		x->answer = (s >> 8) % 4;
		for (int i = 0, k = 1; i < 4; ++i) {
			snprintf(x->options[i], sizeof(x->options[i]), "%s", i == x->answer ? opts[0] : opts[k++]);
		}
		++n;
	}
	return n;
}

static int buildTest(const struct LingoDict* d, struct LingoBook* book, struct LingoQuestion* q, unsigned seed) {
	int nExtra = deckCount();
	struct LingoExtra* extra = nExtra ? calloc(nExtra, sizeof(*extra)) : NULL;
	for (int i = 0; i < nExtra; ++i) {
		const struct SavedWord* w = deckGet(i);
		extra[i] = (struct LingoExtra) { w->head, w->reading, w->gloss, w->context };
	}
	int n = d ? lingoBuildTest(d, book, extra, nExtra, seed, q, TEST_LEN) : 0;
	free(extra);
	if (n < TEST_LEN) {
		n += kanaQuestions(q + n, TEST_LEN - n, seed);
	}
	return n;
}

static void drawIntro(int attempt, int passed, double t) {
	uiChalkBackground();
	float pulse = 0.85f + 0.15f * sinf((float) t * 3);
	uiJpCentered(480, 150, 56, uiAlpha(C_YELLOW, pulse), "チェックポイント");
	uiTextCentered(480, 200, 22, C_CHALK, attempt ? "Let's try that again." : "Your game is saved. Before you carry on,");
	uiTextCentered(480, 232, 22, C_CHALK, attempt ? "Fresh questions, same words." : "show what you've picked up so far.");
	char buf[96];
	snprintf(buf, sizeof(buf), "%d questions · get %d right to keep playing", TEST_LEN, PASS_MARK);
	uiTextCentered(480, 290, 17, C_SOFT, buf);
	if (passed > 0) {
		snprintf(buf, sizeof(buf), "Checkpoints passed in this game: %d", passed);
		uiTextCentered(480, 318, 15, C_GREEN, buf);
	}
}

/* Runs one question. Returns 1 right, 0 wrong. */
static int runQuestion(const struct LingoQuestion* q, int i, int n, int score) {
	int picked = -1;
	double pickedAt = 0;
	while (true) {
		uiPollInput();
		double now = uiTime();
		static double last;
		float dt = last ? (float) (now - last) : 0;
		last = now;
		uiDustUpdate(dt > 0.1f ? 0.1f : dt);
		uiFrameBegin();
		uiChalkBackground();
		/* progress */
		for (int k = 0; k < n; ++k) {
			uiCircle(40 + k * 22, 36, k == i ? 7 : 5, k < i ? C_GREEN : (k == i ? C_YELLOW : C_LINE_SOFT));
		}
		char sc[32];
		snprintf(sc, sizeof(sc), "%d / %d right", score, i);
		uiText(SCREEN_W - 150, 42, 15, C_SOFT, sc);
		const char* ask = q->kind == LQ_CLOZE ? "Which word fills the gap?" :
		                  q->kind == LQ_READING ? (q->met >= 0 ? "How is this read?" : "How does this read?") :
		                                          "What does this mean?";
		uiText(40, 92, 17, C_SOFT, ask);
		if (q->kind == LQ_CLOZE) {
			uiRound(40, 112, SCREEN_W - 80, 108, 18, C_SHEET);
			uiJp(66, 162, 24, C_CHALK, q->prompt);
			uiText(66, 202, 15, C_YELLOW, q->sub);
		} else {
			float px = 72;
			while (px > 30 && uiJpW(px, q->prompt) > SCREEN_W - 120) {
				px -= 4;
			}
			uiJpCentered(SCREEN_W / 2, 190, px, C_CHALK, q->prompt);
			if (q->sub[0] && q->kind != LQ_READING) {
				uiJpCentered(SCREEN_W / 2, 226, 22, C_YELLOW, q->sub);
			} else if (q->met >= 0 && q->sub[0]) {
				uiTextCentered(SCREEN_W / 2, 226, 16, C_SOFT, q->sub);
			}
		}
		for (int o = 0; o < 4; ++o) {
			float bx = 40 + (o % 2) * 450, by = 250 + (o / 2) * 96, bw = 430, bh = 84;
			uint32_t edge = C_LINE;
			if (picked >= 0) {
				edge = o == q->answer ? C_GREEN : (o == picked ? C_RED : C_LINE_SOFT);
			}
			uiRound(bx, by, bw, bh, 16, picked >= 0 && o == q->answer ? uiAlpha(C_GREEN, 0.15f) : C_SHEET);
			uiRoundOutline(bx, by, bw, bh, 16, 3, edge);
			if (q->kind == LQ_MEANING || (q->kind == LQ_READING && q->met < 0)) {
				uiTextWrap(bx + 20, by + 36, bw - 40, 17, 23, C_CHALK, q->options[o], 2);
			} else {
				uiJp(bx + 20, by + 54, 26, C_CHALK, q->options[o]);
			}
			if (picked < 0 && uiTapped(bx, by, bw, bh)) {
				picked = o;
				pickedAt = now;
				if (o == q->answer) {
					uiDust(bx + bw / 2, by + bh / 2, C_GREEN, 18);
				}
			}
		}
		if (picked >= 0 && q->context[0] && q->kind != LQ_CLOZE) {
			uiText(40, 466, 13, C_SOFT, "You met it here:");
			uiJp(40, 492, 17, C_MUTED, q->context);
		}
		uiDustDraw();
		uiFrameEnd();
		if (picked >= 0 && now - pickedAt > (picked == q->answer ? 0.7 : 1.8)) {
			return picked == q->answer;
		}
	}
}

static void runReview(struct LingoQuestion* q, const int* wrong, int nWrong, int score, int n) {
	double start = uiTime();
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		char buf[96];
		snprintf(buf, sizeof(buf), "%d of %d. You need %d to carry on.", score, n, PASS_MARK);
		uiTextCentered(480, 70, 24, C_YELLOW, buf);
		uiTextCentered(480, 102, 16, C_SOFT, "Here are the ones you missed. Have a look, then try again.");
		float y = 130;
		for (int k = 0; k < nWrong && y < 440; ++k) {
			const struct LingoQuestion* x = &q[wrong[k]];
			uiRound(40, y, SCREEN_W - 80, 96, 16, C_SHEET);
			uiJp(64, y + 42, 28, C_CHALK, x->word);
			float w = uiJpW(28, x->word);
			if (strcmp(x->word, x->reading)) {
				uiJp(80 + w, y + 40, 18, C_YELLOW, x->reading);
				w += uiJpW(18, x->reading) + 16;
			}
			uiTextWrap(96 + w, y + 38, SCREEN_W - 180 - w, 16, 21, C_CHALK, x->meaning, 1);
			if (x->context[0]) {
				uiJp(64, y + 78, 15, C_MUTED, x->context);
			}
			y += 106;
		}
		bool go = uiButton(SCREEN_W / 2 - 120, 462, 240, 56, "Try again", NULL, C_YELLOW, true);
		uiFrameEnd();
		if (uiTime() - start > 0.6 && go) {
			return;
		}
	}
}

static void celebrate(int passed) {
	double start = uiTime();
	while (true) {
		uiPollInput();
		double now = uiTime();
		static double last;
		float dt = last ? (float) (now - last) : 0;
		last = now;
		uiDustUpdate(dt > 0.1f ? 0.1f : dt);
		uiFrameBegin();
		uiChalkBackground();
		float t = (float) (now - start);
		/* a teacher's red circle, then the go-ahead */
		struct KakuPt pts[200];
		int np = 0;
		for (int i = 0; i < 200; ++i) {
			float u = i / 199.0f;
			float a = -M_PI / 2 + u * 2.15f * M_PI;
			float r = 110 * (0.62f + 0.3f * fabsf(sinf(u * 5 * M_PI)));
			pts[np++] = (struct KakuPt) { 480 + r * cosf(a), 220 + r * sinf(a) };
		}
		int shown = (int) (np * fminf(1, t * 1.4f));
		if (shown > 1) {
			uiThickLine(pts, shown, 1, 0, 0, 7, C_HANAMARU);
		}
		uiJpCentered(480, 240, 48, uiAlpha(C_CHALK, t * 2 - 0.4f), "ごうかく！");
		char buf[64];
		snprintf(buf, sizeof(buf), "Checkpoint %d passed. Back to your game.", passed);
		uiTextCentered(480, 400, 20, uiAlpha(C_YELLOW, t * 2 - 0.8f), buf);
		if (t > 0.2f && t < 0.3f) {
			uiDust(480, 220, C_YELLOW, 40);
		}
		uiDustDraw();
		uiTextCentered(480, 510, 15, C_SOFT, "Tap to play");
		uiFrameEnd();
		if (t > 1.0f && (IN.released || uiOk())) {
			return;
		}
	}
}

void lingoCheckpointTest(const struct LingoDict* d, struct LingoBook* book) {
	uiInit();
	static struct LingoQuestion q[TEST_LEN];
	unsigned seed = (unsigned) (uiTime() * 1000) ^ (unsigned) book->passed * 7919u;
	for (int attempt = 0;; ++attempt) {
		/* intro */
		double start = uiTime();
		while (true) {
			uiPollInput();
			uiFrameBegin();
			drawIntro(attempt, book->passed, uiTime() - start);
			bool go = uiButton(SCREEN_W / 2 - 120, 400, 240, 60, "Start", NULL, C_YELLOW, true);
			uiFrameEnd();
			if (uiTime() - start > 0.6 && go) {
				break;
			}
		}
		int n = buildTest(d, book, q, seed + attempt * 101);
		int score = 0, wrong[TEST_LEN], nWrong = 0;
		for (int i = 0; i < n; ++i) {
			int r = runQuestion(&q[i], i, n, score);
			score += r;
			if (!r) {
				wrong[nWrong++] = i;
			}
			if (q[i].met >= 0 && q[i].met < book->n) {
				struct LingoMet* m = &book->items[q[i].met];
				if (r && m->right < 255) {
					m->right++;
				} else if (!r && m->wrong < 255) {
					m->wrong++;
				}
			}
		}
		int need = n < TEST_LEN ? (n * 4 + 4) / 5 : PASS_MARK;
		if (score >= need) {
			lingoCheckpointPassed(book);
			celebrate(book->passed);
			return;
		}
		runReview(q, wrong, nWrong, score, n);
	}
}
