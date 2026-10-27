/* Lingo / Kaku shared UI: chalkboard drawing, fonts, input, storage,
 * the title screen and the in-game study panel. */
#include "psp2-lingo-ui.h"
#include "psp2-lingo-kana.h"
#include "psp2-lingo-font.h"
#include "psp2-lingo-rom.h"
#include "psp2-lingo-quiz.h"
#include "psp2-lingo-systems.h"
#include "psp2-lingo-textsrc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <vita2d.h>

#define DATA_DIR "ux0:data/lingo"
#define DICT_PATH "app0:lingo/lingo.dic"
#define FONT_PATH "app0:lingo/klee.ttf"
#define UI_FONT_PATH "app0:lingo/ui.ttf"

static struct UiFont* sUi;
static struct UiFont* sKlee;
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
	sUi = uiFontLoad(UI_FONT_PATH);
	sKlee = uiFontLoad(FONT_PATH);
	if (!sUi) {
		sUi = sKlee;
	}
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
	vita2d_wait_rendering_done();
	if (sKlee && sKlee != sUi) {
		uiFontFree(sKlee);
	}
	uiFontFree(sUi);
	sUi = sKlee = NULL;
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

/* UI text: px is roughly the height of a capital letter */
#define UI_EM(px) ((px) * 1.2f)
#define JP_EM(px) ((px) * 1.25f)

int uiText(float x, float y, float px, uint32_t c, const char* s) {
	return (int) ceilf(uiFontDraw(sUi, x, y, UI_EM(px), c, s));
}

int uiTextW(float px, const char* s) {
	return (int) ceilf(uiFontWidth(sUi, UI_EM(px), s));
}

void uiTextCentered(float cx, float y, float px, uint32_t c, const char* s) {
	uiFontDraw(sUi, cx - uiFontWidth(sUi, UI_EM(px), s) / 2.0f, y, UI_EM(px), c, s);
}

/* Japanese display text in the handwriting font */
int uiJp(float x, float y, float px, uint32_t c, const char* s) {
	return (int) ceilf(uiFontDraw(sKlee ? sKlee : sUi, x, y, JP_EM(px), c, s));
}

int uiJpW(float px, const char* s) {
	return (int) ceilf(uiFontWidth(sKlee ? sKlee : sUi, JP_EM(px), s));
}

void uiJpCentered(float cx, float y, float px, uint32_t c, const char* s) {
	struct UiFont* f = sKlee ? sKlee : sUi;
	uiFontDraw(f, cx - uiFontWidth(f, JP_EM(px), s) / 2.0f, y, JP_EM(px), c, s);
}

void uiClip(float x, float y, float w, float h) {
	vita2d_set_clip_rectangle((int) x, (int) y, (int) (x + w), (int) (y + h));
	vita2d_enable_clipping();
}

void uiNoClip(void) {
	vita2d_disable_clipping();
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
		/* shrink a long label to fit rather than spill past the edge */
		float px = 19, avail = x + w - 14 - tx;
		while (px > 12 && uiTextW(px, label) > avail) {
			px -= 1;
		}
		uiText(tx, y + lift + h / 2 + px * 0.37f, px, C_CHALK, label);
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
		"Fonts: Klee One by Fontworks and M PLUS Rounded 1c, SIL Open Font License.",
		"Emulator cores from libretro, each under its own licence (see LICENSES.txt).",
		"Speech: whisper.cpp (MIT) with OpenAI's Whisper tiny model (MIT).",
		"Kaku practice and Lingo by Troy Stokoe.",
		"Bring your own game cartridge dumps. No games are included.",
	};
	float y = 145;
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
		y += uiTextWrap(150, y, 660, 15, 22, i == 8 ? C_SOFT : C_CHALK, lines[i], 2);
		if (!lines[i][0]) {
			y += 10;
		}
	}
	uiTextCentered(480, 440, 16, C_SOFT, "Tap anywhere to close");
}

static int checkpointMode(void);
static void setCheckpointMode(int mode);
static bool unlockStrict(void);

static char sLastPath[512];
const char* lingoLastGamePath(void) {
	return sLastPath;
}

bool lingoLauncherRun(char* path, size_t pathSize) {
	uiInit();
	double last = uiTime();
	double start = last;
	int focus = 0;
	bool about = false;
	bool unlockStrictPending = false;
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
			focus = (focus + 1) % 5;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			focus = (focus + 4) % 5;
		}
		int choice = -1;
		if (!about) {
			if (uiButton(80, 252, 420, 50, "Play a game", "遊", C_YELLOW, focus == 0)) {
				choice = 0;
			}
			char buf[64];
			if (dueKaku > 0) {
				snprintf(buf, sizeof(buf), "Kaku practice  ·  %d due", dueKaku);
			} else {
				snprintf(buf, sizeof(buf), "Kaku practice");
			}
			if (uiButton(80, 308, 420, 50, buf, "書", C_GREEN, focus == 1)) {
				choice = 1;
			}
			if (dueWords > 0) {
				snprintf(buf, sizeof(buf), "Saved words  ·  %d due", dueWords);
			} else {
				snprintf(buf, sizeof(buf), "Saved words  ·  %d", deckCount());
			}
			if (uiButton(80, 364, 420, 50, buf, "語", C_PINK, focus == 2)) {
				choice = 2;
			}
			if (uiButton(80, 420, 205, 50, "Starter games", "初", C_BLUE, focus == 3)) {
				choice = 3;
			}
			if (uiButton(295, 420, 205, 50, "From phone", "送", C_GREEN, focus == 4)) {
				choice = 4;
			}
			if (uiTapped(820, 490, 120, 40)) {
				about = true;
			}
		}
		uiText(840, 515, 16, C_SOFT, "About");
		{
			int mode = checkpointMode();
			float x = 560, y = 470;
			static const char* const LABEL[] = { "Checkpoint tests: off", "Checkpoint tests: on", "Checkpoint tests: strict" };
			uiRound(x, y, 250, 34, 12, mode ? uiAlpha(mode == 2 ? C_RED : C_YELLOW, 0.22f) : C_SHEET);
			uiText(x + 14, y + 23, 14, mode == 2 ? C_RED : mode ? C_YELLOW : C_SOFT, LABEL[mode]);
			if (!about && uiTapped(x, y, 250, 34)) {
				if (mode == 2) {
					unlockStrictPending = true;
				} else {
					setCheckpointMode(mode + 1);
				}
			}
		}
		uiTextWrap(560, 300, 360, 15, 22, C_MUTED,
		           "Thirteen consoles. Words appear beside the game as people talk: Lingo learns how each game "
		           "stores its text, or you can teach it a line. Save your game and a checkpoint tests the words "
		           "you met.",
		           7);
		uiDustDraw();
		if (about) {
			drawAbout();
			if (IN.released || uiBack()) {
				about = false;
				IN.released = false;
			}
		}
		uiFrameEnd();

		if (unlockStrictPending) {
			unlockStrictPending = false;
			if (unlockStrict()) {
				setCheckpointMode(0);
			}
			last = uiTime();
		}
		if (choice == 0) {
			if (lingoSystemsRun(path, pathSize)) {
				snprintf(sLastPath, sizeof(sLastPath), "%s", path);
				return true;
			}
			last = uiTime();
		}
		if (choice == 1) {
			kakuRun();
			last = uiTime();
		} else if (choice == 2) {
			kakuReviewWords();
			last = uiTime();
		} else if (choice == 3) {
			lingoGamesRun();
			last = uiTime();
		} else if (choice == 4) {
			lingoUploadRun();
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
	double changed; /* last time it grew */
	bool met;       /* its words counted for checkpoints (once it has finished typing) */
};

static struct LingoDict* sDict;

/* The dictionary is 20 MB, too much for newlib's default heap, so it gets its
 * own memory block like mGBA's ROM buffer. */
static struct LingoDict* loadDictBlock(const char* path) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return NULL;
	}
	SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	if (size <= 0) {
		sceIoClose(fd);
		return NULL;
	}
	size_t alloc = ((size_t) size + 0xFFF) & ~(size_t) 0xFFF;
	SceUID block = sceKernelAllocMemBlock("lingo-dict", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, alloc, NULL);
	void* base = NULL;
	if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
		sceIoClose(fd);
		return NULL;
	}
	size_t got = 0;
	while (got < (size_t) size) {
		int r = sceIoRead(fd, (uint8_t*) base + got, (size_t) size - got > 0x100000 ? 0x100000 : (size_t) size - got);
		if (r <= 0) {
			break;
		}
		got += r;
	}
	sceIoClose(fd);
	struct LingoDict* d = got == (size_t) size ? lingoDictFromMemory(base, got, false) : NULL;
	if (!d) {
		sceKernelFreeMemBlock(block);
	}
	return d;
}
static bool sDictTried;
static struct LingoScanner* sScanner;
static struct Msg* sHist; /* ring */
static int sHistN, sHistHead;
static int sView;   /* 0 = newest, 1 = the one before... */
static int sSelTok = -1, sSelEntry;
static char sGameCode[5];
static bool sSupported;
static char sGameKey[48];
static double sToastAt;
static char sToast[64];
static struct LingoText sPending;
static struct LingoMem sMem;
static struct LingoBook sBook;
static char sBookPath[128];
static bool sBookLoaded;
static int sPrevDirty;
static double sLastFrame;
static int sCheckpointsOn = -1;

void lingoCheckpointTest(const struct LingoDict* d, struct LingoBook* book);

/* 0 off, 1 on, 2 strict (turning it off takes passing a test) */
static int checkpointMode(void) {
	if (sCheckpointsOn < 0) {
		sCheckpointsOn = 1;
		FILE* f = fopen(DATA_DIR "/settings.txt", "r");
		if (f) {
			char line[64];
			while (fgets(line, sizeof(line), f)) {
				if (!strncmp(line, "checkpoints=", 12)) {
					sCheckpointsOn = line[12] == '0' ? 0 : line[12] == '2' ? 2 : 1;
				}
			}
			fclose(f);
		}
	}
	return sCheckpointsOn;
}

bool lingoCheckpointsEnabled(void) {
	return checkpointMode() > 0;
}

bool lingoCheckpointsStrict(void) {
	return checkpointMode() == 2;
}

static void setCheckpointMode(int mode) {
	sCheckpointsOn = mode;
	FILE* f = fopen(DATA_DIR "/settings.txt", "w");
	if (f) {
		fprintf(f, "checkpoints=%d\n", mode);
		fclose(f);
	}
}

void lingoSetCheckpointsEnabled(bool on) {
	setCheckpointMode(on ? 1 : 0);
}

/* Strict mode: leaving it means passing a test first. */
static bool unlockStrict(void) {
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(SCREEN_W / 2, 190, 24, C_YELLOW, "Strict checkpoints are on");
		uiTextWrap(200, 240, 560, 16, 24, C_CHALK,
		           "You asked Lingo not to let you skip tests. To switch them off, pass a short kana test first.", 3);
		bool go = uiButton(200, 340, 270, 60, "Take the test", "試", C_GREEN, false);
		bool cancel = uiButton(490, 340, 270, 60, "Keep strict", NULL, C_SOFT, false);
		uiFrameEnd();
		if (cancel || uiBack()) {
			return false;
		}
		if (go) {
			break;
		}
	}
	struct LingoBook b;
	lingoBookInit(&b);
	lingoCheckpointTest(lingoDictGet(), &b); /* returns once passed */
	lingoBookFree(&b);
	return true;
}

static void bookSave(void) {
	if (sBookLoaded && sBookPath[0]) {
		lingoBookSave(&sBook, sBookPath);
	}
}

bool lingoCheckpointDue(void) {
	return sBookLoaded && lingoCheckpointsEnabled() && sBook.pending;
}

void lingoCheckpointRun(void) {
	lingoCheckpointTest(sDict, &sBook);
	bookSave();
	sLastFrame = 0;
}

const struct LingoDict* lingoDictGet(void) {
	if (!sDictTried) {
		sDictTried = true;
		sDict = loadDictBlock(DICT_PATH);
	}
	return sDict;
}

void lingoNoteLookup(const uint32_t* cp, int len, const struct LingoToken* tok) {
	if (!sBookLoaded || !lingoDictGet() || len <= 0) {
		return;
	}
	struct LingoText t;
	memset(&t, 0, sizeof(t));
	t.len = len < LINGO_MAX_TEXT ? len : LINGO_MAX_TEXT;
	memcpy(t.cp, cp, t.len * sizeof(uint32_t));
	lingoBookMeet(&sBook, sDict, &t, tok, 1);
}

static bool sWantsLookup;
bool lingoPanelWantsLookup(void) {
	bool w = sWantsLookup;
	sWantsLookup = false;
	return w;
}

static bool sWantsTeach;
static bool sWantsFix;
bool lingoPanelWantsTeach(void) {
	bool w = sWantsTeach;
	sWantsTeach = false;
	return w;
}

void lingoPanelSetSource(const char* system, const char* baseName, const uint8_t* rom, size_t romSize) {
	if (sSupported) {
		/* Gen 3 Pokémon: only the text box itself, exactly as it prints */
		lingoTextGameUnloaded();
		return;
	}
	lingoTextSetGame(system, sGameKey, baseName, rom, romSize);
}

void lingoPanelSetRegions(const struct LingoRegions* rs) {
	lingoTextSetRegions(rs);
}

void lingoPanelGameUnloaded(void) {
	lingoTextGameUnloaded();
	bookSave();
	lingoBookFree(&sBook);
	sBookLoaded = false;
}
static struct LingoRom sRom;
static bool sRomTried;
static vita2d_texture* sSpriteTex;
static int sSpriteSpecies = -1;

/* Pokémon names in a line belong to the player's own game: look them up there. */
static int tokenSpecies(const struct LingoText* t, const struct LingoToken* tok) {
	if (!sSupported || tok->len < 2 || tok->len > 5) {
		return -1;
	}
	for (int i = 0; i < tok->len; ++i) {
		uint32_t c = t->cp[tok->start + i];
		if (c < 0x30A1 || c > 0x30FC) {
			return -1;
		}
	}
	if (!sRomTried && sMem.rom) {
		sRomTried = true;
		lingoRomIndex(sMem.rom, sMem.romSize, &sRom);
	}
	if (!sRom.names) {
		return -1;
	}
	return lingoRomFindSpecies(&sRom, t->cp + tok->start, tok->len);
}

static bool spriteFor(int species) {
	if (species == sSpriteSpecies && sSpriteTex) {
		return true;
	}
	static uint32_t px[64 * 64];
	if (!sRom.ready || !lingoRomSprite(&sRom, species, px)) {
		return false;
	}
	if (!sSpriteTex) {
		sSpriteTex = vita2d_create_empty_texture(64, 64);
		if (!sSpriteTex) {
			return false;
		}
		vita2d_texture_set_filters(sSpriteTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
	}
	uint8_t* dst = vita2d_texture_get_datap(sSpriteTex);
	unsigned stride = vita2d_texture_get_stride(sSpriteTex);
	for (int y = 0; y < 64; ++y) {
		memcpy(dst + y * stride, px + y * 64, 64 * 4);
	}
	sSpriteSpecies = species;
	return true;
}

void lingoPanelInit(void) {
	uiInit();
	if (!sScanner) {
		sScanner = malloc(sizeof(*sScanner));
		lingoScannerReset(sScanner);
	}
	if (!sHist) {
		sHist = calloc(HISTORY, sizeof(*sHist));
	}
	lingoDictGet();
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
	/* one word book per game, keyed by its product code (or title) */
	if (sBookLoaded) {
		lingoPanelGameUnloaded();
	}
	char key[40] = "";
	const char* src = gameCode && gameCode[0] ? gameCode : (title ? title : "game");
	for (int i = 0, k = 0; src[i] && k < 32; ++i) {
		char c = src[i];
		key[k++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
		key[k] = 0;
	}
	snprintf(sBookPath, sizeof(sBookPath), DATA_DIR "/game-%s.bin", key[0] ? key : "game");
	snprintf(sGameKey, sizeof(sGameKey), "%s", key[0] ? key : "game");
	lingoBookInit(&sBook);
	lingoBookLoad(&sBook, sBookPath);
	sBookLoaded = true;
	sPrevDirty = 0;
	sLastFrame = 0;
	memset(&sRom, 0, sizeof(sRom));
	sRomTried = false;
	sSpriteSpecies = -1;
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

/* letters only, so the same line from two readers compares equal */
static int squash(const struct LingoText* t, uint32_t* out, int max) {
	int n = 0;
	for (int i = 0; i < t->len && n < max; ++i) {
		if (t->cp[i] != '\n' && t->cp[i] != 0x3000 && t->cp[i] != ' ') {
			out[n++] = t->cp[i];
		}
	}
	return n;
}

static void retokenize(struct Msg* m) {
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

/* A line from a reader: new, a longer version of the newest (it's still
 * typing out), or the same line again from another reader (ignored). */
static void pushText(const struct LingoText* t) {
	struct Msg* top = msgAt(0);
	struct Msg* m = NULL;
	if (top) {
		static uint32_t a[LINGO_MAX_TEXT], b[LINGO_MAX_TEXT];
		int na = squash(&top->text, a, LINGO_MAX_TEXT), nb = squash(t, b, LINGO_MAX_TEXT);
		bool fresh = uiTime() - top->arrived < 20;
		if (na == nb && !memcmp(a, b, sizeof(uint32_t) * (size_t) na)) {
			return; /* the same line */
		}
		if (fresh && nb < na && !memcmp(a, b, sizeof(uint32_t) * (size_t) nb)) {
			return; /* an earlier, shorter view of what's on show */
		}
		if (top->text.start == t->start || (fresh && na < nb && !memcmp(a, b, sizeof(uint32_t) * (size_t) na))) {
			m = top; /* the line grew */
		}
	}
	if (!m) {
		if (top && !top->met && sDict && sBookLoaded) {
			lingoBookMeet(&sBook, sDict, &top->text, top->tok, top->nTok);
			top->met = true;
		}
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
	m->text = *t;
	m->changed = uiTime();
	m->met = false;
	retokenize(m);
}

/* words count as met once their line has finished typing out */
static void meetSettled(void) {
	struct Msg* top = msgAt(0);
	if (top && !top->met && sDict && sBookLoaded && uiTime() - top->changed > 1.5) {
		lingoBookMeet(&sBook, sDict, &top->text, top->tok, top->nTok);
		top->met = true;
	}
}

void lingoPanelFrame(const struct LingoMem* mem, int saveDirty) {
	if (!sScanner) {
		return;
	}
	sMem = *mem;
	/* play time, and the moment a save finishes (its dirty flag clears) */
	double now = uiTime();
	if (sLastFrame > 0 && sBookLoaded) {
		float dt = (float) (now - sLastFrame);
		lingoCheckpointTick(&sBook, dt > 0.5f ? 0.5f : dt);
		if (sBook.playSince >= LINGO_CP_FORCE_PLAY) {
			sBook.pending = true;
		}
	}
	sLastFrame = now;
	if (sPrevDirty && !saveDirty && sBookLoaded) {
		lingoCheckpointSaved(&sBook);
		bookSave();
	}
	sPrevDirty = saveDirty;
	/* Gen 3 Pokémon: two readers (the text window and the message buffer);
	 * everything else: tables, learning and the screen */
	if (sSupported) {
		if (lingoScannerStep(sScanner, mem, &sPending)) {
			pushText(&sPending);
		}
	} else if (lingoTextStep(&sPending)) {
		pushText(&sPending);
	}
	meetSettled();
}

static void tokenUtf8(const struct Msg* m, int t, char* out, size_t n) {
	lingoUtf8FromCodepoints(m->text.cp + m->tok[t].start, m->tok[t].len, out, n);
}

static void toast(const char* s) {
	snprintf(sToast, sizeof(sToast), "%s", s);
	sToastAt = uiTime();
}

static const char* grammarNote(const char* reason);
static bool hasKanji(const char* s);
static struct Msg* sFixMsg;  /* a long press asked to fix a letter in this line */
static double sPressAt;      /* when the current touch began */

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
			bool word = m->tok[t].nIds > 0 || m->tok[t].isWord;
			if (sel) {
				uiRound(cx - 4, cy - px * 1.15f, tw + 8, px * 1.55f, 8, uiAlpha(C_YELLOW, 0.22f * alpha));
			}
			uint32_t col = sel ? C_YELLOW : (word ? C_CHALK : C_SOFT);
			uiJp(cx, cy, px, uiAlpha(col, alpha), s);
			/* furigana over words written with kanji */
			if (m->tok[t].nIds && sDict && hasKanji(s)) {
				struct LingoEntry fe;
				if (lingoDictEntry(sDict, m->tok[t].ids[0], &fe) && !strcmp(fe.headword, s) && strcmp(fe.reading, s)) {
					float rw = uiJpW(px * 0.42f, fe.reading);
					uiJp(cx + (tw - rw) / 2, cy - px * 1.12f, px * 0.42f, uiAlpha(C_YELLOW, 0.85f * alpha), fe.reading);
				}
			}
			if (word && interactive) {
				/* chalk underline marks things you can tap */
				uiSegment(cx + 1, cy + 6, cx + tw - 1, cy + 6, 2, uiAlpha(sel ? C_YELLOW : C_LINE, alpha));
			}
			if (interactive && word && uiTapped(cx - 4, cy - px * 1.2f, tw + 8, px * 1.7f)) {
				sSelTok = t;
				sSelEntry = 0;
				uiDust(cx + tw / 2, cy, C_YELLOW, 6);
			}
			/* hold a word to correct a letter Lingo misread */
			if (interactive && m->text.hasCodes && IN.down && uiHeld(cx - 4, cy - px * 1.2f, tw + 8, px * 1.7f) &&
			    fabsf(IN.x - IN.sx) < 8 && fabsf(IN.y - IN.sy) < 8 && uiTime() - sPressAt > 0.7 && sPressAt > 0) {
				sFixMsg = m;
				sPressAt = 0;
			}
		}
		cx += tw + gap;
	}
	return cy - y + lineH;
}

static void drawWordCard(float x, float y, float w, float h, struct Msg* m) {
	uiRound(x, y, w, h, 16, C_SHEET);
	uiRoundOutline(x, y, w, h, 16, 2, C_LINE_SOFT);
	int species = (m && sSelTok >= 0 && sSelTok < m->nTok) ? tokenSpecies(&m->text, &m->tok[sSelTok]) : -1;
	float spriteW = 0;
	if (species >= 0 && spriteFor(species)) {
		/* the Pokémon itself, from the player's cartridge */
		float sx = x + w - 120, sy = y + 10;
		uiRound(sx - 6, sy - 2, 112, 112, 14, uiAlpha(C_CHALK, 0.06f));
		vita2d_draw_texture_scale(sSpriteTex, sx, sy, 1.55f, 1.55f);
		uiText(sx + 4, sy + 120, 12, C_SOFT, "from your game");
		spriteW = 124;
	}
	if (m && sSelTok >= 0 && sSelTok < m->nTok && !m->tok[sSelTok].nIds && m->tok[sSelTok].isWord) {
		char name[64];
		tokenUtf8(m, sSelTok, name, sizeof(name));
		uiJp(x + 22, y + 58, 40, C_CHALK, name);
		if (species >= 0) {
			uiText(x + 22, y + 92, 17, C_YELLOW, "Pokémon");
			uiTextWrap(x + 22, y + 122, w - 44 - spriteW, 15, 22, C_CHALK,
			           "A name, written in katakana like every Pokémon name. Try sounding it out.", 3);
		} else {
			uiTextWrap(x + 22, y + 96, w - 44 - spriteW, 15, 22, C_SOFT,
			           "Not in the dictionary. Katakana like this is usually a name or a made-up word.", 3);
		}
		if (species >= 0) {
			bool saved = deckHas(name, name);
			float bx = x + 22, by = y + h - 50;
			uiRound(bx, by, 104, 38, 12, saved ? uiAlpha(C_YELLOW, 0.3f) : C_SHEET_HI);
			uiRoundOutline(bx, by, 104, 38, 12, 2, C_YELLOW);
			uiText(bx + 14, by + 26, 17, C_YELLOW, saved ? "★ Saved" : "☆ Save");
			if (!saved && uiTapped(bx, by, 104, 38)) {
				struct SavedWord sw;
				memset(&sw, 0, sizeof(sw));
				snprintf(sw.head, sizeof(sw.head), "%s", name);
				snprintf(sw.reading, sizeof(sw.reading), "%s", name);
				snprintf(sw.gloss, sizeof(sw.gloss), "a Pokémon's name");
				lingoUtf8FromCodepoints(m->text.cp, m->text.len, sw.context, sizeof(sw.context));
				if (deckAdd(&sw)) {
					toast("★ Saved for review in Kaku");
				}
			}
		}
		return;
	}
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
	/* the word, with its reading above any kanji (furigana) */
	float hy = y + 54;
	if (hasKanji(e.headword)) {
		hy = y + 66;
		float hw = uiJpW(34, e.headword);
		float rw = uiJpW(13, e.reading);
		uiJp(x + 22 + (hw - rw) / 2, y + 28, 13, C_YELLOW, e.reading);
		uiJp(x + 22, hy, 34, C_CHALK, e.headword);
	} else {
		uiJp(x + 22, hy, 34, C_CHALK, e.headword);
	}
	float gy2 = hy + 26;
	if (t->reason[sSelEntry][0]) {
		char buf[200];
		const char* note = grammarNote(t->reason[sSelEntry]);
		snprintf(buf, sizeof(buf), "← %s%s%s", t->reason[sSelEntry], note ? "  ·  " : "", note ? note : "");
		gy2 += uiTextWrap(x + 22, gy2, w - 44 - spriteW, 13, 17, C_GREEN, buf, 2) + 4;
	}
	int lines = (int) ((y + h - (t->nIds > 1 ? 52 : 14) - gy2) / 21);
	if (lines > 0) {
		uiTextWrap(x + 22, gy2 + 4, w - 44 - spriteW - (t->nIds > 1 ? 0 : 120), 15, 21, C_CHALK, e.gloss, lines);
	}
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

/* ---- grammar notes for the endings the tokenizer takes off ---- */

static const struct {
	const char* label;
	const char* note;
} NOTES[] = {
	{ "polite", "〜ます: the polite present/future. What shopkeepers and strangers say." },
	{ "polite past", "〜ました: polite past. \"did\"." },
	{ "polite negative", "〜ません: polite \"don't / won't\"." },
	{ "polite negative past", "〜ませんでした: polite \"didn't\"." },
	{ "polite volitional", "〜ましょう: \"let's ...\" or \"shall I ...?\"." },
	{ "negative", "〜ない: plain \"not\". Casual speech and the base for many endings." },
	{ "negative past", "〜なかった: plain \"didn't\"." },
	{ "negative (casual)", "〜ん: a spoken, clipped 〜ない (わからん = わからない)." },
	{ "negative (rough)", "〜ねえ: a rough, slangy 〜ない. Tough characters talk like this." },
	{ "past", "〜た: plain past. \"did\" or \"has done\"." },
	{ "te-form", "〜て: links to what comes next: \"...and\", 〜てください (please), 〜ている (is doing)." },
	{ "was -ing", "〜ていた: \"was doing\" or \"had been\"." },
	{ "potential / passive", "〜られる: \"can do\", or \"is done (to someone)\". Context decides." },
	{ "can", "〜える/〜られる: \"can do\"." },
	{ "can come", "こられる: \"can come\"." },
	{ "passive", "〜れる/〜られる: \"is done (to someone)\"." },
	{ "causative", "〜せる/〜させる: \"make or let someone do\"." },
	{ "want to", "〜たい: \"want to do\"." },
	{ "try", "〜てみる: \"try doing and see\"." },
	{ "have done", "〜てある: \"has been done (and stays so)\"." },
	{ "in advance", "〜ておく: \"do in advance\" or \"leave it so\"." },
	{ "completely", "〜てしまう/〜ちゃう: \"do completely\", often \"end up doing (oops)\"." },
	{ "goes on", "〜ていく: \"go on doing\", change moving away." },
	{ "comes to", "〜てくる: \"come to do\", change moving toward now." },
	{ "for me", "〜てくれる: someone does it for me / us." },
	{ "for someone", "〜てあげる: I do it for someone." },
	{ "please", "〜てください / 〜なさい: a request or instruction." },
	{ "command", "Imperative: a blunt order. Common in battle and from bosses." },
	{ "if", "〜ば/〜たら: \"if\"." },
	{ "if / when", "〜たら: \"if\" or \"when\" (once it has happened)." },
	{ "even if", "〜ても: \"even if\"." },
	{ "while", "〜ながら: \"while doing\"." },
	{ "without", "〜ずに/〜ないで: \"without doing\"." },
	{ "looks like", "〜そう: \"looks like / seems\"." },
	{ "too much", "〜すぎる: \"too much\"." },
	{ "becomes", "〜くなる/〜になる: \"becomes\"." },
	{ "adverb", "〜く: the adverb form of an い-adjective (はやい → はやく, quickly)." },
	{ "-tari", "〜たり: listing actions as examples (\"do things like ... and ...\")." },
};

/* The note for the outermost ending in a reason like "polite past, te-form". */
static const char* grammarNote(const char* reason) {
	char first[LINGO_MAX_REASON];
	snprintf(first, sizeof(first), "%s", reason);
	char* comma = strchr(first, ',');
	if (comma) {
		*comma = 0;
	}
	for (size_t i = 0; i < sizeof(NOTES) / sizeof(NOTES[0]); ++i) {
		if (!strcmp(NOTES[i].label, first)) {
			return NOTES[i].note;
		}
	}
	return NULL;
}

static bool hasKanji(const char* s) {
	uint32_t cp[64];
	int n = lingoUtf8Decode(s, cp, 64);
	for (int i = 0; i < n; ++i) {
		if (cp[i] >= 0x4E00 && cp[i] <= 0x9FFF) {
			return true;
		}
	}
	return false;
}

/* ---- the study panel, shared by every console ---- */

static bool sBreakdown;   /* message area: word chips or a word-by-word list */
static float sScroll;     /* breakdown scroll */
static bool sVoice;       /* the front end can replay and transcribe audio */

void lingoPanelSetVoice(bool on) {
	sVoice = on;
}

static bool chip(float x, float y, float w, float h, const char* label, const char* jp, uint32_t col) {
	bool held = uiHeld(x, y, w, h);
	uiRound(x, y + (held ? 1 : 0), w, h, 10, held ? C_SHEET_HI : C_SHEET);
	uiRoundOutline(x, y + (held ? 1 : 0), w, h, 10, 2, uiAlpha(col, 0.8f));
	float tx = x + 10;
	if (jp) {
		uiJp(tx, y + h / 2 + 8, 16, col, jp);
		tx += uiJpW(16, jp) + 6;
	}
	uiText(tx, y + h / 2 + 6, 14, C_CHALK, label);
	return uiTapped(x, y, w, h);
}

/* One row of the breakdown: the word as written, its dictionary form with
 * reading, a short meaning and the ending's name. Returns the height used. */
static float breakdownRow(struct Msg* m, int t, float x, float y, float w, bool draw) {
	struct LingoToken* tok = &m->tok[t];
	char surf[96];
	tokenUtf8(m, t, surf, sizeof(surf));
	float h = 30;
	if (!draw) {
		return h + (tok->nIds && tok->reason[0][0] ? 18 : 0);
	}
	bool sel = t == sSelTok;
	if (sel) {
		uiRound(x - 8, y - 22, w + 16, h + (tok->nIds && tok->reason[0][0] ? 18 : 0) + 4, 8, uiAlpha(C_YELLOW, 0.15f));
	}
	uiJp(x, y, 20, tok->nIds ? C_CHALK : C_SOFT, surf);
	float sw = fmaxf(uiJpW(20, surf) + 14, 92);
	if (tok->nIds && sDict) {
		struct LingoEntry e;
		lingoDictEntry(sDict, tok->ids[0], &e);
		char head[160];
		if (strcmp(e.headword, surf) || strcmp(e.reading, surf)) {
			if (hasKanji(e.headword)) {
				snprintf(head, sizeof(head), "%s（%s）", e.headword, e.reading);
			} else {
				snprintf(head, sizeof(head), "%s", e.headword);
			}
		} else {
			head[0] = 0;
		}
		float hx = x + sw;
		if (head[0]) {
			uiJp(hx, y, 15, C_YELLOW, head);
			hx += uiJpW(15, head) + 10;
		}
		char g[96];
		lingoShortGloss(e.gloss, g, sizeof(g));
		uiTextWrap(hx, y - 1, x + w - hx, 13, 16, C_MUTED, g, 1);
		if (tok->reason[0][0]) {
			char r[80];
			snprintf(r, sizeof(r), "← %s", tok->reason[0]);
			uiText(x + sw, y + 18, 12, C_GREEN, r);
		}
	}
	return h + (tok->nIds && tok->reason[0][0] ? 18 : 0);
}

/* ---- the word-by-word line ---- */

enum WordKind { WK_OTHER, WK_VERB, WK_NOUN, WK_ADJ, WK_PARTICLE, WK_ADVERB, WK_NAME };

static const struct {
	const char* label;
	uint32_t color;
} KINDS[] = {
	[WK_OTHER] = { "expression", RGBA(0xd8, 0xd8, 0xc8, 0xff) },
	[WK_VERB] = { "verb", RGBA(0xff, 0x9e, 0x8a, 0xff) },
	[WK_NOUN] = { "noun", RGBA(0x9c, 0xc8, 0xf0, 0xff) },
	[WK_ADJ] = { "adjective", RGBA(0xa6, 0xe3, 0xbd, 0xff) },
	[WK_PARTICLE] = { "particle", RGBA(0x9d, 0xb2, 0xa4, 0xff) },
	[WK_ADVERB] = { "adverb", RGBA(0xf1, 0xd3, 0x7c, 0xff) },
	[WK_NAME] = { "name", RGBA(0xf4, 0xa6, 0xc0, 0xff) },
};

static enum WordKind kindOf(const struct LingoEntry* e, const struct LingoToken* t) {
	if (!e) {
		return WK_NAME;
	}
	unsigned p = e->pos;
	bool inflected = t && t->reason[0][0];
	if (p & LPOS_PRT) {
		return WK_PARTICLE;
	}
	if (p & (LPOS_V1 | LPOS_V5 | LPOS_VK)) {
		return WK_VERB;
	}
	if ((p & LPOS_VS) && (inflected || !(p & LPOS_NOUN))) {
		return WK_VERB;
	}
	if (p & (LPOS_ADJI | LPOS_ADJNA)) {
		return WK_ADJ;
	}
	if (p & LPOS_NOUN) {
		return WK_NOUN;
	}
	if (p & LPOS_ADV) {
		return WK_ADVERB;
	}
	return WK_OTHER;
}

/* What particles do, in a word or two (the dictionary's glosses are long) */
static const char* particleMeaning(const char* s) {
	static const char* const P[][2] = {
		{ "は", "topic" },      { "が", "subject" },  { "を", "object" },   { "に", "to / at" },  { "で", "at / by" },
		{ "の", "'s / of" },    { "と", "and / with" }, { "も", "also" },     { "へ", "toward" },   { "から", "from / so" },
		{ "まで", "until" },     { "よ", "! (telling)" }, { "ね", "right?" },   { "か", "? (asking)" }, { "な", "! / don't" },
		{ "や", "and (lists)" }, { "より", "than" },     { "ぞ", "! (strong)" }, { "さ", "you see" },  { "わ", "! (soft)" },
		{ "って", "quoting" },   { "けど", "but" },      { "のに", "even though" }, { "ので", "because" }, { "だけ", "only" },
	};
	for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); ++i) {
		if (!strcmp(P[i][0], s)) {
			return P[i][1];
		}
	}
	return NULL;
}

struct WordInfo {
	char surface[96];
	char romaji[96];
	char meaning[96];
	char furigana[64];
	enum WordKind kind;
	bool known;
};

static void wordInfo(struct Msg* m, int t, struct WordInfo* wi) {
	struct LingoToken* tok = &m->tok[t];
	memset(wi, 0, sizeof(*wi));
	tokenUtf8(m, t, wi->surface, sizeof(wi->surface));
	struct LingoEntry e;
	bool have = tok->nIds && sDict && lingoDictEntry(sDict, tok->ids[0], &e);
	wi->known = have;
	wi->kind = have ? kindOf(&e, tok) : WK_NAME;
	/* romaji from the kana as written; for kanji, from the dictionary reading */
	uint32_t cps[64];
	int n = tok->len < 64 ? tok->len : 64;
	memcpy(cps, m->text.cp + tok->start, (size_t) n * sizeof(uint32_t));
	if (hasKanji(wi->surface) && have) {
		n = lingoUtf8Decode(e.reading, cps, 64);
		if (strcmp(e.headword, wi->surface) == 0 && strcmp(e.reading, wi->surface)) {
			snprintf(wi->furigana, sizeof(wi->furigana), "%s", e.reading);
		}
	}
	lingoRomaji(cps, n, wi->kind == WK_PARTICLE, wi->romaji, sizeof(wi->romaji));
	if (!have) {
		snprintf(wi->meaning, sizeof(wi->meaning), "%s", tokenSpecies(&m->text, tok) >= 0 ? "Pokémon" : "");
		return;
	}
	const char* pm = wi->kind == WK_PARTICLE ? particleMeaning(wi->surface) : NULL;
	if (pm) {
		snprintf(wi->meaning, sizeof(wi->meaning), "%s", pm);
	} else {
		lingoShortGloss(e.gloss, wi->meaning, sizeof(wi->meaning));
	}
}

/* fits s into w pixels at px, with an ellipsis */
static void fitText(char* s, float px, float w) {
	if (uiTextW(px, s) <= w) {
		return;
	}
	size_t l = strlen(s);
	while (l > 0) {
		--l;
		while (l > 0 && ((unsigned char) s[l] & 0xC0) == 0x80) {
			--l;
		}
		s[l] = 0;
		char t[128];
		snprintf(t, sizeof(t), "%s…", s);
		if (uiTextW(px, t) <= w) {
			snprintf(s, 96, "%s", t);
			return;
		}
	}
}

/* The line as blocks: the word (coloured by kind, furigana over kanji),
 * romaji under it, and a short meaning under that. Tap one for the full
 * definition. Returns the height used. */
static float wordBlocks(struct Msg* m, float x0, float y0, float w, bool draw, float alpha) {
	const float WORD = 21, ROW = 74, GAP = 6, MAXW = 150;
	float x = x0, y = y0;
	for (int t = 0; t < m->nTok; ++t) {
		uint32_t first = m->text.cp[m->tok[t].start];
		if (first == '\n') {
			if (x > x0) {
				x = x0;
				y += ROW;
			}
			continue;
		}
		if (first == 0x3000 || first == ' ') {
			x += 8;
			continue;
		}
		struct WordInfo wi;
		wordInfo(m, t, &wi);
		bool punct = !wi.known && !wi.meaning[0] && m->tok[t].len == 1 && !lingoIsKana(first) && !(first >= 0x4E00 && first <= 0x9FFF);
		float ww = uiJpW(WORD, wi.surface);
		float bw = ww;
		if (!punct) {
			float rw = uiTextW(11, wi.romaji), mw = uiTextW(11, wi.meaning);
			bw = fmaxf(bw, fminf(MAXW, fmaxf(rw, mw)));
			bw = fmaxf(bw, 24);
		}
		bw += 12;
		if (x + bw > x0 + w && x > x0) {
			x = x0;
			y += ROW;
		}
		if (draw) {
			uint32_t col = KINDS[wi.kind].color;
			bool sel = t == sSelTok;
			if (!punct) {
				uiRound(x, y, bw, ROW - 8, 10, sel ? uiAlpha(col, 0.28f * alpha) : uiAlpha(col, 0.09f * alpha));
				uiRound(x, y + ROW - 12, bw, 4, 2, uiAlpha(col, (sel ? 0.95f : 0.6f) * alpha)); /* colour bar */
			}
			float cx = x + (bw - ww) / 2;
			if (wi.furigana[0]) {
				float fw = uiJpW(9, wi.furigana);
				uiJp(x + (bw - fw) / 2, y + 12, 9, uiAlpha(C_YELLOW, alpha), wi.furigana);
			}
			uiJp(cx, y + 34, WORD, uiAlpha(punct ? C_SOFT : (wi.known ? C_CHALK : col), alpha), wi.surface);
			if (!punct) {
				char r[96], mn[96];
				snprintf(r, sizeof(r), "%s", wi.romaji);
				snprintf(mn, sizeof(mn), "%s", wi.meaning);
				fitText(r, 11, bw - 8);
				fitText(mn, 11, bw - 8);
				uiTextCentered(x + bw / 2, y + 48, 11, uiAlpha(C_MUTED, alpha), r);
				uiTextCentered(x + bw / 2, y + 61, 11, uiAlpha(col, alpha), mn);
				if (uiTapped(x, y, bw, ROW - 8) && fabsf(IN.y - IN.sy) < 10) {
					sSelTok = sSelTok == t ? -1 : t;
					sSelEntry = 0;
					uiDust(x + bw / 2, y + 30, col, 6);
				}
				/* hold a word to correct a letter Lingo misread */
				if (m->text.hasCodes && IN.down && uiHeld(x, y, bw, ROW - 8) && fabsf(IN.x - IN.sx) < 8 &&
				    fabsf(IN.y - IN.sy) < 8 && sPressAt > 0 && uiTime() - sPressAt > 0.7) {
					sFixMsg = m;
					sPressAt = 0;
				}
			}
		}
		x += bw + GAP;
	}
	return y - y0 + ROW;
}

/* The selected word's full entry, under the line. Returns the height used. */
static float wordDetail(struct Msg* m, float x, float y, float w) {
	if (sSelTok < 0 || sSelTok >= m->nTok) {
		uiText(x + 4, y + 18, 13, C_SOFT, "Tap a word for its full meaning. Hold it to fix a misread letter.");
		return 30;
	}
	struct LingoToken* t = &m->tok[sSelTok];
	struct WordInfo wi;
	wordInfo(m, sSelTok, &wi);
	uint32_t col = KINDS[wi.kind].color;
	float y0 = y;
	if (!t->nIds || !sDict) {
		uiRound(x, y, w, 96, 12, C_SHEET_HI);
		uiJp(x + 14, y + 40, 26, col, wi.surface);
		uiText(x + 14, y + 62, 13, C_MUTED, wi.romaji);
		const char* why = tokenSpecies(&m->text, t) >= 0 ? "A Pokémon's name, written in katakana like all of them."
		                                                  : "Not in the dictionary: katakana like this is usually a name or a made-up word.";
		uiTextWrap(x + 14, y + 84, w - 28, 13, 17, C_CHALK, why, 1);
		return 104;
	}
	if (sSelEntry >= t->nIds) {
		sSelEntry = 0;
	}
	struct LingoEntry e;
	lingoDictEntry(sDict, t->ids[sSelEntry], &e);
	wi.kind = kindOf(&e, t);
	col = KINDS[wi.kind].color;
	/* measure first, then draw on a sheet */
	float h = 0;
	for (int pass = 0; pass < 2; ++pass) {
		bool draw = pass == 1;
		if (draw) {
			uiRound(x, y0, w, h, 12, C_SHEET_HI);
			uiRound(x, y0, 5, h, 2, col);
		}
		float yy = y0 + 10;
		float hy = yy + 30;
		if (hasKanji(e.headword)) {
			float hw = uiJpW(28, e.headword), rw = uiJpW(12, e.reading);
			if (draw) {
				uiJp(x + 16 + (hw - rw) / 2, yy + 8, 12, C_YELLOW, e.reading);
			}
			hy = yy + 42;
		}
		if (draw) {
			uiJp(x + 16, hy, 28, C_CHALK, e.headword);
			float hw = uiJpW(28, e.headword);
			char r[96];
			uint32_t rc[64];
			int rn = lingoUtf8Decode(e.reading, rc, 64);
			lingoRomaji(rc, rn, wi.kind == WK_PARTICLE, r, sizeof(r));
			uiText(x + 28 + hw, hy - 14, 13, C_MUTED, r);
			uiRound(x + 28 + hw, hy - 6, uiTextW(11, KINDS[wi.kind].label) + 14, 18, 9, uiAlpha(col, 0.25f));
			uiText(x + 35 + hw, hy + 7, 11, col, KINDS[wi.kind].label);
			/* save */
			bool saved = deckHas(e.headword, e.reading);
			float bx = x + w - 92, by = yy;
			uiRound(bx, by, 80, 30, 10, saved ? uiAlpha(C_YELLOW, 0.3f) : C_SHEET);
			uiRoundOutline(bx, by, 80, 30, 10, 2, C_YELLOW);
			uiText(bx + 10, by + 21, 14, C_YELLOW, saved ? "★ Saved" : "☆ Save");
			if (!saved && uiTapped(bx, by, 80, 30)) {
				struct SavedWord sw;
				memset(&sw, 0, sizeof(sw));
				snprintf(sw.head, sizeof(sw.head), "%s", e.headword);
				snprintf(sw.reading, sizeof(sw.reading), "%s", e.reading);
				snprintf(sw.gloss, sizeof(sw.gloss), "%s", e.gloss);
				lingoUtf8FromCodepoints(m->text.cp, m->text.len, sw.context, sizeof(sw.context));
				if (deckAdd(&sw)) {
					toast("★ Saved for review in Kaku");
					uiDust(bx + 40, by + 15, C_YELLOW, 18);
				}
			}
		}
		yy = hy + 12;
		if (t->reason[sSelEntry][0]) {
			char buf[220];
			const char* note = grammarNote(t->reason[sSelEntry]);
			snprintf(buf, sizeof(buf), "%s ← %s%s%s", wi.surface, t->reason[sSelEntry], note ? ". " : "", note ? note : "");
			yy += draw ? uiTextWrap(x + 16, yy + 14, w - 32, 13, 17, C_GREEN, buf, 3) : 17 * 3;
		}
		/* the meanings, one per line */
		const char* g = e.gloss;
		if (*g == '[') {
			const char* q = strchr(g, ']');
			g = q ? q + 2 : g;
		}
		yy += 6;
		int lines = draw ? uiTextWrap(x + 16, yy + 16, w - 32, 15, 20, C_CHALK, g, 5) : 0;
		if (!draw) {
			lines = 20 * 3;
		}
		yy += lines + 10;
		/* other words this could be */
		if (t->nIds > 1) {
			float ox = x + 16;
			if (draw) {
				uiText(ox, yy + 14, 12, C_SOFT, "Or:");
			}
			ox += 30;
			for (int k = 0; k < t->nIds; ++k) {
				struct LingoEntry o;
				lingoDictEntry(sDict, t->ids[k], &o);
				float ow = uiJpW(15, o.headword) + 16;
				if (ox + ow > x + w - 10) {
					break;
				}
				if (draw) {
					bool on = k == sSelEntry;
					uiRound(ox, yy, ow, 22, 8, on ? uiAlpha(C_YELLOW, 0.25f) : C_SHEET);
					uiJp(ox + 8, yy + 17, 15, on ? C_YELLOW : C_SOFT, o.headword);
					if (uiTapped(ox, yy, ow, 22)) {
						sSelEntry = k;
					}
				}
				ox += ow + 6;
			}
			yy += 30;
		}
		if (!draw) {
			h = yy - y0 + 4;
		} else {
			y = yy;
		}
	}
	return h + 6;
}

/* Below the game: earlier lines (tap one to look at it again) and the colour key. */
static void earlierLines(float x, float y, float w, float h) {
	uiRound(x, y, w, h, 14, C_SHEET);
	/* the key */
	float kx = x + 12, ky = y + h - 12;
	static const enum WordKind ORDER[] = { WK_VERB, WK_NOUN, WK_ADJ, WK_ADVERB, WK_PARTICLE, WK_NAME };
	for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); ++i) {
		uint32_t col = KINDS[ORDER[i]].color;
		uiRound(kx, ky - 9, 12, 8, 3, col);
		uiText(kx + 16, ky, 11, col, KINDS[ORDER[i]].label);
		kx += 16 + uiTextW(11, KINDS[ORDER[i]].label) + 14;
	}
	float ly = y + 22;
	uiText(x + 12, ly, 12, C_SOFT, sHistN > 1 ? "Earlier lines" : "Earlier lines will collect here");
	ly += 6;
	for (int b = 0; b < sHistN && ly < y + h - 40; ++b) {
		if (b == sView) {
			continue;
		}
		struct Msg* m = msgAt(b);
		char s[400];
		lingoUtf8FromCodepoints(m->text.cp, m->text.len, s, sizeof(s));
		for (char* p = s; *p; ++p) {
			if (*p == '\n') {
				*p = ' ';
			}
		}
		int hgt = uiTextWrap(x + 12, ly + 18, w - 24, 13, 18, uiAlpha(C_MUTED, 0.9f), s, 1);
		if (uiTapped(x + 4, ly, w - 8, hgt + 4)) {
			sView = b;
			sSelTok = -1;
		}
		ly += hgt + 4;
	}
}

static enum LingoPanelAction panelDraw(float gx, float gy, float gw, float gh, bool retro, const char* system, const char* game) {
	static double last;
	double now = uiTime();
	float dt = last ? (float) (now - last) : 0;
	last = now;
	if (dt > 0.1f) {
		dt = 0.1f;
	}
	uiPollInput();
	uiDustUpdate(dt);
	if (IN.pressed) {
		sPressAt = now;
	}
	enum LingoPanelAction act = LPA_NONE;

	/* board behind everything but the game */
	uiRect(0, 0, SCREEN_W, gy, C_BOARD);
	uiRect(0, gy + gh, gx + gw, SCREEN_H - gy - gh, C_BOARD);
	uiRect(0, 0, gx, SCREEN_H, C_BOARD);
	uiRect(gx + gw, 0, SCREEN_W - gx - gw, SCREEN_H, C_BOARD);
	uiRoundOutline(gx - 4, gy - 4, gw + 8, gh + 8, 6, 2, uiAlpha(C_CHALK, 0.35f));

	float px0 = gx + gw + 14;
	float pw = SCREEN_W - px0 - 12;

	/* row 1: name and buttons */
	uiText(px0, 32, 19, C_YELLOW, "LINGO");
	if (system && system[0]) {
		uiJp(px0 + 70, 32, 14, C_SOFT, system);
	}
	{
		int nb = retro ? 3 : 2;
		float bw = retro ? 84 : 100, bh = 30, gap = 6;
		float bx = SCREEN_W - 12 - nb * bw - (nb - 1) * gap, by = 10;
		if (chip(bx, by, bw, bh, "Look up", "あ", C_BLUE)) {
			act = LPA_LOOKUP;
		}
		if (chip(bx + bw + gap, by, bw, bh, "Kaku", "書", C_GREEN)) {
			act = LPA_KAKU;
		}
		if (retro && chip(bx + 2 * (bw + gap), by, bw, bh, "Menu", NULL, C_SOFT)) {
			act = LPA_MENU;
		}
	}
	/* row 2: where the text comes from, and the checkpoint meter */
	{
		float y = 60;
		float prog = 1;
		bool reading = sSupported;
		const char* st = sSupported ? "Reading Pokémon text · built in" : lingoTextStatus(&prog, &reading);
		uiCircle(px0 + 5, y - 5, 4, reading ? C_GREEN : uiAlpha(C_YELLOW, 0.5f + 0.5f * sinf((float) now * 4)));
		uiTextWrap(px0 + 16, y, pw - 100, 13, 16, reading ? C_SOFT : C_YELLOW, st, 1);
		if (!sSupported) {
			if (chip(SCREEN_W - 12 - 84, y - 20, 84, 26, reading ? "Teach" : "Teach…", NULL, C_YELLOW)) {
				act = LPA_TEACH;
			}
			if (!reading) {
				uiRound(px0 + 16, y + 6, pw - 120, 5, 2.5f, C_SHEET);
				uiRound(px0 + 16, y + 6, fmaxf(5, (pw - 120) * prog), 5, 2.5f, C_YELLOW);
			}
		}
		if (sBookLoaded && lingoCheckpointsEnabled()) {
			float fw = sBook.wordsSince / (float) LINGO_CP_MIN_WORDS;
			float ft = sBook.playSince / (float) LINGO_CP_MIN_PLAY;
			float f = fminf(1, fmaxf(fw, ft));
			float my = 80;
			uiRound(px0, my, pw * 0.42f, 8, 4, C_SHEET);
			uiRound(px0, my, fmaxf(8, pw * 0.42f * f), 8, 4, f >= 1 ? C_YELLOW : C_GREEN);
			char cp[64];
			if (f >= 1) {
				snprintf(cp, sizeof(cp), "Checkpoint at your next save");
			} else {
				snprintf(cp, sizeof(cp), "Next checkpoint · %d new words", sBook.wordsSince);
			}
			uiText(px0 + pw * 0.42f + 10, my + 9, 12, f >= 1 ? C_YELLOW : C_SOFT, cp);
		}
	}

	/* the line on show, word by word with meanings */
	float my = 96, mh = SCREEN_H - my - 12;
	uiRound(px0, my, pw, mh, 16, C_SHEET);
	uiRoundOutline(px0, my, pw, mh, 16, 2, C_LINE_SOFT);
	struct Msg* cur = msgAt(sView);
	if (cur) {
		float tx = px0 + 14, ty = my + 10;
		if (sView) {
			if (chip(tx, ty, 96, 26, "↓ Newest", NULL, C_YELLOW)) {
				sView = 0;
				sSelTok = -1;
			}
			tx += 102;
		}
		if (cur->text.hasCodes && (cur->text.start >> 24) < 8) {
			/* text that isn't dialogue (a menu, a name list): stop reading it */
			if (chip(tx, ty, 58, 26, "Hide", NULL, C_RED)) {
				lingoTextIgnore(cur->text.start);
				toast("Text from there will be skipped");
			}
		}
		if (sVoice) {
			float vx = px0 + pw - 14 - 3 * 64 - 12;
			if (chip(vx, ty, 64, 26, "▶ Hear", NULL, C_PINK)) {
				act = LPA_HEAR;
			}
			if (chip(vx + 70, ty, 64, 26, "▶ Slow", NULL, C_PINK)) {
				act = LPA_HEAR_SLOW;
			}
			if (chip(vx + 140, ty, 64, 26, "✎ Text", NULL, C_PINK)) {
				act = LPA_TRANSCRIBE;
			}
		}
		float cy0 = my + 42, ch = mh - 48;
		uiClip(px0 + 4, cy0, pw - 8, ch);
		/* drag to scroll when a long line and its definition don't fit */
		static float scroll;
		static struct Msg* scrolledFor;
		if (scrolledFor != cur) {
			scroll = 0;
			scrolledFor = cur;
		}
		{
			static float lastY;
			if (IN.down && IN.sx > px0 && IN.sy > cy0) {
				if (IN.pressed) {
					lastY = IN.y;
				}
				if (fabsf(IN.y - IN.sy) > 10) {
					scroll += IN.y - lastY;
				}
				lastY = IN.y;
			}
		}
		float a = (float) ((now - cur->arrived) * 4);
		float y = cy0 + 8 + scroll;
		float used = wordBlocks(cur, px0 + 14, y, pw - 28, true, a > 1 ? 1 : a);
		y += used + 6;
		y += wordDetail(cur, px0 + 14, y, pw - 28);
		float contentH = y - (cy0 + 8 + scroll);
		float minScroll = fminf(0, ch - 16 - contentH);
		if (scroll < minScroll) {
			scroll = minScroll;
		}
		if (scroll > 0) {
			scroll = 0;
		}
		uiNoClip();
	} else {
		bool reading = sSupported;
		float prog;
		if (!sSupported) {
			lingoTextStatus(&prog, &reading);
		}
		uiJpCentered(px0 + pw / 2, my + 130, 46, uiAlpha(C_CHALK, 0.18f), "…");
		if (reading) {
			uiTextWrap(px0 + 24, my + 180, pw - 48, 17, 25, C_CHALK, "Talk to someone in the game. Their words will appear here with their meanings.", 3);
		} else {
			uiTextWrap(px0 + 24, my + 170, pw - 48, 16, 23, C_CHALK,
			           "Lingo is learning how this game stores its text. Play normally and let some dialogue print; "
			           "lines will start appearing here.",
			           4);
			uiTextWrap(px0 + 24, my + 270, pw - 48, 14, 20, C_SOFT,
			           "Faster: tap Teach and type one line you can see. Lingo can also read the screen itself.", 4);
		}
	}
	/* below the game: earlier lines and the colour key */
	float cardY = gy + gh + 14;
	if (SCREEN_H - cardY - 12 >= 70) {
		earlierLines(gx, cardY, gw, SCREEN_H - cardY - 12);
	}
	(void) game;
	/* toast */
	if (sToast[0] && now - sToastAt < 1.8) {
		float a = (float) (1.8 - (now - sToastAt));
		float tw = uiTextW(17, sToast) + 36;
		uiRound((SCREEN_W - tw) / 2, 470, tw, 40, 14, uiAlpha(C_SHADE, a));
		uiTextCentered(SCREEN_W / 2, 497, 17, uiAlpha(C_YELLOW, a), sToast);
	}
	uiDustDraw();
	return act;
}

/* Fix a letter: pick the misread character, type what it should be. The
 * game's table learns it, and lines already shown are corrected. */
static void fixLetterRun(struct Msg* m) {
	int pick = -1;
	uint32_t typed[4];
	int n = 0;
	bool kata = false;
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiText(24, 34, 15, C_YELLOW, "FIX A LETTER");
		uiTextWrap(24, 58, 900, 14, 20, C_SOFT,
		           pick < 0 ? "Tap the character Lingo got wrong." : "Type what it should be, then Done.", 2);
		/* the line, one tile per character */
		float x = 24, y = 92;
		for (int i = 0; i < m->text.len; ++i) {
			uint32_t c = m->text.cp[i];
			if (c == '\n') {
				continue;
			}
			char s[8] = { 0 };
			lingoUtf8Encode(c == 0x3000 ? 0x3000 : c, s);
			float tw = 40;
			if (x + tw > SCREEN_W - 24) {
				x = 24;
				y += 46;
			}
			if (y > 190) {
				break;
			}
			bool can = m->text.code[i] != 0xFFFF && c != 0x3000;
			uiRound(x, y, tw - 4, 40, 8, i == pick ? uiAlpha(C_YELLOW, 0.3f) : can ? C_SHEET : C_BOARD);
			uiJpCentered(x + (tw - 4) / 2, y + 30, 20, can ? C_CHALK : C_SOFT, s);
			if (can && uiTapped(x, y, tw - 4, 40)) {
				pick = i;
				n = 0;
			}
			x += tw;
		}
		bool done = false;
		if (pick >= 0) {
			char now[32] = "";
			lingoUtf8FromCodepoints(typed, n, now, sizeof(now));
			uiText(600, 40, 14, C_SOFT, "Should be:");
			uiJp(700, 44, 26, C_GREEN, now);
			done = lingoKeyboard(230, typed, &n, 1, &kata, false);
		} else {
			uiText(24, 520, 14, C_SOFT, "○ Back");
		}
		uiFrameEnd();
		if (uiBack()) {
			return;
		}
		if (done) {
			if (n == 1 && pick >= 0) {
				uint16_t code = m->text.code[pick];
				if (lingoTextFix(code, typed[0])) {
					/* correct every line on show that used it */
					for (int b = 0; b < sHistN; ++b) {
						struct Msg* h = msgAt(b);
						if (!h->text.hasCodes) {
							continue;
						}
						for (int i = 0; i < h->text.len; ++i) {
							if (h->text.code[i] == code) {
								h->text.cp[i] = typed[0];
							}
						}
						retokenize(h);
					}
					toast("Fixed. Lingo will read it right from now on");
				}
			}
			return;
		}
	}
}

bool lingoPanelDraw(float gx, float gy, float gw, float gh) {
	enum LingoPanelAction act = panelDraw(gx, gy, gw, gh, false, NULL, NULL);
	if (act == LPA_LOOKUP) {
		sWantsLookup = true;
	} else if (act == LPA_TEACH) {
		sWantsTeach = true;
	} else if (sFixMsg) {
		sWantsFix = true;
	}
	return act == LPA_KAKU;
}

bool lingoPanelWantsFix(void) {
	bool w = sWantsFix;
	sWantsFix = false;
	return w;
}

void lingoPanelFixRun(void) {
	if (sFixMsg) {
		fixLetterRun(sFixMsg);
		sFixMsg = NULL;
	}
}

enum LingoPanelAction lingoRetroPanelDraw(float gx, float gy, float gw, float gh, const char* system, const char* game) {
	enum LingoPanelAction act = panelDraw(gx, gy, gw, gh, true, system, game);
	if (act == LPA_NONE && sFixMsg) {
		act = LPA_FIX;
	}
	return act;
}

double lingoPanelCurrentArrived(void) {
	struct Msg* m = msgAt(0);
	return m ? m->arrived : 0;
}

const struct LingoText* lingoPanelCurrentText(void) {
	struct Msg* m = msgAt(sView);
	return m ? &m->text : NULL;
}

void lingoPanelAddText(const uint32_t* cp, int len) {
	if (len <= 0) {
		return;
	}
	static struct LingoText t;
	memset(&t, 0, sizeof(t));
	t.len = len < LINGO_MAX_TEXT ? len : LINGO_MAX_TEXT;
	memcpy(t.cp, cp, (size_t) t.len * sizeof(uint32_t));
	t.start = 0xFE000000u | (uint32_t) sHistHead;
	pushText(&t);
	sView = 0;
	sSelTok = -1;
}
