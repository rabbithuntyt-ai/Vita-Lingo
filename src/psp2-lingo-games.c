/* Lingo: the starter-games shelf. Each card is original chalk art: the title
 * hand-lettered, a colour, and a small generic doodle. No cover art. */
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>

enum Doodle { D_GEMS, D_GEM, D_FLAME_LEAF, D_STARS, D_SUN_MOON, D_SWORD };

struct Game {
	const char* jp;      /* title as written in Japan */
	const char* en;      /* what English speakers call it */
	const char* system;
	int level;           /* 1 start here, 2 next, 3 stretch */
	bool panel;          /* the study panel reads its text */
	const char* script;  /* how it is written */
	const char* why;
	enum Doodle doodle;
	uint32_t a, b;       /* chalk colours */
};

static const struct Game GAMES[] = {
	{ "ルビー・サファイア", "Pokémon Ruby & Sapphire", "Game Boy Advance", 1, true, "Kana only",
	  "The best place to start. Every line is hiragana and katakana with spaces between phrases, and the study panel "
	  "reads all of it. Menus, moves and items repeat constantly, so words stick fast.",
	  D_GEMS, C_RED, C_BLUE },
	{ "エメラルド", "Pokémon Emerald", "Game Boy Advance", 1, true, "Kana only",
	  "The same region and the same kana-only text as Ruby and Sapphire, with more story. Fully supported by the "
	  "study panel.",
	  D_GEM, C_GREEN, C_GREEN },
	{ "ファイアレッド・リーフグリーン", "Pokémon FireRed & LeafGreen", "Game Boy Advance", 1, true, "Kana only",
	  "If you played the original in English you will recognise every scene, which makes guessing meanings much "
	  "easier. Fully supported by the study panel.",
	  D_FLAME_LEAF, C_RED, C_GREEN },
	{ "ポケットモンスター 金・銀", "Pokémon Gold & Silver", "Game Boy Color", 2, false, "Kana only",
	  "Kana only, like every Pokémon game of its era. The study panel does not read Game Boy Color games, so this is "
	  "pure reading practice. Star words you look up elsewhere and review them in Kaku.",
	  D_SUN_MOON, C_YELLOW, C_SOFT },
	{ "MOTHER 1+2", "Mother 1+2 (Japan only)", "Game Boy Advance", 2, false, "Mostly kana",
	  "The series is written almost entirely in kana. Warm, funny everyday dialogue. The panel does not read it, so "
	  "go slowly and use it as reading practice.",
	  D_STARS, C_PINK, C_YELLOW },
	{ "MOTHER 3", "Mother 3 (Japan only)", "Game Boy Advance", 3, false, "Mostly kana",
	  "Mostly kana like the rest of the series, but a longer, heavier story. A great goal once Pokémon feels "
	  "comfortable.",
	  D_STARS, C_YELLOW, C_GREEN },
	{ "聖剣伝説", "Seiken Densetsu (Final Fantasy Adventure)", "Game Boy", 3, false, "Kana with some kanji",
	  "Picked out by Time Extension as a good stepping stone for reading retro games in Japanese. Short adventure, "
	  "classic RPG vocabulary.",
	  D_SWORD, C_BLUE, C_CHALK },
};
#define NGAMES ((int) (sizeof(GAMES) / sizeof(GAMES[0])))

static void star(float cx, float cy, float r, uint32_t c) {
	struct KakuPt p[11];
	for (int i = 0; i <= 10; ++i) {
		float a = -M_PI / 2 + i * M_PI / 5;
		float rr = (i % 2) ? r * 0.45f : r;
		p[i] = (struct KakuPt) { cx + rr * cosf(a), cy + rr * sinf(a) };
	}
	uiThickLine(p, 11, 1, 0, 0, 3, c);
}

static void gem(float cx, float cy, float s, uint32_t c) {
	struct KakuPt p[] = {
		{ cx - s, cy - s * 0.35f }, { cx - s * 0.55f, cy - s * 0.8f }, { cx + s * 0.55f, cy - s * 0.8f },
		{ cx + s, cy - s * 0.35f }, { cx, cy + s }, { cx - s, cy - s * 0.35f }, { cx + s, cy - s * 0.35f },
	};
	uiThickLine(p, 7, 1, 0, 0, 4, c);
	uiSegment(cx - s * 0.55f, cy - s * 0.8f, cx - s * 0.3f, cy - s * 0.35f, 3, c);
	uiSegment(cx + s * 0.55f, cy - s * 0.8f, cx + s * 0.3f, cy - s * 0.35f, 3, c);
	uiSegment(cx - s * 0.3f, cy - s * 0.35f, cx, cy + s, 3, uiAlpha(c, 0.6f));
	uiSegment(cx + s * 0.3f, cy - s * 0.35f, cx, cy + s, 3, uiAlpha(c, 0.6f));
}

static void drawDoodle(const struct Game* g, float cx, float cy, float t) {
	float bob = sinf(t * 2) * 3;
	switch (g->doodle) {
	case D_GEMS:
		gem(cx - 36, cy + bob, 30, g->a);
		gem(cx + 36, cy - bob, 30, g->b);
		break;
	case D_GEM:
		gem(cx, cy + bob, 40, g->a);
		break;
	case D_FLAME_LEAF: {
		struct KakuPt fl[24];
		for (int i = 0; i < 24; ++i) {
			float u = i / 23.0f;
			float a = u * 2 * M_PI;
			float r = 26 * (0.6f + 0.4f * sinf(a / 2)) * (1 + 0.25f * sinf(a * 3 + t * 6));
			fl[i] = (struct KakuPt) { cx - 40 + r * sinf(a) * 0.8f, cy + 8 - r * (1 - cosf(a)) * 0.9f + 20 };
		}
		uiThickLine(fl, 24, 1, 0, bob, 4, g->a);
		struct KakuPt lf[] = { { cx + 20, cy + 26 }, { cx + 34, cy - 12 }, { cx + 62, cy - 26 }, { cx + 60, cy + 6 },
			                   { cx + 20, cy + 26 } };
		uiThickLine(lf, 5, 1, 0, -bob, 4, g->b);
		uiSegment(cx + 20, cy + 26 - bob, cx + 56, cy - 18 - bob, 2.5f, g->b);
		break;
	}
	case D_STARS:
		star(cx - 30, cy + bob, 22, g->a);
		star(cx + 28, cy - 14 - bob, 14, g->b);
		star(cx + 34, cy + 24 + bob, 9, uiAlpha(C_CHALK, 0.7f));
		break;
	case D_SUN_MOON:
		uiCircle(cx - 30, cy + bob, 20, uiAlpha(g->a, 0.85f));
		for (int i = 0; i < 8; ++i) {
			float a = i * M_PI / 4 + t * 0.5f;
			uiSegment(cx - 30 + cosf(a) * 27, cy + bob + sinf(a) * 27, cx - 30 + cosf(a) * 36, cy + bob + sinf(a) * 36, 3, g->a);
		}
		uiCircle(cx + 34, cy - bob, 20, uiAlpha(g->b, 0.85f));
		uiCircle(cx + 44, cy - bob - 6, 17, C_SHEET);
		break;
	case D_SWORD:
		uiSegment(cx - 34, cy + 34 + bob, cx + 30, cy - 30 + bob, 6, g->b);
		uiSegment(cx - 40, cy + 14 + bob, cx - 14, cy + 40 + bob, 6, g->a);
		uiCircle(cx - 42, cy + 42 + bob, 5, g->a);
		break;
	}
}

static void drawCard(const struct Game* g, float x, float y, float w, float h, float t, bool focus) {
	uiRound(x + 5, y + 8, w, h, 22, RGBA(0, 0, 0, 0x55));
	uiRound(x, y, w, h, 22, C_SHEET);
	uiRoundOutline(x, y, w, h, 22, focus ? 3 : 2, focus ? g->a : C_LINE);
	/* colour band */
	uiRound(x + 14, y + 14, w - 28, 120, 16, uiAlpha(g->a, 0.12f));
	drawDoodle(g, x + w / 2, y + 74, t);
	/* title */
	float px = 40;
	while (px > 20 && uiJpW(px, g->jp) > w - 40) {
		px -= 2;
	}
	uiJpCentered(x + w / 2, y + 184, px, C_CHALK, g->jp);
	uiTextCentered(x + w / 2, y + 214, 15, C_SOFT, g->en);
	/* chips */
	float cx = x + 20;
	const char* chips[3] = { g->system, g->script, g->panel ? "Study panel reads it" : NULL };
	uint32_t cols[3] = { C_SOFT, C_YELLOW, C_GREEN };
	float cy = y + 234;
	for (int i = 0; i < 3; ++i) {
		if (!chips[i]) {
			continue;
		}
		float cw = uiTextW(13, chips[i]) + 20;
		if (cx + cw > x + w - 20) {
			cx = x + 20;
			cy += 32;
		}
		uiRound(cx, cy, cw, 26, 9, uiAlpha(cols[i], 0.16f));
		uiText(cx + 10, cy + 18, 13, cols[i], chips[i]);
		cx += cw + 8;
	}
	uiTextWrap(x + 22, cy + 56, w - 44, 15, 22, C_CHALK, g->why, 6);
	/* level */
	static const char* const lv[4] = { "", "Start here", "Next step", "Stretch goal" };
	for (int i = 0; i < 3; ++i) {
		star(x + w - 92 + i * 26, y + h - 26, 9, i < g->level ? C_YELLOW : C_LINE_SOFT);
	}
	uiText(x + 22, y + h - 20, 15, g->level == 1 ? C_YELLOW : C_SOFT, lv[g->level]);
}

void lingoGamesRun(void) {
	uiInit();
	int cur = 0;
	float pos = 0;     /* animated card position */
	float drag = 0;
	double last = uiTime(), start = last;
	while (true) {
		uiPollInput();
		double now = uiTime();
		float dt = (float) (now - last);
		last = now;
		float t = (float) (now - start);
		if (IN.buttonsPressed & (SCE_CTRL_RIGHT | SCE_CTRL_R1)) {
			cur = cur < NGAMES - 1 ? cur + 1 : cur;
		}
		if (IN.buttonsPressed & (SCE_CTRL_LEFT | SCE_CTRL_L1)) {
			cur = cur > 0 ? cur - 1 : cur;
		}
		if (IN.down && IN.sy > 70 && IN.sy < 500) {
			drag = IN.x - IN.sx;
		}
		if (IN.released && fabsf(drag) > 60) {
			cur += drag < 0 ? 1 : -1;
			if (cur < 0) {
				cur = 0;
			}
			if (cur >= NGAMES) {
				cur = NGAMES - 1;
			}
		}
		if (!IN.down) {
			drag *= 0.6f;
		}
		pos += (cur - pos) * fminf(1, dt * 10);

		uiFrameBegin();
		uiChalkBackground();
		uiJp(36, 56, 32, C_CHALK, "はじめの ゲーム");
		uiText(36, 86, 16, C_SOFT, "Starter games, easiest first. Bring your own cartridge dumps.");
		float cw = 440, ch = 404;
		for (int i = 0; i < NGAMES; ++i) {
			float off = (i - pos) * (cw + 30) + drag;
			float x = SCREEN_W / 2 - cw / 2 + off;
			if (x > SCREEN_W || x + cw < 0) {
				continue;
			}
			drawCard(&GAMES[i], x, 104, cw, ch, t, i == cur);
			if (i != cur && uiTapped(x, 104, cw, ch) && fabsf(drag) < 10) {
				cur = i;
			}
		}
		/* dots */
		for (int i = 0; i < NGAMES; ++i) {
			uiCircle(SCREEN_W / 2 - (NGAMES - 1) * 9 + i * 18, 538 - 14, i == cur ? 5 : 3.5f, i == cur ? C_YELLOW : C_LINE);
		}
		uiText(36, 534, 14, C_SOFT, "○ Back    ◀ ▶ Browse");
		uiFrameEnd();
		if (uiBack()) {
			return;
		}
	}
}
