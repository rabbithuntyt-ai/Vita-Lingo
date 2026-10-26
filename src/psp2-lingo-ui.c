/* Lingo / Kaku shared UI: chalkboard drawing, fonts, input, storage,
 * the title screen and the in-game study panel. */
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <vita2d.h>

#define DATA_DIR "ux0:data/lingo"
#define DICT_PATH "app0:lingo/lingo.dic"
#define FONT_PATH "app0:lingo/klee.ttf"

static vita2d_pgf* sPgf;
static vita2d_font* sKlee;
static bool sReady;

uint32_t uiAlpha(uint32_t c, float a) {
	if (a < 0) {
		a = 0;
	}
	if (a > 1) {
		a = 1;
	}
	uint32_t al = (uint32_t) ((c >> 24) * a);
	return (c & 0x00FFFFFF) | (al << 24);
}

double uiTime(void) {
	return sceKernelGetProcessTimeWide() / 1000000.0;
}

void uiInit(void) {
	if (sReady) {
		return;
	}
	sPgf = vita2d_load_default_pgf();
	sKlee = vita2d_load_font_file(FONT_PATH);
	sceIoMkdir(DATA_DIR, 0777);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	deckLoad();
	sReady = true;
}

void uiDeinit(void) {
	if (!sReady) {
		return;
	}
	storeSave();
	if (sKlee) {
		vita2d_free_font(sKlee);
	}
	vita2d_free_pgf(sPgf);
	sReady = false;
}

void uiFrameBegin(void) {
	vita2d_start_drawing();
	vita2d_set_clear_color(C_BOARD);
	vita2d_clear_screen();
}

void uiFrameEnd(void) {
	vita2d_end_drawing();
	vita2d_common_dialog_update();
	vita2d_wait_rendering_done();
	vita2d_swap_buffers();
}

/* ---- shapes ---- */

void uiRect(float x, float y, float w, float h, uint32_t c) {
	vita2d_draw_rectangle(x, y, w, h, c);
}

void uiCircle(float x, float y, float r, uint32_t c) {
	vita2d_draw_fill_circle(x, y, r, c);
}

void uiRound(float x, float y, float w, float h, float r, uint32_t c) {
	if (r * 2 > h) {
		r = h / 2;
	}
	if (r * 2 > w) {
		r = w / 2;
	}
	/* a cross of rectangles plus four quarter circles, drawn as one strip so alpha doesn't overlap */
	int seg = 6;
	int n = 4 * (seg + 1);
	vita2d_color_vertex* v = vita2d_pool_memalign((n + 2) * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
	if (!v) {
		return;
	}
	float cx[4] = { x + w - r, x + r, x + r, x + w - r };
	float cy[4] = { y + r, y + r, y + h - r, y + h - r };
	int k = 0;
	/* fan around the centre, corners top-right, top-left, bottom-left, bottom-right */
	v[k++] = (vita2d_color_vertex) { x + w / 2, y + h / 2, 0.5f, c };
	for (int q = 0; q < 4; ++q) {
		for (int i = 0; i <= seg; ++i) {
			float a = (q == 0 ? 0 : q == 1 ? -M_PI / 2 : q == 2 ? -M_PI : -3 * M_PI / 2) - (M_PI / 2) * i / seg;
			v[k++] = (vita2d_color_vertex) { cx[q] + r * cosf(a), cy[q] + r * sinf(a), 0.5f, c };
		}
	}
	v[k++] = v[1];
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, k);
}

void uiRoundOutline(float x, float y, float w, float h, float r, float t, uint32_t c) {
	if (r * 2 > h) {
		r = h / 2;
	}
	int seg = 6;
	int n = 4 * (seg + 1) + 1;
	vita2d_color_vertex* v = vita2d_pool_memalign(n * 2 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
	if (!v) {
		return;
	}
	float cx[4] = { x + w - r, x + r, x + r, x + w - r };
	float cy[4] = { y + r, y + r, y + h - r, y + h - r };
	int k = 0;
	for (int q = 0; q < 4; ++q) {
		for (int i = 0; i <= seg; ++i) {
			float a = (q == 0 ? 0 : q == 1 ? -M_PI / 2 : q == 2 ? -M_PI : -3 * M_PI / 2) - (M_PI / 2) * i / seg;
			float ca = cosf(a), sa = sinf(a);
			v[k++] = (vita2d_color_vertex) { cx[q] + r * ca, cy[q] + r * sa, 0.5f, c };
			v[k++] = (vita2d_color_vertex) { cx[q] + (r - t) * ca, cy[q] + (r - t) * sa, 0.5f, c };
		}
	}
	v[k++] = v[0];
	v[k++] = v[1];
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, k);
}

void uiSegment(float x0, float y0, float x1, float y1, float width, uint32_t c) {
	struct KakuPt p[2] = { { x0, y0 }, { x1, y1 } };
	uiThickLine(p, 2, 1, 0, 0, width, c);
}

void uiThickLine(const struct KakuPt* p, int n, float scale, float ox, float oy, float width, uint32_t c) {
	if (n < 1) {
		return;
	}
	float hw = width / 2;
	if (n >= 2) {
		vita2d_color_vertex* v = vita2d_pool_memalign(n * 2 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
		if (!v) {
			return;
		}
		for (int i = 0; i < n; ++i) {
			int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
			float dx = (p[b].x - p[a].x) * scale, dy = (p[b].y - p[a].y) * scale;
			float L = sqrtf(dx * dx + dy * dy);
			float nx = L > 0 ? -dy / L : 0, ny = L > 0 ? dx / L : 0;
			float x = ox + p[i].x * scale, y = oy + p[i].y * scale;
			v[i * 2] = (vita2d_color_vertex) { x + nx * hw, y + ny * hw, 0.5f, c };
			v[i * 2 + 1] = (vita2d_color_vertex) { x - nx * hw, y - ny * hw, 0.5f, c };
		}
		vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, n * 2);
	}
	/* round caps */
	uiCircle(ox + p[0].x * scale, oy + p[0].y * scale, hw, c);
	uiCircle(ox + p[n - 1].x * scale, oy + p[n - 1].y * scale, hw, c);
}

/* A board with a few faint smudges and a chalk tray, the house style. */
void uiChalkBackground(void) {
	uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
	static const float sm[][4] = {
		{ 140, 90, 120, 0.05f }, { 720, 140, 180, 0.04f }, { 420, 420, 160, 0.035f }, { 860, 460, 110, 0.05f },
	};
	for (size_t i = 0; i < sizeof(sm) / sizeof(sm[0]); ++i) {
		uiCircle(sm[i][0], sm[i][1], sm[i][2], uiAlpha(C_CHALK, sm[i][3]));
	}
	uiRect(0, SCREEN_H - 10, SCREEN_W, 10, RGBA(0x5a, 0x3e, 0x2b, 0xff)); /* wooden tray */
	uiRect(0, SCREEN_H - 10, SCREEN_W, 2, RGBA(0x7d, 0x58, 0x3c, 0xff));
}

/* ---- chalk dust ---- */

struct Dust {
	float x, y, vx, vy, life, max;
	uint32_t c;
};
static struct Dust sDust[160];

void uiDust(float x, float y, uint32_t c, int count) {
	static unsigned seed = 7;
	for (int k = 0; k < count; ++k) {
		for (size_t i = 0; i < sizeof(sDust) / sizeof(sDust[0]); ++i) {
			if (sDust[i].life <= 0) {
				seed = seed * 1103515245 + 12345;
				float a = (seed >> 8) % 6283 / 1000.0f;
				float s = 40 + (seed >> 4) % 120;
				sDust[i] = (struct Dust) { x, y, cosf(a) * s, sinf(a) * s - 40, 0.6f + (seed % 40) / 100.0f, 0, c };
				sDust[i].max = sDust[i].life;
				break;
			}
		}
	}
}

void uiDustUpdate(float dt) {
	for (size_t i = 0; i < sizeof(sDust) / sizeof(sDust[0]); ++i) {
		struct Dust* d = &sDust[i];
		if (d->life <= 0) {
			continue;
		}
		d->life -= dt;
		d->x += d->vx * dt;
		d->y += d->vy * dt;
		d->vy += 160 * dt;
		d->vx *= 0.97f;
	}
}

void uiDustDraw(void) {
	for (size_t i = 0; i < sizeof(sDust) / sizeof(sDust[0]); ++i) {
		struct Dust* d = &sDust[i];
		if (d->life > 0) {
			uiCircle(d->x, d->y, 1.5f + 2 * d->life / d->max, uiAlpha(d->c, d->life / d->max));
		}
	}
}

/* ---- text ---- */

static float pgfScale(float px) {
	return px / 17.0f;
}

int uiText(float x, float y, float px, uint32_t c, const char* s) {
	return vita2d_pgf_draw_text(sPgf, (int) x, (int) y, c, pgfScale(px), s);
}

int uiTextW(float px, const char* s) {
	return vita2d_pgf_text_width(sPgf, pgfScale(px), s);
}

void uiTextCentered(float cx, float y, float px, uint32_t c, const char* s) {
	uiText(cx - uiTextW(px, s) / 2.0f, y, px, c, s);
}

int uiJp(float x, float y, float px, uint32_t c, const char* s) {
	if (sKlee) {
		return vita2d_font_draw_text(sKlee, (int) x, (int) y, c, (unsigned) (px * 1.25f), s);
	}
	return uiText(x, y, px, c, s);
}

int uiJpW(float px, const char* s) {
	if (sKlee) {
		return vita2d_font_text_width(sKlee, (unsigned) (px * 1.25f), s);
	}
	return uiTextW(px, s);
}

void uiJpCentered(float cx, float y, float px, uint32_t c, const char* s) {
	uiJp(cx - uiJpW(px, s) / 2.0f, y, px, c, s);
}

static int utf8Len(unsigned char b) {
	return b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
}

int uiTextWrap(float x, float y, float w, float px, float lineH, uint32_t c, const char* s, int maxLines) {
	char line[512];
	size_t ll = 0;
	int lines = 0;
	const char* p = s;
	while (*p && lines < maxLines) {
		/* next unit: an ASCII word with its trailing space, or one wide character */
		const char* q = p;
		if ((unsigned char) *q < 0x80) {
			while (*q && (unsigned char) *q < 0x80 && *q != ' ' && *q != '\n') {
				++q;
			}
			while (*q == ' ') {
				++q;
			}
			if (q == p) {
				++q;
			}
		} else {
			q += utf8Len((unsigned char) *q);
		}
		size_t ul = q - p;
		bool newline = *p == '\n';
		char trial[512];
		if (!newline && ll + ul < sizeof(trial)) {
			memcpy(trial, line, ll);
			memcpy(trial + ll, p, ul);
			trial[ll + ul] = 0;
		}
		if (newline || (ll && uiTextW(px, trial) > w) || ll + ul >= sizeof(line) - 1) {
			line[ll] = 0;
			bool last = lines == maxLines - 1 && !newline;
			if (last) {
				/* ellipsize */
				while (ll > 0 && uiTextW(px, line) + uiTextW(px, "…") > w) {
					--ll;
					while (ll > 0 && ((unsigned char) line[ll] & 0xC0) == 0x80) {
						--ll;
					}
					line[ll] = 0;
				}
				strcat(line, "…");
			}
			uiText(x, y + lines * lineH, px, c, line);
			++lines;
			ll = 0;
			if (newline) {
				p = q;
				continue;
			}
			if (last) {
				return lines * lineH;
			}
			while (*p == ' ') {
				++p;
			}
			continue;
		}
		memcpy(line + ll, p, ul);
		ll += ul;
		p = q;
	}
	if (ll && lines < maxLines) {
		line[ll] = 0;
		uiText(x, y + lines * lineH, px, c, line);
		++lines;
	}
	return lines * lineH;
}

/* ---- input ---- */

struct UiInput IN;
static uint32_t sPrevButtons;
static bool sPrevDown;

void uiPollInput(void) {
	SceTouchData t;
	memset(&t, 0, sizeof(t));
	sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
	bool down = t.reportNum > 0;
	IN.pressed = down && !sPrevDown;
	IN.released = !down && sPrevDown;
	if (down) {
		IN.x = t.report[0].x / 2.0f;
		IN.y = t.report[0].y / 2.0f;
		if (IN.pressed) {
			IN.sx = IN.x;
			IN.sy = IN.y;
		}
	}
	IN.down = down;
	sPrevDown = down;
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	IN.buttonsPressed = pad.buttons & ~sPrevButtons;
	IN.buttons = pad.buttons;
	sPrevButtons = pad.buttons;
}

static bool inside(float px, float py, float x, float y, float w, float h) {
	return px >= x && px < x + w && py >= y && py < y + h;
}

bool uiTapped(float x, float y, float w, float h) {
	return IN.released && inside(IN.x, IN.y, x, y, w, h) && inside(IN.sx, IN.sy, x, y, w, h);
}

bool uiHeld(float x, float y, float w, float h) {
	return IN.down && inside(IN.sx, IN.sy, x, y, w, h) && inside(IN.x, IN.y, x, y, w, h);
}

bool uiBack(void) {
	return IN.buttonsPressed & SCE_CTRL_CIRCLE;
}

bool uiOk(void) {
	return IN.buttonsPressed & SCE_CTRL_CROSS;
}

bool uiButton(float x, float y, float w, float h, const char* label, const char* jp, uint32_t accent, bool focus) {
	bool held = uiHeld(x, y, w, h);
	float lift = held ? 2 : 0;
	uiRound(x + 3, y + 5, w, h, 14, RGBA(0, 0, 0, 0x50));
	uiRound(x, y + lift, w, h, 14, held ? C_SHEET_HI : C_SHEET);
	uiRoundOutline(x, y + lift, w, h, 14, focus ? 3 : 2, focus ? C_YELLOW : uiAlpha(accent, 0.75f));
	float tx = x + 18;
	if (jp && *jp) {
		uiJp(x + 18, y + lift + h / 2 + 15, 30, accent, jp);
		tx = x + 18 + uiJpW(30, jp) + 14;
	}
	if (label) {
		uiText(tx, y + lift + h / 2 + 7, 19, C_CHALK, label);
	}
	return uiTapped(x, y, w, h) || (focus && uiOk());
}

/* ---- dates ---- */

uint32_t uiToday(void) {
	SceDateTime t;
	sceRtcGetCurrentClockLocalTime(&t);
	int y = t.year, m = t.month, d = t.day;
	/* days from civil (Howard Hinnant) */
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned) (y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (uint32_t) (era * 146097 + (int) doe - 719468 + 100000);
}

/* ---- review store ---- */

struct StoreEntry {
	char key[60];
	struct KakuCard card;
};
static struct StoreEntry* sStore;
static int sStoreN, sStoreCap;
static bool sStoreLoaded, sStoreDirty;

static void storeLoad(void) {
	if (sStoreLoaded) {
		return;
	}
	sStoreLoaded = true;
	FILE* f = fopen(DATA_DIR "/review.bin", "rb");
	if (!f) {
		return;
	}
	char magic[4];
	int32_t n = 0;
	if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "LKS1", 4) && fread(&n, 4, 1, f) == 1 && n > 0 && n < 100000) {
		sStore = calloc(n + 64, sizeof(*sStore));
		sStoreCap = n + 64;
		sStoreN = (int) fread(sStore, sizeof(*sStore), n, f);
	}
	fclose(f);
}

void storeSave(void) {
	if (!sStoreDirty) {
		return;
	}
	FILE* f = fopen(DATA_DIR "/review.tmp", "wb");
	if (!f) {
		return;
	}
	int32_t n = sStoreN;
	fwrite("LKS1", 1, 4, f);
	fwrite(&n, 4, 1, f);
	fwrite(sStore, sizeof(*sStore), sStoreN, f);
	fclose(f);
	sceIoRemove(DATA_DIR "/review.bin");
	sceIoRename(DATA_DIR "/review.tmp", DATA_DIR "/review.bin");
	sStoreDirty = false;
}

struct KakuCard* storeGet(const char* key, bool create) {
	storeLoad();
	for (int i = 0; i < sStoreN; ++i) {
		if (!strncmp(sStore[i].key, key, sizeof(sStore[i].key) - 1)) {
			if (create) {
				sStoreDirty = true;
			}
			return &sStore[i].card;
		}
	}
	if (!create) {
		return NULL;
	}
	if (sStoreN == sStoreCap) {
		int cap = sStoreCap ? sStoreCap * 2 : 256;
		struct StoreEntry* s = realloc(sStore, cap * sizeof(*s));
		if (!s) {
			return NULL;
		}
		sStore = s;
		sStoreCap = cap;
	}
	struct StoreEntry* e = &sStore[sStoreN++];
	memset(e, 0, sizeof(*e));
	snprintf(e->key, sizeof(e->key), "%s", key);
	kakuCardInit(&e->card, uiToday());
	sStoreDirty = true;
	return &e->card;
}

int storeDueCount(const char* prefix, uint32_t today) {
	storeLoad();
	int n = 0;
	size_t pl = strlen(prefix);
	for (int i = 0; i < sStoreN; ++i) {
		if (!strncmp(sStore[i].key, prefix, pl) && sStore[i].card.due <= today) {
			++n;
		}
	}
	return n;
}

/* ---- saved words ---- */

static struct SavedWord* sDeck;
static int sDeckN, sDeckCap;

static void clean(char* s) {
	for (; *s; ++s) {
		if (*s == '\t' || *s == '\n' || *s == '\r') {
			*s = ' ';
		}
	}
}

void deckLoad(void) {
	sDeckN = 0;
	FILE* f = fopen(DATA_DIR "/saved-words.tsv", "r");
	if (!f) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') {
			continue;
		}
		line[strcspn(line, "\r\n")] = 0;
		struct SavedWord w;
		memset(&w, 0, sizeof(w));
		char* fields[4] = { 0 };
		int nf = 0;
		char* p = line;
		fields[nf++] = p;
		while (*p && nf < 4) {
			if (*p == '\t') {
				*p = 0;
				fields[nf++] = p + 1;
			}
			++p;
		}
		if (nf < 3) {
			continue;
		}
		snprintf(w.head, sizeof(w.head), "%s", fields[0]);
		snprintf(w.reading, sizeof(w.reading), "%s", fields[1]);
		snprintf(w.gloss, sizeof(w.gloss), "%s", fields[2]);
		if (nf > 3) {
			snprintf(w.context, sizeof(w.context), "%s", fields[3]);
		}
		if (sDeckN == sDeckCap) {
			int cap = sDeckCap ? sDeckCap * 2 : 64;
			struct SavedWord* d = realloc(sDeck, cap * sizeof(*d));
			if (!d) {
				break;
			}
			sDeck = d;
			sDeckCap = cap;
		}
		sDeck[sDeckN++] = w;
	}
	fclose(f);
}

int deckCount(void) {
	return sDeckN;
}

const struct SavedWord* deckGet(int i) {
	return i >= 0 && i < sDeckN ? &sDeck[i] : NULL;
}

bool deckHas(const char* head, const char* reading) {
	for (int i = 0; i < sDeckN; ++i) {
		if (!strcmp(sDeck[i].head, head) && !strcmp(sDeck[i].reading, reading)) {
			return true;
		}
	}
	return false;
}

bool deckAdd(const struct SavedWord* src) {
	if (deckHas(src->head, src->reading)) {
		return false;
	}
	struct SavedWord w = *src;
	clean(w.head);
	clean(w.reading);
	clean(w.gloss);
	clean(w.context);
	bool fresh = false;
	FILE* t = fopen(DATA_DIR "/saved-words.tsv", "r");
	if (t) {
		fclose(t);
	} else {
		fresh = true;
	}
	FILE* f = fopen(DATA_DIR "/saved-words.tsv", "a");
	if (!f) {
		return false;
	}
	if (fresh) {
		fprintf(f, "#separator:tab\n#columns:Word\tReading\tMeaning\tFrom the game\n");
	}
	fprintf(f, "%s\t%s\t%s\t%s\n", w.head, w.reading, w.gloss, w.context);
	fclose(f);
	if (sDeckN == sDeckCap) {
		int cap = sDeckCap ? sDeckCap * 2 : 64;
		struct SavedWord* d = realloc(sDeck, cap * sizeof(*d));
		if (!d) {
			return false;
		}
		sDeck = d;
		sDeckCap = cap;
	}
	sDeck[sDeckN++] = w;
	char key[60];
	snprintf(key, sizeof(key), "w:%s|%s", w.head, w.reading);
	storeGet(key, true);
	storeSave();
	return true;
}

/* ---- title screen ---- */

static void drawAbout(void) {
	uiRound(110, 70, 740, 400, 18, C_SHADE);
	uiRound(120, 60, 720, 400, 18, C_SHEET);
	uiRoundOutline(120, 60, 720, 400, 18, 2, C_LINE);
	uiText(150, 105, 24, C_YELLOW, "About Lingo");
	static const char* const lines[] = {
		"Built on mGBA by endrift and contributors (MPL 2.0).",
		"Dictionary: JMdict, Electronic Dictionary Research and Development Group, CC BY-SA 4.0.",
		"Kanji meanings and readings: KANJIDIC, EDRDG, CC BY-SA 4.0.",
		"Stroke order: KanjiVG by Ulrich Apel, CC BY-SA 3.0.",
		"Handwriting font: Klee One by Fontworks, SIL Open Font License.",
		"Stroke matching follows the approach of Hanzi Writer (MIT).",
		"Kaku practice and Lingo by Troy Stokoe.",
		"",
		"Bring your own game cartridge dumps. No games are included.",
	};
	float y = 145;
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
		y += uiTextWrap(150, y, 660, 16, 24, i == 8 ? C_SOFT : C_CHALK, lines[i], 2);
		if (!lines[i][0]) {
			y += 10;
		}
	}
	uiTextCentered(480, 440, 16, C_SOFT, "Tap anywhere to close");
}

void lingoLauncherRun(void) {
	uiInit();
	double last = uiTime();
	double start = last;
	int focus = 0;
	bool about = false;
	while (true) {
		uiPollInput();
		double now = uiTime();
		float dt = (float) (now - last);
		last = now;
		float t = (float) (now - start);
		uiDustUpdate(dt);
		uint32_t today = uiToday();
		int dueWords = storeDueCount("w:", today);
		int dueKaku = storeDueCount("k:", today) + storeDueCount("j:", today);

		uiFrameBegin();
		uiChalkBackground();
		/* title, written on like chalk */
		float reveal = t * 1.6f;
		uiJp(80, 140, 74, uiAlpha(C_CHALK, reveal), "ことば");
		uiText(82, 196, 46, uiAlpha(C_YELLOW, reveal - 0.3f), "LINGO");
		uiText(84, 232, 18, uiAlpha(C_SOFT, reveal - 0.6f), "Play games in Japanese. Learn every word you meet.");
		/* a little doodle: a Game Boy shape in chalk */
		float bx = 640, by = 70;
		uiRoundOutline(bx, by, 150, 200, 14, 3, uiAlpha(C_CHALK, 0.55f));
		uiRoundOutline(bx + 18, by + 20, 114, 80, 6, 3, uiAlpha(C_CHALK, 0.45f));
		uiJpCentered(bx + 75, by + 72, 30, uiAlpha(C_GREEN, 0.8f + 0.2f * sinf(t * 2)), "あ");
		uiSegment(bx + 30, by + 140, bx + 60, by + 140, 4, uiAlpha(C_CHALK, 0.5f));
		uiSegment(bx + 45, by + 125, bx + 45, by + 155, 4, uiAlpha(C_CHALK, 0.5f));
		uiCircle(bx + 110, by + 132, 8, uiAlpha(C_RED, 0.7f));
		uiCircle(bx + 126, by + 148, 8, uiAlpha(C_YELLOW, 0.7f));

		if (IN.buttonsPressed & SCE_CTRL_DOWN) {
			focus = (focus + 1) % 3;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			focus = (focus + 2) % 3;
		}
		int choice = -1;
		if (!about) {
			if (uiButton(80, 280, 420, 66, "Play a game", "遊", C_YELLOW, focus == 0)) {
				choice = 0;
			}
			char buf[64];
			if (dueKaku > 0) {
				snprintf(buf, sizeof(buf), "Kaku practice  ·  %d due", dueKaku);
			} else {
				snprintf(buf, sizeof(buf), "Kaku practice");
			}
			if (uiButton(80, 360, 420, 66, buf, "書", C_GREEN, focus == 1)) {
				choice = 1;
			}
			if (dueWords > 0) {
				snprintf(buf, sizeof(buf), "Saved words  ·  %d due", dueWords);
			} else {
				snprintf(buf, sizeof(buf), "Saved words  ·  %d", deckCount());
			}
			if (uiButton(80, 440, 420, 66, buf, "語", C_PINK, focus == 2)) {
				choice = 2;
			}
			if (uiTapped(820, 490, 120, 40)) {
				about = true;
			}
		}
		uiText(840, 515, 16, C_SOFT, "About");
		uiTextWrap(560, 330, 360, 16, 24, C_MUTED,
		           "Inside a game, pick Study panel from the Screen mode setting, or press Square to cycle to it. "
		           "Talk to anyone and their words appear beside the game. Tap a word to look it up, star it to keep it.",
		           6);
		uiDustDraw();
		if (about) {
			drawAbout();
			if (IN.released || uiBack()) {
				about = false;
				IN.released = false;
			}
		}
		uiFrameEnd();

		if (choice == 0) {
			uiDust(290, 313, C_YELLOW, 20);
			return;
		}
		if (choice == 1) {
			kakuRun();
			last = uiTime();
		} else if (choice == 2) {
			kakuReviewWords();
			last = uiTime();
		}
	}
}

/* ---- the in-game study panel ---- */

#define HISTORY 10

struct Msg {
	struct LingoText text;
	struct LingoToken tok[LINGO_MAX_TOKENS];
	int nTok;
	double arrived;
};

static struct LingoDict* sDict;
static bool sDictTried;
static struct LingoScanner* sScanner;
static struct Msg* sHist; /* ring */
static int sHistN, sHistHead;
static int sView;   /* 0 = newest, 1 = the one before... */
static int sSelTok = -1, sSelEntry;
static char sGameCode[5];
static bool sSupported;
static double sToastAt;
static char sToast[64];
static struct LingoText sPending;

void lingoPanelInit(void) {
	uiInit();
	if (!sScanner) {
		sScanner = malloc(sizeof(*sScanner));
		lingoScannerReset(sScanner);
	}
	if (!sHist) {
		sHist = calloc(HISTORY, sizeof(*sHist));
	}
	if (!sDictTried) {
		sDictTried = true;
		sDict = lingoDictLoad(DICT_PATH);
	}
}

void lingoPanelGameLoaded(const char* gameCode, const char* title) {
	(void) title;
	lingoPanelInit();
	memset(sGameCode, 0, sizeof(sGameCode));
	if (gameCode) {
		memcpy(sGameCode, gameCode, 4);
	}
	static const char* const known[] = { "AXVJ", "AXPJ", "BPEJ", "BPRJ", "BPGJ" };
	sSupported = false;
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
		if (!memcmp(sGameCode, known[i], 4)) {
			sSupported = true;
		}
	}
	lingoScannerReset(sScanner);
	sHistN = 0;
	sHistHead = 0;
	sView = 0;
	sSelTok = -1;
}

static struct Msg* msgAt(int back) {
	if (back >= sHistN) {
		return NULL;
	}
	int idx = (sHistHead - 1 - back + HISTORY * 2) % HISTORY;
	return &sHist[idx];
}

void lingoPanelFrame(const struct LingoMem* mem) {
	if (!sScanner) {
		return;
	}
	if (lingoScannerStep(sScanner, mem, &sPending)) {
		/* a message that just grew (same start) replaces the newest entry */
		struct Msg* top = msgAt(0);
		struct Msg* m;
		if (top && top->text.start == sPending.start) {
			m = top;
		} else {
			m = &sHist[sHistHead];
			sHistHead = (sHistHead + 1) % HISTORY;
			if (sHistN < HISTORY) {
				++sHistN;
			}
			m->arrived = uiTime();
			if (sView == 0) {
				sSelTok = -1;
			} else if (sView < HISTORY - 1) {
				++sView; /* keep looking at the same message */
			}
		}
		m->text = sPending;
		m->nTok = sDict ? lingoTokenize(sDict, m->text.cp, m->text.len, m->tok, LINGO_MAX_TOKENS) : 0;
		if (!sDict) {
			/* still show the text, one token per character */
			for (int i = 0; i < m->text.len && i < LINGO_MAX_TOKENS; ++i) {
				memset(&m->tok[i], 0, sizeof(m->tok[i]));
				m->tok[i].start = i;
				m->tok[i].len = 1;
				m->nTok = i + 1;
			}
		}
	}
}

static void tokenUtf8(const struct Msg* m, int t, char* out, size_t n) {
	lingoUtf8FromCodepoints(m->text.cp + m->tok[t].start, m->tok[t].len, out, n);
}

static void toast(const char* s) {
	snprintf(sToast, sizeof(sToast), "%s", s);
	sToastAt = uiTime();
}

/* Lays out (and optionally draws) a message as word chips. Returns the height used. */
static float layoutMessage(struct Msg* m, float x, float y, float w, float px, bool draw, bool interactive, float alpha) {
	float cx = x, cy = y;
	float lineH = px * 1.9f;
	float gap = px * 0.18f;
	for (int t = 0; t < m->nTok; ++t) {
		uint32_t first = m->text.cp[m->tok[t].start];
		if (first == '\n') {
			cx = x;
			cy += lineH;
			continue;
		}
		if (first == 0x3000) {
			cx += px * 0.55f;
			continue;
		}
		char s[96];
		tokenUtf8(m, t, s, sizeof(s));
		float tw = uiJpW(px, s);
		if (cx + tw > x + w && cx > x) {
			cx = x;
			cy += lineH;
		}
		if (draw) {
			bool sel = interactive && t == sSelTok;
			bool word = m->tok[t].nIds > 0;
			if (sel) {
				uiRound(cx - 4, cy - px * 1.15f, tw + 8, px * 1.55f, 8, uiAlpha(C_YELLOW, 0.22f * alpha));
			}
			uint32_t col = sel ? C_YELLOW : (word ? C_CHALK : C_SOFT);
			uiJp(cx, cy, px, uiAlpha(col, alpha), s);
			if (word && interactive) {
				/* chalk underline marks things you can tap */
				uiSegment(cx + 1, cy + 6, cx + tw - 1, cy + 6, 2, uiAlpha(sel ? C_YELLOW : C_LINE, alpha));
			}
			if (interactive && word && uiTapped(cx - 4, cy - px * 1.2f, tw + 8, px * 1.7f)) {
				sSelTok = t;
				sSelEntry = 0;
				uiDust(cx + tw / 2, cy, C_YELLOW, 6);
			}
		}
		cx += tw + gap;
	}
	return cy - y + lineH;
}

static void drawWordCard(float x, float y, float w, float h, struct Msg* m) {
	uiRound(x, y, w, h, 16, C_SHEET);
	uiRoundOutline(x, y, w, h, 16, 2, C_LINE_SOFT);
	if (!m || sSelTok < 0 || sSelTok >= m->nTok || !m->tok[sSelTok].nIds || !sDict) {
		uiJp(x + 22, y + 58, 30, uiAlpha(C_CHALK, 0.3f), "?");
		uiTextWrap(x + 64, y + 44, w - 90, 17, 25, C_SOFT,
		           sDict ? "Tap an underlined word to look it up." : "Dictionary file missing from the app folder.", 3);
		char buf[64];
		snprintf(buf, sizeof(buf), "★ %d saved words", deckCount());
		uiText(x + 22, y + h - 22, 16, C_MUTED, buf);
		return;
	}
	struct LingoToken* t = &m->tok[sSelTok];
	if (sSelEntry >= t->nIds) {
		sSelEntry = 0;
	}
	struct LingoEntry e;
	lingoDictEntry(sDict, t->ids[sSelEntry], &e);
	uiJp(x + 22, y + 58, 40, C_CHALK, e.headword);
	float hw = uiJpW(40, e.headword);
	if (strcmp(e.headword, e.reading)) {
		uiJp(x + 34 + hw, y + 56, 22, C_YELLOW, e.reading);
	}
	if (t->reason[sSelEntry][0]) {
		char buf[96];
		snprintf(buf, sizeof(buf), "← %s", t->reason[sSelEntry]);
		uiText(x + 22, y + 86, 15, C_GREEN, buf);
	}
	uiTextWrap(x + 22, y + 112, w - 44, 16, 23, C_CHALK, e.gloss, 3);
	/* other readings */
	if (t->nIds > 1) {
		float ox = x + 22;
		for (int k = 0; k < t->nIds; ++k) {
			struct LingoEntry o;
			lingoDictEntry(sDict, t->ids[k], &o);
			float ow = uiJpW(17, o.headword) + 18;
			if (ox + ow > x + w - 140) {
				break;
			}
			bool on = k == sSelEntry;
			uiRound(ox, y + h - 44, ow, 30, 10, on ? uiAlpha(C_YELLOW, 0.25f) : C_SHEET_HI);
			uiJp(ox + 9, y + h - 22, 17, on ? C_YELLOW : C_SOFT, o.headword);
			if (uiTapped(ox, y + h - 44, ow, 30)) {
				sSelEntry = k;
			}
			ox += ow + 8;
		}
	}
	/* save */
	bool saved = deckHas(e.headword, e.reading);
	float bx = x + w - 120, by = y + h - 50;
	uiRound(bx, by, 104, 38, 12, saved ? uiAlpha(C_YELLOW, 0.3f) : C_SHEET_HI);
	uiRoundOutline(bx, by, 104, 38, 12, 2, C_YELLOW);
	uiText(bx + 14, by + 26, 17, C_YELLOW, saved ? "★ Saved" : "☆ Save");
	if (!saved && uiTapped(bx, by, 104, 38)) {
		struct SavedWord sw;
		memset(&sw, 0, sizeof(sw));
		snprintf(sw.head, sizeof(sw.head), "%s", e.headword);
		snprintf(sw.reading, sizeof(sw.reading), "%s", e.reading);
		snprintf(sw.gloss, sizeof(sw.gloss), "%s", e.gloss);
		lingoUtf8FromCodepoints(m->text.cp, m->text.len, sw.context, sizeof(sw.context));
		if (deckAdd(&sw)) {
			toast("★ Saved for review in Kaku");
			uiDust(bx + 52, by + 18, C_YELLOW, 18);
		}
	}
}

bool lingoPanelDraw(float gx, float gy, float gw, float gh) {
	static double last;
	double now = uiTime();
	float dt = last ? (float) (now - last) : 0;
	last = now;
	if (dt > 0.1f) {
		dt = 0.1f;
	}
	uiPollInput();
	uiDustUpdate(dt);

	/* board behind everything but the game */
	uiRect(0, 0, SCREEN_W, gy, C_BOARD);
	uiRect(0, gy + gh, gx + gw, SCREEN_H - gy - gh, C_BOARD);
	uiRect(gx + gw, 0, SCREEN_W - gx - gw, SCREEN_H, C_BOARD);
	/* a chalk frame around the game */
	uiRoundOutline(gx - 4, gy - 4, gw + 8, gh + 8, 6, 2, uiAlpha(C_CHALK, 0.35f));

	float px0 = gx + gw + 14;
	float pw = SCREEN_W - px0 - 12;
	/* header */
	uiText(px0, 34, 20, C_YELLOW, "LINGO");
	uiJp(px0 + 74, 34, 18, C_SOFT, "よみ");
	bool openKaku = false;
	{
		float bx = SCREEN_W - 132, by = 10;
		uiRound(bx, by, 120, 34, 11, C_SHEET);
		uiRoundOutline(bx, by, 120, 34, 11, 2, C_GREEN);
		uiJp(bx + 12, by + 25, 18, C_GREEN, "書");
		uiText(bx + 40, by + 24, 16, C_CHALK, "Kaku");
		if (uiTapped(bx, by, 120, 34)) {
			openKaku = true;
		}
	}
	/* message area */
	float my = 56, mh = SCREEN_H - my - 12;
	uiRound(px0, my, pw, mh, 16, C_SHEET);
	uiRoundOutline(px0, my, pw, mh, 16, 2, C_LINE_SOFT);
	struct Msg* cur = msgAt(sView);
	if (!cur) {
		uiJpCentered(px0 + pw / 2, my + 150, 46, uiAlpha(C_CHALK, 0.18f), "…");
		uiTextWrap(px0 + 26, my + 200, pw - 52, 18, 27, C_CHALK, "Talk to someone in the game. Their words will appear here.", 3);
		if (!sSupported) {
			uiTextWrap(px0 + 26, my + 290, pw - 52, 15, 22, C_SOFT,
			           "Text reading is made for the Japanese releases of Pokémon Ruby, Sapphire, Emerald, FireRed and "
			           "LeafGreen. Other games may not show anything here.",
			           5);
		}
	} else {
		float a = (float) ((now - cur->arrived) * 4);
		float slide = a < 1 ? (1 - a) * 14 : 0;
		float used = layoutMessage(cur, px0 + 22, my + 48 + slide, pw - 44, 26, true, true, a);
		/* earlier lines */
		float ey = my + 48 + used + 18;
		if (sHistN > 1 && ey < my + mh - 60) {
			uiSegment(px0 + 22, ey - 8, px0 + pw - 22, ey - 8, 1.5f, C_LINE_SOFT);
			uiText(px0 + 22, ey + 14, 14, C_SOFT, sView ? "Newer and earlier lines" : "Earlier");
			ey += 26;
			for (int b = 0; b < sHistN && ey < my + mh - 30; ++b) {
				if (b == sView) {
					continue;
				}
				struct Msg* m = msgAt(b);
				char s[300];
				lingoUtf8FromCodepoints(m->text.cp, m->text.len, s, sizeof(s));
				for (char* p = s; *p; ++p) {
					if (*p == '\n') {
						*p = ' ';
					}
				}
				int hgt = uiTextWrap(px0 + 22, ey + 16, pw - 44, 15, 21, uiAlpha(C_MUTED, 0.85f), s, 2);
				if (uiTapped(px0 + 10, ey - 2, pw - 20, hgt + 6)) {
					sView = b;
					sSelTok = -1;
				}
				ey += hgt + 10;
			}
		}
		if (sView) {
			float bx = px0 + pw - 120, by = my + 8;
			uiRound(bx, by, 108, 28, 10, C_SHEET_HI);
			uiText(bx + 12, by + 20, 14, C_YELLOW, "↓ Newest");
			if (uiTapped(bx, by, 108, 28)) {
				sView = 0;
				sSelTok = -1;
			}
		}
	}
	/* word card below the game */
	drawWordCard(gx, gy + gh + 14, gw, SCREEN_H - (gy + gh + 14) - 12, cur);
	/* toast */
	if (sToast[0] && now - sToastAt < 1.8) {
		float a = (float) (1.8 - (now - sToastAt));
		float tw = uiTextW(17, sToast) + 36;
		uiRound((SCREEN_W - tw) / 2, 470, tw, 40, 14, uiAlpha(C_SHADE, a));
		uiTextCentered(SCREEN_W / 2, 497, 17, uiAlpha(C_YELLOW, a), sToast);
	}
	uiDustDraw();
	return openKaku;
}
