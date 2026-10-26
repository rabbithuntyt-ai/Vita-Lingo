/* Lingo: text drawing with FreeType and our own glyph cache.
 *
 * vita2d's font code caches one bitmap per glyph at whatever size it was
 * first drawn and stretches it for every other size, drops glyphs once its
 * 512x512 atlas is full, and rounds advances to whole pixels. On a Vita that
 * showed up as blurry or missing characters and labels that measured shorter
 * than they drew. Here each glyph is rasterised at (close to) the size it is
 * drawn at, advances are kept fractional, and the cache spreads over three
 * pages that are recycled oldest-first when they fill. */
#include "psp2-lingo-font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <vita2d.h>

#define PAGE_W 1024
#define PAGE_H 1024
#define PAGES 3
#define SLOTS 8192
#define MAX_RASTER 160 /* larger text is drawn from a 160 px bitmap, scaled */

struct Page {
	vita2d_texture* tex;
	uint8_t* data;
	unsigned stride;
	int shelfX, shelfY, shelfH;
	uint32_t gen;
};

struct Glyph {
	uint32_t key;  /* codepoint | font << 24, 0 = empty */
	uint16_t size; /* raster size */
	uint8_t page;
	uint8_t hasBits;
	uint32_t gen;
	uint16_t x, y, w, h;
	int16_t left, top;
	float adv; /* at raster size */
};

struct UiFont {
	FT_Face face;
	void* fileData;
};

static FT_Library sLib;
static struct Page sPages[PAGES];
static int sCur;
static struct Glyph* sSlots;
static struct UiFont* sFonts[4];
static int sNFonts;

static bool pagesReady(void) {
	if (sPages[0].tex) {
		return true;
	}
	for (int i = 0; i < PAGES; ++i) {
		sPages[i].tex = vita2d_create_empty_texture_format(PAGE_W, PAGE_H, SCE_GXM_TEXTURE_FORMAT_U8_R111);
		if (!sPages[i].tex) {
			return false;
		}
		vita2d_texture_set_filters(sPages[i].tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		sPages[i].data = vita2d_texture_get_datap(sPages[i].tex);
		sPages[i].stride = vita2d_texture_get_stride(sPages[i].tex);
		memset(sPages[i].data, 0, sPages[i].stride * PAGE_H);
		sPages[i].gen = 1;
	}
	if (!sSlots) {
		sSlots = calloc(SLOTS, sizeof(struct Glyph));
	}
	return sSlots != NULL;
}

struct UiFont* uiFontLoad(const char* path) {
	if (!sLib && FT_Init_FreeType(&sLib)) {
		return NULL;
	}
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = malloc((size_t) n);
	if (!data || fread(data, 1, (size_t) n, f) != (size_t) n) {
		free(data);
		fclose(f);
		return NULL;
	}
	fclose(f);
	struct UiFont* font = calloc(1, sizeof(*font));
	if (!font || FT_New_Memory_Face(sLib, data, n, 0, &font->face)) {
		free(font);
		free(data);
		return NULL;
	}
	font->fileData = data;
	if (sNFonts < 4) {
		sFonts[sNFonts++] = font;
	}
	return font;
}

void uiFontFree(struct UiFont* font) {
	if (!font) {
		return;
	}
	for (int i = 0; i < sNFonts; ++i) {
		if (sFonts[i] == font) {
			sFonts[i] = NULL;
		}
	}
	FT_Done_Face(font->face);
	free(font->fileData);
	free(font);
	if (sSlots) {
		memset(sSlots, 0, SLOTS * sizeof(struct Glyph));
	}
}

static int fontId(const struct UiFont* f) {
	for (int i = 0; i < sNFonts; ++i) {
		if (sFonts[i] == f) {
			return i;
		}
	}
	return 0;
}

static unsigned rasterSize(float em) {
	if (em > MAX_RASTER) {
		return MAX_RASTER;
	}
	if (em <= 40) {
		return (unsigned) (em + 0.5f);
	}
	return ((unsigned) (em + 1)) & ~1u; /* even sizes above 40 px */
}

static bool pagePlace(struct Page* p, int w, int h, int* x, int* y) {
	if (w > PAGE_W || h > PAGE_H) {
		return false;
	}
	if (p->shelfX + w + 1 > PAGE_W) {
		p->shelfY += p->shelfH + 1;
		p->shelfX = 0;
		p->shelfH = 0;
	}
	if (p->shelfY + h + 1 > PAGE_H) {
		return false;
	}
	*x = p->shelfX;
	*y = p->shelfY;
	p->shelfX += w + 1;
	if (h > p->shelfH) {
		p->shelfH = h;
	}
	return true;
}

/* Moves to the next page, wiping it. The page wiped is the one that has gone
 * longest without new glyphs, so nothing on screen still uses it. */
static void nextPage(void) {
	sCur = (sCur + 1) % PAGES;
	struct Page* p = &sPages[sCur];
	memset(p->data, 0, p->stride * PAGE_H);
	p->shelfX = p->shelfY = p->shelfH = 0;
	p->gen++;
}

static struct Glyph* lookup(struct UiFont* font, uint32_t cp, unsigned size, bool render) {
	uint32_t key = (cp & 0xFFFFFF) | ((uint32_t) (fontId(font) + 1) << 24);
	uint32_t h = (key * 2654435761u) ^ (size * 40503u);
	struct Glyph* g = NULL;
	for (int probe = 0; probe < 32; ++probe) {
		struct Glyph* s = &sSlots[(h + (uint32_t) probe) % SLOTS];
		if (s->key == key && s->size == size) {
			g = s;
			break;
		}
		if (!s->key) {
			g = s;
			g->key = key;
			g->size = (uint16_t) size;
			g->hasBits = 0;
			g->gen = 0;
			g->adv = -1;
			break;
		}
	}
	if (!g) {
		/* a crowded neighbourhood: evict the first slot */
		g = &sSlots[h % SLOTS];
		g->key = key;
		g->size = (uint16_t) size;
		g->hasBits = 0;
		g->gen = 0;
		g->adv = -1;
	}
	bool valid = g->hasBits && sPages[g->page].gen == g->gen;
	if (g->adv >= 0 && (!render || valid)) {
		return g;
	}
	FT_Face face = font->face;
	FT_UInt gi = FT_Get_Char_Index(face, cp);
	FT_Set_Pixel_Sizes(face, 0, size);
	if (!gi || FT_Load_Glyph(face, gi, render ? FT_LOAD_RENDER : FT_LOAD_DEFAULT)) {
		g->adv = gi ? 0 : -2; /* -2: this font lacks the character */
		g->hasBits = 0;
		return g;
	}
	g->adv = face->glyph->advance.x / 64.0f;
	if (!render) {
		return g;
	}
	FT_Bitmap* bm = &face->glyph->bitmap;
	g->left = (int16_t) face->glyph->bitmap_left;
	g->top = (int16_t) face->glyph->bitmap_top;
	g->w = (uint16_t) bm->width;
	g->h = (uint16_t) bm->rows;
	if (!bm->width || !bm->rows) {
		g->hasBits = 1;
		g->page = (uint8_t) sCur;
		g->gen = sPages[sCur].gen;
		return g;
	}
	int x, y;
	if (!pagePlace(&sPages[sCur], bm->width, bm->rows, &x, &y)) {
		nextPage();
		if (!pagePlace(&sPages[sCur], bm->width, bm->rows, &x, &y)) {
			g->hasBits = 0;
			return g;
		}
	}
	struct Page* p = &sPages[sCur];
	for (unsigned r = 0; r < bm->rows; ++r) {
		memcpy(p->data + (y + r) * p->stride + x, bm->buffer + r * bm->pitch, bm->width);
	}
	g->x = (uint16_t) x;
	g->y = (uint16_t) y;
	g->page = (uint8_t) sCur;
	g->gen = p->gen;
	g->hasBits = 1;
	return g;
}

static int utf8Next(const char** s, uint32_t* cp) {
	const unsigned char* p = (const unsigned char*) *s;
	if (!*p) {
		return 0;
	}
	if (p[0] < 0x80) {
		*cp = p[0];
		*s += 1;
	} else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
		*cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
		*s += 2;
	} else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
		*cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
		*s += 3;
	} else if (p[1] && p[2] && p[3]) {
		*cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
		*s += 4;
	} else {
		*cp = '?';
		*s += 1;
	}
	return 1;
}

/* The font that has this character: the one asked for, else any loaded one. */
static struct UiFont* withChar(struct UiFont* font, uint32_t cp) {
	if (FT_Get_Char_Index(font->face, cp)) {
		return font;
	}
	for (int i = 0; i < sNFonts; ++i) {
		if (sFonts[i] && sFonts[i] != font && FT_Get_Char_Index(sFonts[i]->face, cp)) {
			return sFonts[i];
		}
	}
	return font;
}

static float run(struct UiFont* font, float x, float y, float em, uint32_t color, const char* s, bool draw) {
	if (!font || !s || !pagesReady()) {
		return 0;
	}
	unsigned size = rasterSize(em);
	float k = em / size;
	float pen = x;
	uint32_t cp;
	while (utf8Next(&s, &cp)) {
		if (cp == '\n' || cp == '\r') {
			continue;
		}
		struct UiFont* f = withChar(font, cp);
		struct Glyph* g = lookup(f, cp, size, draw);
		if (g->adv < 0) {
			pen += em * 0.5f;
			continue;
		}
		if (draw && g->hasBits && g->w && sPages[g->page].gen == g->gen) {
			vita2d_draw_texture_tint_part_scale(sPages[g->page].tex, pen + g->left * k, y - g->top * k, g->x, g->y, g->w, g->h,
			                                    k, k, color);
		}
		pen += g->adv * k;
	}
	return pen - x;
}

float uiFontDraw(struct UiFont* font, float x, float y, float em, uint32_t color, const char* s) {
	return run(font, x, y, em, color, s, true);
}

float uiFontWidth(struct UiFont* font, float em, const char* s) {
	return run(font, 0, 0, em, 0, s, false);
}

float uiFontAscent(struct UiFont* font, float em) {
	if (!font) {
		return em * 0.8f;
	}
	return em * font->face->ascender / (float) font->face->units_per_EM;
}
