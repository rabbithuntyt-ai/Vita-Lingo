/* Lingo: a picture of each game, taken from the game itself.
 *
 * Box art comes from libretro's thumbnail server when the Vita is online.
 * When it isn't (or the game isn't there), the game browser shows a frame
 * of the player's own game instead: after about a minute of play Lingo
 * saves one screenshot per game to ux0:data/lingo/shots/<file>.png. */
#include "psp2-lingo-shot.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#define SHOTS "ux0:data/lingo/shots"

static char sName[200];
static double sPlay;
static double sLast;
static bool sDone;

static void shotPath(const char* name, char* out, size_t n) {
	char base[200];
	snprintf(base, sizeof(base), "%s", name);
	char* dot = strrchr(base, '.');
	if (dot && strlen(dot) <= 5) {
		*dot = 0;
	}
	for (char* p = base; *p; ++p) {
		if (*p == '/' || *p == ':' || *p == '\\') {
			*p = '_';
		}
	}
	snprintf(out, n, SHOTS "/%s.png", base);
}

static bool exists(const char* path) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return false;
	}
	sceIoClose(fd);
	return true;
}

void lingoShotGame(const char* fileName) {
	const char* base = fileName ? strrchr(fileName, '/') : NULL;
	snprintf(sName, sizeof(sName), "%s", base ? base + 1 : (fileName ? fileName : ""));
	sPlay = 0;
	sLast = 0;
	char p[300];
	shotPath(sName, p, sizeof(p));
	sDone = !sName[0] || exists(p);
}

static inline void toRgb(enum LingoShotFormat f, const uint8_t* src, unsigned x, uint8_t* rgb) {
	switch (f) {
	case LSF_RGBX8888: /* bytes R G B X */
		rgb[0] = src[x * 4];
		rgb[1] = src[x * 4 + 1];
		rgb[2] = src[x * 4 + 2];
		break;
	case LSF_BGRX8888: /* bytes B G R X (XRGB8888 little-endian) */
		rgb[0] = src[x * 4 + 2];
		rgb[1] = src[x * 4 + 1];
		rgb[2] = src[x * 4];
		break;
	case LSF_RGB565: {
		uint16_t v = (uint16_t) (src[x * 2] | (src[x * 2 + 1] << 8));
		rgb[0] = (uint8_t) (((v >> 11) & 31) * 255 / 31);
		rgb[1] = (uint8_t) (((v >> 5) & 63) * 255 / 63);
		rgb[2] = (uint8_t) ((v & 31) * 255 / 31);
		break;
	}
	case LSF_XRGB1555: {
		uint16_t v = (uint16_t) (src[x * 2] | (src[x * 2 + 1] << 8));
		rgb[0] = (uint8_t) (((v >> 10) & 31) * 255 / 31);
		rgb[1] = (uint8_t) (((v >> 5) & 31) * 255 / 31);
		rgb[2] = (uint8_t) ((v & 31) * 255 / 31);
		break;
	}
	}
}

static bool writePng(const char* path, const void* pixels, unsigned w, unsigned h, unsigned stride, enum LingoShotFormat f) {
	char tmp[320];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* fp = fopen(tmp, "wb");
	if (!fp) {
		return false;
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	uint8_t* row = malloc(w * 3);
	bool ok = false;
	if (png && info && row && !setjmp(png_jmpbuf(png))) {
		png_init_io(png, fp);
		png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);
		png_write_info(png, info);
		for (unsigned y = 0; y < h; ++y) {
			const uint8_t* src = (const uint8_t*) pixels + y * stride;
			for (unsigned x = 0; x < w; ++x) {
				toRgb(f, src, x, row + x * 3);
			}
			png_write_row(png, row);
		}
		png_write_end(png, NULL);
		ok = true;
	}
	if (png) {
		png_destroy_write_struct(&png, info ? &info : NULL);
	}
	free(row);
	fclose(fp);
	if (ok) {
		sceIoRemove(path);
		ok = sceIoRename(tmp, path) >= 0;
	} else {
		sceIoRemove(tmp);
	}
	return ok;
}

void lingoShotFrame(const void* pixels, unsigned w, unsigned h, unsigned stride, enum LingoShotFormat f) {
	if (sDone || !pixels || !w || !h) {
		return;
	}
	double now = sceKernelGetProcessTimeWide() / 1e6;
	if (sLast > 0) {
		double dt = now - sLast;
		sPlay += dt < 0.5 ? dt : 0.5;
	}
	sLast = now;
	if (sPlay < 60) {
		return;
	}
	sDone = true;
	/* skip a blank frame (a fade): check a few pixels vary */
	uint8_t a[3], b[3];
	toRgb(f, (const uint8_t*) pixels + (h / 3) * stride, w / 3, a);
	toRgb(f, (const uint8_t*) pixels + (2 * h / 3) * stride, 2 * w / 3, b);
	if (!memcmp(a, b, 3)) {
		sDone = false;
		sPlay = 50; /* try again in ten seconds */
		return;
	}
	sceIoMkdir("ux0:data/lingo", 0777);
	sceIoMkdir(SHOTS, 0777);
	char p[300];
	shotPath(sName, p, sizeof(p));
	writePng(p, pixels, w, h, stride, f);
}

static vita2d_texture* sTex;
static char sTexFor[300];

struct vita2d_texture* lingoShotTexture(const char* fileName) {
	char p[300];
	shotPath(fileName, p, sizeof(p));
	if (sTex && !strcmp(sTexFor, p)) {
		return sTex;
	}
	if (sTex) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(sTex);
		sTex = NULL;
	}
	snprintf(sTexFor, sizeof(sTexFor), "%s", p);
	if (exists(p)) {
		sTex = vita2d_load_PNG_file(p);
		if (sTex) {
			vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
		}
	}
	return sTex;
}
