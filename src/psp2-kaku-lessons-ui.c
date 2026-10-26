/* Kaku lessons on the Vita: catch-up classes (one idea at a time, then a
 * check) and units (grammar notes, then drills where every wrong answer
 * explains itself). */
#include "psp2-kaku-lessons.h"
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <vita2d.h>

static unsigned sSeed = 77;
static int rnd(int n) {
	sSeed = sSeed * 1664525 + 1013904223;
	return n > 0 ? (int) ((sSeed >> 8) % (unsigned) n) : 0;
}

static bool done(const char* prefix, int i) {
	char key[32];
	snprintf(key, sizeof(key), "%s%d", prefix, i);
	struct KakuCard* c = storeGet(key, false);
	return c && c->reps > 0;
}

static void markDone(const char* prefix, int i, bool clean) {
	char key[32];
	snprintf(key, sizeof(key), "%s%d", prefix, i);
	struct KakuCard* c = storeGet(key, true);
	kakuCardGrade(c, clean ? KAKU_GOOD : KAKU_HARD, uiToday());
	storeSave();
}

/* ---- shared pieces ---- */

static void frameHeader(const char* kicker, const char* title, int step, int steps) {
	uiText(40, 44, 14, C_SOFT, kicker);
	uiTextWrap(40, 76, 700, 22, 28, C_YELLOW, title, 1);
	for (int k = 0; k < steps && steps <= 40; ++k) {
		float x = SCREEN_W - 40 - (steps - k) * 13;
		uiCircle(x, 40, k == step ? 5 : 3.5f, k < step ? C_GREEN : (k == step ? C_YELLOW : C_LINE_SOFT));
	}
}

static int navButtons(bool canBack, const char* nextLabel) {
	int r = 0;
	if (canBack && (uiButton(40, 462, 150, 56, "◀ Back", NULL, C_SOFT, false) || (IN.buttonsPressed & SCE_CTRL_LEFT))) {
		r = -1;
	}
	if (nextLabel && (uiButton(SCREEN_W - 230, 462, 190, 56, nextLabel, NULL, C_YELLOW, true) ||
	                  (IN.buttonsPressed & SCE_CTRL_RIGHT))) {
		r = 1;
	}
	return r;
}

static void sheet(float y, float h) {
	uiRound(40, y, SCREEN_W - 80, h, 20, C_SHEET);
	uiRoundOutline(40, y, SCREEN_W - 80, h, 20, 2, C_LINE_SOFT);
}

/* A multiple-choice question. Options with their own explanation show it
 * when picked. Returns 1 right, 0 wrong (after the player moves on), -1 quit. */
static int ask(const char* kicker, const char* title, int step, int steps, const char* q, const char* ctx,
               const char* const* opts, const char* const* optWhy, int n, int answer, const char* why) {
	int order[4] = { 0, 1, 2, 3 };
	for (int i = n - 1; i > 0; --i) {
		int j = rnd(i + 1);
		int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
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
		frameHeader(kicker, title, step, steps);
		float y = 112;
		y += uiTextWrap(40, y + 12, SCREEN_W - 80, 19, 27, C_CHALK, q, 3) + 4;
		if (ctx && ctx[0]) {
			y += uiTextWrap(40, y + 10, SCREEN_W - 80, 15, 22, C_SOFT, ctx, 2);
		}
		float oy = fmaxf(y + 10, 178);
		float oh = n > 3 ? 52 : 60;
		for (int k = 0; k < n; ++k) {
			int o = order[k];
			float by = oy + k * (oh + 10);
			bool isAns = o == answer;
			uint32_t edge = C_LINE;
			if (picked >= 0) {
				edge = isAns ? C_GREEN : (o == picked ? C_RED : C_LINE_SOFT);
			}
			uiRound(40, by, SCREEN_W - 80, oh, 14, picked >= 0 && isAns ? uiAlpha(C_GREEN, 0.15f) : C_SHEET);
			uiRoundOutline(40, by, SCREEN_W - 80, oh, 14, 2, edge);
			uiJp(62, by + oh / 2 + 9, 22, C_CHALK, opts[o]);
			if (picked < 0 && uiTapped(40, by, SCREEN_W - 80, oh)) {
				picked = o;
				pickedAt = now;
				if (isAns) {
					uiDust(SCREEN_W / 2, by + oh / 2, C_GREEN, 16);
				}
			}
		}
		if (picked >= 0) {
			float fy = oy + n * (oh + 10) + 6;
			if (picked != answer && optWhy && optWhy[picked] && optWhy[picked][0]) {
				fy += uiTextWrap(40, fy + 14, SCREEN_W - 80, 15, 21, C_RED, optWhy[picked], 2) + 2;
			}
			if (why && why[0] && fy < 440) {
				uiTextWrap(40, fy + 14, SCREEN_W - 80, 15, 21, C_GREEN, why, (int) ((452 - fy) / 21));
			}
			if (navButtons(false, "Next ▶") == 1 || (now - pickedAt > 0.3 && uiOk())) {
				uiFrameEnd();
				return picked == answer ? 1 : 0;
			}
		}
		uiText(40, 534, 14, C_SOFT, "○ Leave");
		uiDustDraw();
		uiFrameEnd();
		if (uiBack()) {
			return -1;
		}
	}
}

static void finish(const char* title, int good, int total) {
	double start = uiTime();
	while (true) {
		uiPollInput();
		double now = uiTime();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(SCREEN_W / 2, 170, 28, C_YELLOW, title);
		char buf[64];
		snprintf(buf, sizeof(buf), "%d of %d right first time", good, total);
		uiTextCentered(SCREEN_W / 2, 214, 20, C_CHALK, buf);
		if (good == total) {
			uiJpCentered(SCREEN_W / 2, 330, 44, C_HANAMARU, "よくできました");
		} else {
			uiJpCentered(SCREEN_W / 2, 330, 40, C_GREEN, "もう いちど？");
			uiTextCentered(SCREEN_W / 2, 372, 16, C_SOFT, "Run it again any time from the list.");
		}
		uiTextCentered(SCREEN_W / 2, 512, 15, C_SOFT, "Tap to continue");
		uiFrameEnd();
		if (now - start > 0.5 && (IN.released || uiOk() || uiBack())) {
			return;
		}
	}
}

/* ---- a catch-up class ---- */

enum Part { P_IDEA, P_BODY, P_SHAPE, P_EX, P_SLIPS, P_CHECK };

static void runClass(int ci) {
	const struct KakuClass* k = &KAKU_CLASSES[ci];
	/* steps: idea, each body paragraph, shape, examples (2 per step), slips, checks */
	int nEx = (k->nEx + 1) / 2;
	int steps = 1 + k->nBody + (k->nShape ? 1 : 0) + nEx + (k->nSlips ? 1 : 0) + k->nChecks;
	int step = 0;
	int good = 0;
	char kicker[96];
	snprintf(kicker, sizeof(kicker), "CLASS %d OF %d", ci + 1, KAKU_CLASS_COUNT);
	while (step < steps) {
		/* locate the step */
		int s = step;
		enum Part part;
		int idx = 0;
		if (s == 0) {
			part = P_IDEA;
		} else if ((s -= 1) < k->nBody) {
			part = P_BODY;
			idx = s;
		} else if ((s -= k->nBody) < (k->nShape ? 1 : 0)) {
			part = P_SHAPE;
		} else if ((s -= (k->nShape ? 1 : 0)) < nEx) {
			part = P_EX;
			idx = s;
		} else if ((s -= nEx) < (k->nSlips ? 1 : 0)) {
			part = P_SLIPS;
		} else {
			part = P_CHECK;
			idx = s - (k->nSlips ? 1 : 0);
		}
		if (part == P_CHECK) {
			const struct KakuCheck* q = &KAKU_CLASSES[ci].checks[idx];
			int r = ask(kicker, k->title, step, steps, q->q, NULL, q->o, NULL, q->nO, q->answer, q->why);
			if (r < 0) {
				return;
			}
			good += r;
			++step;
			continue;
		}
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		frameHeader(kicker, k->title, step, steps);
		switch (part) {
		case P_IDEA:
			sheet(120, 300);
			uiJp(80, 200, 54, uiAlpha(C_YELLOW, 0.25f), "！");
			uiTextWrap(140, 190, 720, 24, 34, C_CHALK, k->idea, 6);
			break;
		case P_BODY:
			uiTextWrap(40, 140, SCREEN_W - 80, 19, 29, C_CHALK, k->body[idx], 10);
			break;
		case P_SHAPE: {
			sheet(110, 340);
			float y = 150;
			for (int i = 0; i < k->nShape && y < 440; ++i) {
				uiJp(76, y + 4, 24, C_YELLOW, k->shape[i * 2]);
				float w = uiJpW(24, k->shape[i * 2]);
				uiTextWrap(76 + fmaxf(w + 24, 150), y, SCREEN_W - 200 - fmaxf(w + 24, 150), 16, 22, C_CHALK,
				           k->shape[i * 2 + 1], 2);
				y += 44;
			}
			break;
		}
		case P_EX: {
			float y = 130;
			for (int e = idx * 2; e < k->nEx && e < idx * 2 + 2; ++e) {
				sheet(y, 150);
				uiJp(70, y + 48, 26, C_CHALK, k->ex[e * 3]);
				uiText(70, y + 82, 16, C_YELLOW, k->ex[e * 3 + 1]);
				uiTextWrap(70, y + 112, SCREEN_W - 140, 14, 20, C_SOFT, k->ex[e * 3 + 2], 2);
				y += 166;
			}
			break;
		}
		case P_SLIPS: {
			uiText(40, 128, 16, C_SOFT, "The mistake people actually make");
			float y = 146;
			for (int i = 0; i < k->nSlips && y < 420; ++i) {
				uiJp(60, y + 26, 21, C_RED, k->slips[i * 3]);
				float w = uiJpW(21, k->slips[i * 3]);
				uiSegment(56, y + 18, 64 + w, y + 18, 2.5f, uiAlpha(C_RED, 0.8f));
				uiText(80 + w, y + 24, 18, C_SOFT, "→");
				uiJp(110 + w, y + 26, 21, C_GREEN, k->slips[i * 3 + 1]);
				y += 36;
				y += uiTextWrap(60, y + 14, SCREEN_W - 120, 14, 20, C_MUTED, k->slips[i * 3 + 2], 2) + 12;
			}
			break;
		}
		default:
			break;
		}
		int nav = navButtons(step > 0, "Next ▶");
		uiText(40, 534, 14, C_SOFT, "○ Leave");
		uiFrameEnd();
		if (uiBack()) {
			return;
		}
		if (nav == -1) {
			--step;
		} else if (nav == 1 || uiOk()) {
			++step;
		}
	}
	markDone("lc:", ci, good == k->nChecks);
	finish("Class finished", good, k->nChecks);
}

/* ---- a unit: notes then drills ---- */

static void runUnit(int ui) {
	const struct KakuUnit* u = &KAKU_UNITS[ui];
	int steps = u->nNotes + u->nDrills;
	int step = 0, good = 0;
	char kicker[64];
	snprintf(kicker, sizeof(kicker), "UNIT %d OF %d", ui + 1, KAKU_UNIT_COUNT);
	while (step < steps) {
		if (step >= u->nNotes) {
			const struct KakuDrill* d = &u->drills[step - u->nNotes];
			const char* opts[4] = { d->answer, d->wrong[0], d->wrong[1], d->wrong[2] };
			const char* whys[4] = { NULL, d->wrongWhy[0], d->wrongWhy[1], d->wrongWhy[2] };
			int r = ask(kicker, u->topic, step, steps, d->q, d->ctx, opts, whys, 1 + d->nWrong, 0, d->why);
			if (r < 0) {
				return;
			}
			good += r;
			++step;
			continue;
		}
		const struct KakuNote* n = &u->notes[step];
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		frameHeader(kicker, u->topic, step, steps);
		uiText(40, 136, 22, C_CHALK, n->title);
		float y = 150 + uiTextWrap(40, 172, SCREEN_W - 80, 17, 25, C_CHALK, n->body, 7);
		for (int e = 0; e < n->nEx && y < 400; ++e) {
			sheet(y + 14, 64);
			uiJp(64, y + 48, 21, C_CHALK, n->ex[e * 2]);
			uiText(64, y + 70, 14, C_YELLOW, n->ex[e * 2 + 1]);
			y += 74;
		}
		if (step == u->nNotes - 1) {
			uiText(SCREEN_W - 420, 448, 14, C_GREEN, "Next: drills. Every wrong answer explains itself.");
		}
		int nav = navButtons(step > 0, step == u->nNotes - 1 ? "Drills ▶" : "Next ▶");
		uiText(40, 534, 14, C_SOFT, "○ Leave");
		uiFrameEnd();
		if (uiBack()) {
			return;
		}
		if (nav == -1) {
			--step;
		} else if (nav == 1 || uiOk()) {
			++step;
		}
	}
	markDone("lu:", ui, good == u->nDrills);
	finish("Unit finished", good, u->nDrills);
}

/* ---- list ---- */

void kakuLessonsRun(void) {
	uiInit();
	int tab = 0;
	float scroll[2] = { 0, 0 };
	float dragFrom = 0;
	bool dragging = false;
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiJp(36, 60, 36, C_CHALK, "じゅぎょう");
		uiText(36, 90, 15, C_SOFT, "Short classes on one idea each, and units of grammar notes with drills.");
		static const char* const tabs[2] = { "Classes", "Units" };
		for (int i = 0; i < 2; ++i) {
			float x = 600 + i * 150;
			uiRound(x, 36, 140, 40, 13, tab == i ? uiAlpha(C_YELLOW, 0.22f) : C_SHEET);
			uiTextCentered(x + 70, 63, 17, tab == i ? C_YELLOW : C_SOFT, tabs[i]);
			if (uiTapped(x, 36, 140, 40)) {
				tab = i;
			}
		}
		if (IN.buttonsPressed & (SCE_CTRL_L1 | SCE_CTRL_R1)) {
			tab = !tab;
		}
		int count = tab == 0 ? KAKU_CLASS_COUNT : KAKU_UNIT_COUNT;
		float top = 110, bottom = 512, rowH = 70;
		float maxScroll = fmaxf(0, count * rowH - (bottom - top));
		/* drag to scroll */
		if (IN.pressed && IN.y > top && IN.y < bottom) {
			dragFrom = IN.y + scroll[tab];
			dragging = true;
		}
		if (dragging && IN.down) {
			scroll[tab] = dragFrom - IN.y;
		}
		if (!IN.down) {
			dragging = false;
		}
		if (IN.buttons & SCE_CTRL_DOWN) {
			scroll[tab] += 8;
		}
		if (IN.buttons & SCE_CTRL_UP) {
			scroll[tab] -= 8;
		}
		scroll[tab] = fmaxf(0, fminf(maxScroll, scroll[tab]));
		vita2d_set_clip_rectangle(0, (int) top, SCREEN_W, (int) bottom);
		vita2d_enable_clipping();
		int pick = -1;
		for (int i = 0; i < count; ++i) {
			float y = top + i * rowH - scroll[tab];
			if (y + rowH < top || y > bottom) {
				continue;
			}
			bool fin = done(tab == 0 ? "lc:" : "lu:", i);
			uiRound(36, y + 4, SCREEN_W - 72, rowH - 10, 14, C_SHEET);
			uiRoundOutline(36, y + 4, SCREEN_W - 72, rowH - 10, 14, 2, fin ? uiAlpha(C_GREEN, 0.6f) : C_LINE_SOFT);
			char num[8];
			snprintf(num, sizeof(num), "%d", i + 1);
			uiTextCentered(72, y + 44, 20, fin ? C_GREEN : C_SOFT, num);
			const char* title = tab == 0 ? KAKU_CLASSES[i].title : KAKU_UNITS[i].topic;
			uiTextWrap(110, y + 42, SCREEN_W - 260, 17, 22, C_CHALK, title, 1);
			if (tab == 1 && KAKU_UNITS[i].kanji[0]) {
				char k[32];
				/* first few kanji of the unit as a preview */
				uint32_t cps[8];
				int nk = lingoUtf8Decode(KAKU_UNITS[i].kanji, cps, 4);
				lingoUtf8FromCodepoints(cps, nk, k, sizeof(k));
				uiJp(SCREEN_W - 150, y + 46, 18, uiAlpha(C_YELLOW, 0.7f), k);
			}
			if (fin) {
				uiText(SCREEN_W - 70, y + 44, 20, C_GREEN, "✓");
			}
			if (IN.released && fabsf(IN.y - IN.sy) < 12 && IN.y > y + 4 && IN.y < y + rowH - 6 && IN.y > top &&
			    IN.y < bottom) {
				pick = i;
			}
		}
		vita2d_disable_clipping();
		uiText(36, 534, 14, C_SOFT, "○ Back    L / R switch tabs");
		uiFrameEnd();
		if (uiBack()) {
			return;
		}
		if (pick >= 0) {
			if (tab == 0) {
				runClass(pick);
			} else {
				runUnit(pick);
			}
		}
	}
}
