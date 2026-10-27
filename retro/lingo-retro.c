/* Lingo's libretro front end for the Vita. One copy is linked with each core
 * (app0:/<core>.self). Started from Lingo's title screen with
 * argv[1] = game path and argv[2] = console id; returns to it on quit.
 *
 * Plays the game, and around it: the study panel (live text reading,
 * lookup, Kaku, voiced-line replay and transcription), checkpoints when the
 * game saves, save states, rewind, the core's own settings, and a menu. */
#include "libretro.h"
#include "retro-options.h"
#include "retro-rewind.h"
#include "retro-util.h"
#include "retro-voice.h"
#include "retro-whisper.h"
#include "retro-zip.h"

#include "../src/psp2-lingo-net.h"
#include "../src/psp2-lingo-reader.h"
#include "../src/psp2-lingo-shot.h"
#include "../src/psp2-lingo-systems.h"
#include "../src/psp2-lingo-textsrc.h"
#include "../src/psp2-lingo-ui.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <vita2d.h>

/* newlib's heap for the core and the game; the dictionary has its own block */
int _newlib_heap_size_user = 160 * 1024 * 1024;

/* Executable memory for recompiling cores (PCSX ReARMed). They size it with
 * _newlib_vm_size_user and ask for it with getVMBlock(), as they do under
 * RetroArch's Vita start-up code. */
extern int _newlib_vm_size_user __attribute__((weak));
static SceUID sVMBlock = -1;
int getVMBlock(void) {
	if (sVMBlock < 0 && &_newlib_vm_size_user && _newlib_vm_size_user > 0) {
		unsigned size = ((unsigned) _newlib_vm_size_user + 0xFFFFF) & ~0xFFFFFu;
		sVMBlock = sceKernelAllocMemBlockForVM("code", size);
	}
	return sVMBlock;
}

#define DATA "ux0:data/lingo"
#define SYSTEM_DIR DATA "/system"
#define SAVE_DIR DATA "/saves"
#define STATE_DIR DATA "/states"
#define OPTION_DIR DATA "/options"
#define TMP_DIR DATA "/tmp"

static const struct LingoSystem* sSys;
static char sGamePath[1024];
static char sGameName[256];
static char sBaseName[256];
static struct retro_system_av_info sAv;
static enum retro_pixel_format sFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static vita2d_texture* sTex;
static unsigned sTexW, sTexH, sFrameW, sFrameH;
static enum retro_pixel_format sTexFormat;
static struct RetroAudio sAudio;
static volatile bool sAudioRun;
static SceUID sAudioThread;
static uint32_t sPad;
static int16_t sAnalog[2][2];
static FILE* sLog;
static bool sStudy = true;
static void* sContent;
static size_t sContentSize;
static struct RetroOptions* sOpts;
static retro_core_options_update_display_callback_t sOptDisplayCb;
static struct VoiceCapture sVoice;
static bool sVoiceSystem;

/* front-end settings */
static bool sRewindOn = true;
static bool sAutoSkip = true;

/* memory the core told us about (SET_MEMORY_MAPS) */
#define MAX_MAPS 16
static struct {
	void* ptr;
	size_t len;
} sMaps[MAX_MAPS];
static int sNMaps;

/* ---- logging (a core's own messages, kept for bug reports) ---- */

static void logf_(enum retro_log_level level, const char* fmt, ...) {
	if (!sLog || level < RETRO_LOG_WARN) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(sLog, fmt, ap);
	va_end(ap);
	fflush(sLog);
}

static void note(const char* fmt, ...) {
	if (!sLog) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(sLog, fmt, ap);
	va_end(ap);
	fputc('\n', sLog);
	fflush(sLog);
}

/* ---- front-end settings ---- */

static void settingsLoad(void) {
	FILE* f = fopen(DATA "/frontend.cfg", "r");
	if (!f) {
		return;
	}
	char line[64];
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "rewind=", 7)) {
			sRewindOn = line[7] == '1';
		} else if (!strncmp(line, "autoskip=", 9)) {
			sAutoSkip = line[9] == '1';
		}
	}
	fclose(f);
}

static void settingsSave(void) {
	FILE* f = fopen(DATA "/frontend.cfg", "w");
	if (f) {
		fprintf(f, "rewind=%d\nautoskip=%d\n", sRewindOn, sAutoSkip);
		fclose(f);
	}
}

/* ---- libretro callbacks ---- */

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format*) data;
		if (f == RETRO_PIXEL_FORMAT_0RGB1555 || f == RETRO_PIXEL_FORMAT_RGB565 || f == RETRO_PIXEL_FORMAT_XRGB8888) {
			sFormat = f;
			return true;
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = SYSTEM_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = SAVE_DIR;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logf_;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*(unsigned*) data = RETRO_LANGUAGE_JAPANESE;
		return true;
	/* the core's settings */
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		roSetVariables(sOpts, (const struct retro_variable*) data);
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
		roSetDefinitions(sOpts, (const struct retro_core_option_definition*) data);
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
		roSetDefinitions(sOpts, ((const struct retro_core_options_intl*) data)->us);
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		roSetV2(sOpts, (const struct retro_core_options_v2*) data);
		return false; /* no categories: the core uses plain descriptions */
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		roSetV2(sOpts, ((const struct retro_core_options_v2_intl*) data)->us);
		return false;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
		const struct retro_core_option_display* d = data;
		if (d && d->key) {
			roSetVisible(sOpts, d->key, d->visible);
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
		const struct retro_core_options_update_display_callback* c = data;
		sOptDisplayCb = c ? c->callback : NULL;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		v->value = v->key ? roGet(sOpts, v->key) : NULL;
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = sOpts->changed;
		sOpts->changed = false;
		return true;
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
		const struct retro_memory_map* m = data;
		sNMaps = 0;
		for (unsigned i = 0; m && i < m->num_descriptors && sNMaps < MAX_MAPS; ++i) {
			const struct retro_memory_descriptor* d = &m->descriptors[i];
			if (!d->ptr || d->len < 1024 || (d->flags & RETRO_MEMDESC_CONST)) {
				continue; /* ROM and tiny bits aren't where text is written */
			}
			sMaps[sNMaps].ptr = (uint8_t*) d->ptr + d->offset;
			sMaps[sNMaps].len = d->len;
			++sNMaps;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		sAv.geometry = *(const struct retro_game_geometry*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		sAv = *(const struct retro_system_av_info*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	default:
		return false;
	}
}

static SceGxmTextureFormat gxmFormat(enum retro_pixel_format f) {
	switch (f) {
	case RETRO_PIXEL_FORMAT_RGB565:
		return SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB;
	case RETRO_PIXEL_FORMAT_XRGB8888:
		return SCE_GXM_TEXTURE_FORMAT_X8U8U8U8_1RGB;
	default:
		return SCE_GXM_TEXTURE_FORMAT_X1U5U5U5_1RGB;
	}
}

static void video(const void* data, unsigned w, unsigned h, size_t pitch) {
	if (!data || !w || !h) {
		return; /* a duplicate frame: keep the last one */
	}
	if (!sTex || w > sTexW || h > sTexH || sTexFormat != sFormat) {
		if (sTex) {
			vita2d_wait_rendering_done();
			vita2d_free_texture(sTex);
		}
		sTexW = w > sAv.geometry.max_width ? w : sAv.geometry.max_width;
		sTexH = h > sAv.geometry.max_height ? h : sAv.geometry.max_height;
		sTexFormat = sFormat;
		sTex = vita2d_create_empty_texture_format(sTexW, sTexH, gxmFormat(sFormat));
		if (!sTex) {
			return;
		}
	}
	unsigned bpp = sFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
	uint8_t* dst = vita2d_texture_get_datap(sTex);
	unsigned stride = vita2d_texture_get_stride(sTex);
	for (unsigned y = 0; y < h; ++y) {
		memcpy(dst + y * stride, (const uint8_t*) data + y * pitch, w * bpp);
	}
	sFrameW = w;
	sFrameH = h;
	lingoShotFrame(data, w, h, (unsigned) pitch,
	               sFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? LSF_BGRX8888
	               : sFormat == RETRO_PIXEL_FORMAT_RGB565 ? LSF_RGB565
	                                                      : LSF_XRGB1555);
}

static int16_t sOne[64 * 2];
static size_t sOneN;

static size_t audioBatch(const int16_t* data, size_t frames) {
	raPush(&sAudio, data, frames);
	vcPush(&sVoice, data, frames);
	return frames;
}

static void audioSample(int16_t l, int16_t r) {
	sOne[sOneN * 2] = l;
	sOne[sOneN * 2 + 1] = r;
	if (++sOneN == 64) {
		raPush(&sAudio, sOne, sOneN);
		vcPush(&sVoice, sOne, sOneN);
		sOneN = 0;
	}
}

static void inputPoll(void) {
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	sPad = pad.buttons;
	sAnalog[0][0] = (int16_t) ((pad.lx - 128) * 256);
	sAnalog[0][1] = (int16_t) ((pad.ly - 128) * 256);
	sAnalog[1][0] = (int16_t) ((pad.rx - 128) * 256);
	sAnalog[1][1] = (int16_t) ((pad.ry - 128) * 256);
}

/* libretro uses the SNES layout: B bottom, A right, Y left, X top */
static const uint32_t MAP[16] = {
	[RETRO_DEVICE_ID_JOYPAD_B] = SCE_CTRL_CROSS,     [RETRO_DEVICE_ID_JOYPAD_Y] = SCE_CTRL_SQUARE,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = SCE_CTRL_SELECT, [RETRO_DEVICE_ID_JOYPAD_START] = SCE_CTRL_START,
	[RETRO_DEVICE_ID_JOYPAD_UP] = SCE_CTRL_UP,       [RETRO_DEVICE_ID_JOYPAD_DOWN] = SCE_CTRL_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = SCE_CTRL_LEFT,   [RETRO_DEVICE_ID_JOYPAD_RIGHT] = SCE_CTRL_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A] = SCE_CTRL_CIRCLE,    [RETRO_DEVICE_ID_JOYPAD_X] = SCE_CTRL_TRIANGLE,
	[RETRO_DEVICE_ID_JOYPAD_L] = SCE_CTRL_L1,        [RETRO_DEVICE_ID_JOYPAD_R] = SCE_CTRL_R1,
	[RETRO_DEVICE_ID_JOYPAD_L2] = SCE_CTRL_L2,       [RETRO_DEVICE_ID_JOYPAD_R2] = SCE_CTRL_R2,
	[RETRO_DEVICE_ID_JOYPAD_L3] = SCE_CTRL_L3,       [RETRO_DEVICE_ID_JOYPAD_R3] = SCE_CTRL_R3,
};

static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port != 0) {
		return 0;
	}
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
			int16_t bits = 0;
			for (unsigned i = 0; i < 16; ++i) {
				if (sPad & MAP[i]) {
					bits |= 1 << i;
				}
			}
			return bits;
		}
		return id < 16 && (sPad & MAP[id]) ? 1 : 0;
	}
	if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG && index < 2 && id < 2) {
		return sAnalog[index][id];
	}
	return 0;
}

/* ---- audio thread ---- */

static int audioThread(SceSize args, void* argp) {
	(void) args;
	(void) argp;
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	if (port < 0) {
		return 0;
	}
	static int16_t buf[1024 * 2];
	while (sAudioRun) {
		raPull(&sAudio, buf, 1024);
		sceAudioOutOutput(port, buf);
	}
	sceAudioOutReleasePort(port);
	return 0;
}

/* ---- saves ---- */

static void savePath(char* out, size_t n, const char* dir, const char* ext) {
	snprintf(out, n, "%s/%s.%s", dir, sGameName, ext);
}

static void loadSram(void) {
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void* data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data) {
		return;
	}
	char p[512];
	savePath(p, sizeof(p), SAVE_DIR, "srm");
	FILE* f = fopen(p, "rb");
	if (f) {
		fread(data, 1, size, f);
		fclose(f);
	}
}

static void writeSram(void) {
	size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void* data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data) {
		return;
	}
	char p[512], tmp[520];
	savePath(p, sizeof(p), SAVE_DIR, "srm");
	snprintf(tmp, sizeof(tmp), "%s.tmp", p);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	if (ok) {
		sceIoRemove(p);
		sceIoRename(tmp, p);
	}
}

static bool stateIo(bool save) {
	size_t size = retro_serialize_size();
	if (!size) {
		return false;
	}
	void* buf = malloc(size);
	if (!buf) {
		return false;
	}
	char p[512];
	savePath(p, sizeof(p), STATE_DIR, "state");
	bool ok = false;
	if (save) {
		if (retro_serialize(buf, size)) {
			FILE* f = fopen(p, "wb");
			if (f) {
				ok = fwrite(buf, 1, size, f) == size;
				fclose(f);
			}
		}
	} else {
		FILE* f = fopen(p, "rb");
		if (f) {
			ok = fread(buf, 1, size, f) == size && retro_unserialize(buf, size);
			fclose(f);
		}
	}
	free(buf);
	return ok;
}

/* ---- rewind ---- */

static struct RetroRewind* sRewind;
static uint8_t* sRwState;
static size_t sRwSize;
static int sRwEvery = 4;

static void rewindSetup(void) {
	rwFree(sRewind);
	sRewind = NULL;
	free(sRwState);
	sRwState = NULL;
	if (!sRewindOn) {
		return;
	}
	sRwSize = retro_serialize_size();
	if (!sRwSize || sRwSize > 8 * 1024 * 1024) {
		note("rewind: off (state size %u)", (unsigned) sRwSize);
		return;
	}
	/* bigger states: capture less often, so it costs about the same */
	sRwEvery = sRwSize <= 256 * 1024 ? 2 : sRwSize <= 1024 * 1024 ? 4 : 12;
	sRwState = malloc(sRwSize);
	sRewind = sRwState ? rwNew(sRwSize, 24 * 1024 * 1024) : NULL;
	note("rewind: %s (state %u bytes, every %d frames)", sRewind ? "on" : "no memory", (unsigned) sRwSize, sRwEvery);
}

/* ---- screens ---- */

static void gameRect(float* x, float* y, float* w, float* h) {
	float aspect = sAv.geometry.aspect_ratio > 0 ? sAv.geometry.aspect_ratio
	                                             : (sFrameH ? (float) sFrameW / sFrameH : 4.0f / 3.0f);
	/* study: game top left like the GBA layout, leaving room for the word card */
	float areaW = sStudy ? 480 : SCREEN_W, areaH = sStudy ? 360 : SCREEN_H;
	float ox = sStudy ? 12 : 0, oy = sStudy ? 12 : 0;
	float hh = areaH, ww = hh * aspect;
	if (ww > areaW) {
		ww = areaW;
		hh = ww / aspect;
	}
	*x = ox + (sStudy ? (areaW - ww) / 2 : (areaW - ww) / 2);
	*y = oy + (sStudy ? 0 : (areaH - hh) / 2);
	*w = ww;
	*h = hh;
}

static void drawGame(float alpha) {
	if (!sTex || !sFrameW) {
		return;
	}
	float x, y, w, h;
	gameRect(&x, &y, &w, &h);
	vita2d_texture_set_filters(sTex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
	vita2d_draw_texture_tint_part_scale(sTex, x, y, 0, 0, sFrameW, sFrameH, w / sFrameW, h / sFrameH,
	                                    RGBA(255, 255, 255, (int) (255 * alpha)));
}

static void toastLoop(const char* text) {
	double start = uiTime();
	while (uiTime() - start < 0.9) {
		uiPollInput();
		uiFrameBegin();
		uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
		drawGame(0.4f);
		uiTextCentered(SCREEN_W / 2, SCREEN_H / 2, 22, C_YELLOW, text);
		uiFrameEnd();
	}
}

/* The core's own settings. */
static void optionsRun(void) {
	if (sOptDisplayCb) {
		sOptDisplayCb();
	}
	int sel = 0;
	float scroll = 0;
	while (true) {
		uiPollInput();
		int vis[RO_MAX];
		int nv = 0;
		for (int i = 0; i < sOpts->n; ++i) {
			if (sOpts->opt[i].visible && sOpts->opt[i].nValues > 1) {
				vis[nv++] = i;
			}
		}
		if (IN.buttonsPressed & SCE_CTRL_DOWN) {
			sel = sel + 1 < nv ? sel + 1 : sel;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			sel = sel > 0 ? sel - 1 : 0;
		}
		if (nv && (IN.buttonsPressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS))) {
			roStep(sOpts, vis[sel], 1);
		}
		if (nv && (IN.buttonsPressed & SCE_CTRL_LEFT)) {
			roStep(sOpts, vis[sel], -1);
		}
		float top = 90, rowH = 50, bottom = 500;
		if (sel * rowH - scroll > bottom - top - rowH) {
			scroll = sel * rowH - (bottom - top - rowH);
		}
		if (sel * rowH < scroll) {
			scroll = sel * rowH;
		}
		uiFrameBegin();
		uiChalkBackground();
		uiText(40, 50, 22, C_YELLOW, "GAME SETTINGS");
		uiText(260, 50, 14, C_SOFT, "The emulator's own options. Most take effect right away; some after a restart.");
		uiClip(0, top, SCREEN_W, bottom - top);
		for (int k = 0; k < nv; ++k) {
			const struct RetroOption* o = &sOpts->opt[vis[k]];
			float y = top + k * rowH - scroll;
			if (y + rowH < top || y > bottom) {
				continue;
			}
			if (k == sel) {
				uiRound(30, y, SCREEN_W - 60, rowH - 6, 10, uiAlpha(C_YELLOW, 0.16f));
			}
			uiTextWrap(46, y + 29, 520, 16, 20, C_CHALK, o->desc, 1);
			char v[96];
			snprintf(v, sizeof(v), "◀ %s ▶", o->labels[o->current]);
			uiText(SCREEN_W - 50 - uiTextW(16, v), y + 29, 16, o->current == o->def ? C_SOFT : C_GREEN, v);
			if (uiTapped(30, y, SCREEN_W - 60, rowH - 6) && fabsf(IN.y - IN.sy) < 10) {
				sel = k;
				roStep(sOpts, vis[k], IN.x > SCREEN_W / 2 ? 1 : -1);
			}
		}
		uiNoClip();
		if (!nv) {
			uiTextCentered(SCREEN_W / 2, 280, 17, C_SOFT, "This emulator has no settings.");
		} else if (sel < nv && sOpts->opt[vis[sel]].info[0]) {
			uiTextWrap(40, 514, 760, 13, 16, C_MUTED, sOpts->opt[vis[sel]].info, 1);
		}
		bool reset = uiButton(SCREEN_W - 200, 20, 170, 42, "Defaults", NULL, C_RED, false);
		uiText(40, 536, 13, C_SOFT, "◀ ▶ change    ○ back");
		uiFrameEnd();
		if (reset) {
			roReset(sOpts);
		}
		if (uiBack()) {
			return;
		}
	}
}

static void modelDownloadRun(void);

enum MenuChoice { M_RESUME, M_QUIT };

static enum MenuChoice menuRun(void) {
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
		drawGame(0.25f);
		uiJp(60, 60, 26, sSys ? sSys->color : C_CHALK, sSys ? sSys->jp : "");
		uiTextWrap(60, 90, 840, 15, 20, C_SOFT, sGameName, 1);
		int pick = -1;
		char rw[48], fs[48];
		snprintf(rw, sizeof(rw), "Rewind: %s", sRewindOn ? (sRewind ? "on (hold the back)" : "on") : "off");
		snprintf(fs, sizeof(fs), "Frame skip: %s", sAutoSkip ? "auto" : "off");
		const char* items[] = { "Resume",         sStudy ? "Full screen" : "Study layout", "Save state",  "Load state",
			                    "Game settings",  rw,                                       fs,            "Teach Lingo a line",
			                    "Look up a word", "Kaku practice",                          sVoiceSystem ? "Get better speech model" : "Kaku lessons",
			                    "Quit to Lingo" };
		int count = (int) (sizeof(items) / sizeof(items[0]));
		for (int i = 0; i < count; ++i) {
			float x = 60 + (i % 2) * 430, y = 106 + (i / 2) * 68;
			if (uiButton(x, y, 410, 56, items[i], NULL, i == count - 1 ? C_RED : C_YELLOW, false)) {
				pick = i;
			}
		}
		uiText(60, 536, 13, C_SOFT, "○ Resume    Start + Select opens this menu");
		uiFrameEnd();
		if (uiBack()) {
			return M_RESUME;
		}
		switch (pick) {
		case 0:
			return M_RESUME;
		case 1:
			sStudy = !sStudy;
			return M_RESUME;
		case 2:
			toastLoop(stateIo(true) ? "State saved" : "This game can't save a state");
			return M_RESUME;
		case 3:
			toastLoop(stateIo(false) ? "State loaded" : "No saved state yet");
			return M_RESUME;
		case 4:
			optionsRun();
			break;
		case 5:
			sRewindOn = !sRewindOn;
			settingsSave();
			rewindSetup();
			break;
		case 6:
			sAutoSkip = !sAutoSkip;
			settingsSave();
			break;
		case 7:
			lingoTextTeachRun();
			return M_RESUME;
		case 8:
			lingoLookupRun();
			return M_RESUME;
		case 9:
			kakuRun();
			return M_RESUME;
		case 10:
			if (sVoiceSystem) {
				modelDownloadRun();
			} else {
				kakuLessonsRun();
			}
			return M_RESUME;
		case 11:
			return M_QUIT;
		default:
			break;
		}
	}
}

static void fatal(const char* title, const char* body) {
	note("fatal: %s: %s", title, body);
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(SCREEN_W / 2, 200, 24, C_RED, title);
		uiTextWrap(140, 250, 680, 16, 24, C_CHALK, body, 8);
		uiTextCentered(SCREEN_W / 2, 500, 15, C_SOFT, "Tap to go back to Lingo");
		uiFrameEnd();
		if (IN.released || uiBack() || uiOk()) {
			sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
			sceKernelExitProcess(0);
		}
	}
}

/* ---- voiced lines ---- */

/* Sample numbers of recent frames, to find the audio that went with a line. */
#define TIMELINE 1024
static struct {
	double t;
	uint64_t sample;
} sTimeline[TIMELINE];
static int sTimeHead;

static void timelineMark(void) {
	sTimeline[sTimeHead].t = uiTime();
	sTimeline[sTimeHead].sample = sVoice.written;
	sTimeHead = (sTimeHead + 1) % TIMELINE;
}

static uint64_t sampleAt(double t) {
	uint64_t best = sVoice.written;
	double bestDt = 1e9;
	for (int i = 0; i < TIMELINE; ++i) {
		if (sTimeline[i].t <= 0) {
			continue;
		}
		double d = fabs(sTimeline[i].t - t);
		if (d < bestDt) {
			bestDt = d;
			best = sTimeline[i].sample;
		}
	}
	return best;
}

double lingoPanelCurrentArrived(void);

/* The audio for the line on show (from just before it appeared, up to 12 s),
 * or the last 8 s when there is no recent line. */
static size_t lineAudio(int16_t* out, size_t max) {
	double arrived = lingoPanelCurrentArrived();
	double now = uiTime();
	uint64_t from, to = sVoice.written;
	uint64_t rate = (uint64_t) sVoice.rate;
	if (arrived > 0 && now - arrived < 20) {
		from = sampleAt(arrived - 0.4);
		uint64_t limit = from + rate * 12;
		to = to < limit ? to : limit;
	} else {
		from = to > rate * 8 ? to - rate * 8 : 0;
	}
	uint64_t start;
	return vcCopy(&sVoice, from, to, out, max, &start);
}

static void playAudio(bool slow) {
	size_t max = (size_t) (sVoice.rate * 14);
	int16_t* seg = malloc(max * sizeof(int16_t));
	if (!seg) {
		return;
	}
	size_t n = lineAudio(seg, max);
	if (n < sVoice.rate * 0.2) {
		free(seg);
		toastLoop("Nothing to replay yet");
		return;
	}
	int16_t* use = seg;
	if (slow) {
		size_t smax = (size_t) (n / 0.7) + 4096;
		int16_t* s2 = malloc(smax * sizeof(int16_t));
		if (s2) {
			n = vcStretch(seg, n, sVoice.rate, 0.7, s2, smax);
			use = s2;
		}
	}
	size_t omax = (size_t) (n * 48000.0 / sVoice.rate) + 2048;
	int16_t* mono = malloc(omax * sizeof(int16_t));
	size_t m = mono ? vcResample(use, n, sVoice.rate, mono, omax, 48000) : 0;
	if (use != seg) {
		free(use);
	}
	free(seg);
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
	if (port < 0 || !mono) {
		free(mono);
		toastLoop("Couldn't play the line");
		return;
	}
	static int16_t buf[1024 * 2];
	size_t pos = 0;
	while (pos < m) {
		for (size_t i = 0; i < 1024; ++i) {
			int16_t s = pos + i < m ? mono[pos + i] : 0;
			buf[i * 2] = buf[i * 2 + 1] = s;
		}
		pos += 1024;
		sceAudioOutOutput(port, buf);
		/* a frame now and then so the screen says what's happening */
		uiPollInput();
		uiFrameBegin();
		uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
		drawGame(0.35f);
		uiTextCentered(SCREEN_W / 2, SCREEN_H / 2 - 10, 22, C_PINK, slow ? "▶ Playing the line slowly" : "▶ Playing the line");
		uiRound(SCREEN_W / 2 - 200, SCREEN_H / 2 + 14, 400, 8, 4, C_SHEET);
		uiRound(SCREEN_W / 2 - 200, SCREEN_H / 2 + 14, 400 * (float) pos / m, 8, 4, C_PINK);
		uiTextCentered(SCREEN_W / 2, SCREEN_H / 2 + 50, 14, C_SOFT, "Tap to stop");
		uiFrameEnd();
		if (IN.released || uiBack()) {
			break;
		}
	}
	sceAudioOutReleasePort(port);
	free(mono);
}

struct Job {
	int16_t* pcm;
	int n;
	char prompt[400];
	char out[1024];
	volatile int progress;
	volatile int done;
	int result;
};

static void* transcribeThread(void* arg) {
	struct Job* j = arg;
	j->result = lingoWhisperTranscribe(j->pcm, j->n, j->prompt, j->out, sizeof(j->out), &j->progress);
	j->done = 1;
	return NULL;
}

static void transcribe(void) {
	if (!lingoWhisperAvailable()) {
		toastLoop("No speech model on this Vita");
		return;
	}
	size_t max = (size_t) (sVoice.rate * 14);
	int16_t* seg = malloc(max * sizeof(int16_t));
	struct Job* j = calloc(1, sizeof(*j));
	if (!seg || !j) {
		free(seg);
		free(j);
		return;
	}
	size_t n = lineAudio(seg, max);
	j->pcm = malloc((n / 2 + 16000) * sizeof(int16_t));
	j->n = j->pcm ? (int) vcResample(seg, n, sVoice.rate, j->pcm, n / 2 + 16000, 16000) : 0;
	free(seg);
	if (j->n < 8000) {
		free(j->pcm);
		free(j);
		toastLoop("Not enough speech to listen to");
		return;
	}
	/* prime it with the game's recent text */
	const struct LingoText* lt = lingoPanelCurrentText();
	if (lt && lt->len) {
		lingoUtf8FromCodepoints(lt->cp, lt->len < 120 ? lt->len : 120, j->prompt, sizeof(j->prompt));
	}
	pthread_t th;
	bool threaded = pthread_create(&th, NULL, transcribeThread, j) == 0;
	if (!threaded) {
		transcribeThread(j);
	}
	double start = uiTime();
	while (!j->done) {
		uiPollInput();
		uiFrameBegin();
		uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
		drawGame(0.35f);
		float t = (float) (uiTime() - start);
		uiTextCentered(SCREEN_W / 2, SCREEN_H / 2 - 10, 22, C_PINK, "Listening to the line…");
		uiRound(SCREEN_W / 2 - 200, SCREEN_H / 2 + 14, 400, 8, 4, C_SHEET);
		float p = j->progress / 100.0f;
		uiRound(SCREEN_W / 2 - 200, SCREEN_H / 2 + 14, 400 * (p > 0.05f ? p : 0.05f + 0.03f * sinf(t * 3)), 8, 4, C_PINK);
		char s[64];
		snprintf(s, sizeof(s), "%.0f s  ·  offline speech recognition", t);
		uiTextCentered(SCREEN_W / 2, SCREEN_H / 2 + 50, 14, C_SOFT, s);
		uiFrameEnd();
	}
	if (threaded) {
		pthread_join(th, NULL);
	}
	lingoWhisperUnload(); /* give the memory back to the game */
	note("transcribe: %d samples -> %d: %s", j->n, j->result, j->out);
	if (j->result > 0) {
		uint32_t cp[LINGO_MAX_TEXT];
		int k = lingoUtf8Decode(j->out, cp, LINGO_MAX_TEXT);
		lingoPanelAddText(cp, k);
	} else {
		toastLoop(j->result == 0 ? "No speech heard" : j->out);
	}
	free(j->pcm);
	free(j);
}

/* ---- a better speech model, downloaded on the Vita ---- */

#define BASE_MODEL_URL "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q5_1.bin"
#define BASE_MODEL_PATH DATA "/models/ggml-base-q5_1.bin"

struct Download {
	volatile float progress;
	volatile int cancel, done;
	bool ok;
};

static void* downloadThread(void* arg) {
	struct Download* d = arg;
	d->ok = lingoNetDownload(BASE_MODEL_URL, BASE_MODEL_PATH, &d->progress, &d->cancel);
	d->done = 1;
	return NULL;
}

static void modelDownloadRun(void) {
	char ip[32];
	if (!lingoNetUp(ip, sizeof(ip))) {
		toastLoop("Connect to Wi-Fi first");
		return;
	}
	sceIoMkdir(DATA "/models", 0777);
	static struct Download d;
	memset((void*) &d, 0, sizeof(d));
	pthread_t th;
	if (pthread_create(&th, NULL, downloadThread, &d) != 0) {
		return;
	}
	while (!d.done) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(SCREEN_W / 2, 200, 22, C_PINK, "Downloading the better speech model");
		uiTextCentered(SCREEN_W / 2, 232, 14, C_SOFT, "Whisper base (about 57 MB, MIT licence). Recognises speech noticeably better, a little slower.");
		uiRound(SCREEN_W / 2 - 250, 270, 500, 12, 6, C_SHEET);
		uiRound(SCREEN_W / 2 - 250, 270, 500 * d.progress, 12, 6, C_PINK);
		uiTextCentered(SCREEN_W / 2, 320, 14, C_SOFT, "○ Cancel");
		uiFrameEnd();
		if (uiBack()) {
			d.cancel = 1;
		}
	}
	pthread_join(th, NULL);
	lingoWhisperUnload(); /* the next transcription picks the bigger model */
	toastLoop(d.ok ? "Better speech model installed" : d.cancel ? "Download cancelled" : lingoNetLastError());
}

/* ---- loading ---- */

static bool hasExt(const char* path, const char* ext) {
	size_t l = strlen(path), e = strlen(ext);
	return l > e && !strcasecmp(path + l - e, ext);
}

static bool writeTemp(const char* name, const uint8_t* data, size_t size, char* out, size_t outLen) {
	sceIoMkdir(TMP_DIR, 0777);
	/* only one game's worth of temporary files */
	SceUID d = sceIoDopen(TMP_DIR);
	if (d >= 0) {
		SceIoDirent e;
		while (sceIoDread(d, &e) > 0) {
			char p[512];
			snprintf(p, sizeof(p), TMP_DIR "/%s", e.d_name);
			sceIoRemove(p);
		}
		sceIoDclose(d);
	}
	const char* base = strrchr(name, '/');
	snprintf(out, outLen, TMP_DIR "/%s", base ? base + 1 : name);
	FILE* f = fopen(out, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	fclose(f);
	return ok;
}

static bool isVoiceSystem(const char* id) {
	return id && (!strcmp(id, "ps1") || !strcmp(id, "ss") || !strcmp(id, "pce") || !strcmp(id, "md"));
}

static void collectRegions(struct LingoRegions* rs) {
	lingoRegionsClear(rs);
	lingoRegionsAdd(rs, "ram", retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
	lingoRegionsAdd(rs, "vram", retro_get_memory_data(RETRO_MEMORY_VIDEO_RAM), retro_get_memory_size(RETRO_MEMORY_VIDEO_RAM));
	for (int i = 0; i < sNMaps; ++i) {
		char name[16];
		snprintf(name, sizeof(name), "map%d", i);
		lingoRegionsAdd(rs, name, sMaps[i].ptr, sMaps[i].len);
	}
}

/* ---- main ---- */

int main(int argc, char* argv[]) {
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
	sceIoMkdir(DATA, 0777);
	sceIoMkdir(SYSTEM_DIR, 0777);
	sceIoMkdir(SAVE_DIR, 0777);
	sceIoMkdir(STATE_DIR, 0777);
	sceIoMkdir(OPTION_DIR, 0777);
	sLog = fopen(DATA "/core.log", "w");
	vita2d_init();
	vita2d_set_vblank_wait(1);
	uiInit();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
	settingsLoad();

	if (argc < 3 || !argv[1] || !argv[2]) {
		fatal("Nothing to play", "Start games from Lingo's title screen.");
	}
	snprintf(sGamePath, sizeof(sGamePath), "%s", argv[1]);
	sSys = lingoSystemById(argv[2]);
	const char* base = strrchr(sGamePath, '/');
	base = base ? base + 1 : strchr(sGamePath, ':') ? strchr(sGamePath, ':') + 1 : sGamePath;
	snprintf(sBaseName, sizeof(sBaseName), "%s", base);
	snprintf(sGameName, sizeof(sGameName), "%s", base);
	char* dot = strrchr(sGameName, '.');
	if (dot) {
		*dot = 0;
	}

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	retro_get_system_info(&info);
	sOpts = malloc(sizeof(*sOpts));
	{
		char op[256];
		snprintf(op, sizeof(op), OPTION_DIR "/%s.opt", info.library_name ? info.library_name : "core");
		roInit(sOpts, op);
	}
	note("Lingo front end: %s %s, game %s", info.library_name, info.library_version, sGamePath);

	retro_set_environment(environment);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);
	retro_init();

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = sGamePath;
	/* zipped games: FBNeo wants the zip itself; everything else gets what's inside */
	static char unzipped[512];
	bool zipped = hasExt(sGamePath, ".zip") && sSys && strcmp(sSys->id, "neogeo") && retroZipIs(sGamePath);
	if (zipped) {
		uint8_t* data;
		size_t size;
		char inner[256], why[200];
		if (!retroZipExtract(sGamePath, sSys->exts, &data, &size, inner, sizeof(inner), why, sizeof(why))) {
			fatal("Couldn't open the zip", why);
		}
		note("unzipped %s (%u bytes)", inner, (unsigned) size);
		if (info.need_fullpath) {
			if (!writeTemp(inner, data, size, unzipped, sizeof(unzipped))) {
				fatal("Couldn't unzip the game", "There may not be enough space on the memory card.");
			}
			free(data);
			game.path = unzipped;
		} else {
			sContent = data;
			sContentSize = size;
			game.data = data;
			game.size = size;
			game.path = sGamePath;
		}
	} else if (!info.need_fullpath) {
		FILE* f = fopen(sGamePath, "rb");
		if (!f) {
			fatal("Couldn't open the game", sGamePath);
		}
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		sContent = malloc(size > 0 ? size : 1);
		if (!sContent || fread(sContent, 1, size, f) != (size_t) size) {
			fclose(f);
			fatal("Couldn't read the game", "The file may be too large, or the memory card may be busy.");
		}
		fclose(f);
		sContentSize = (size_t) size;
		game.data = sContent;
		game.size = size;
	}
	if (!retro_load_game(&game)) {
		char why[512];
		snprintf(why, sizeof(why),
		         "%s could not start %s. Check the file is a %s game%s%s. Details are in ux0:data/lingo/core.log.",
		         info.library_name, sGameName, sSys ? sSys->en : "supported", sSys && sSys->note ? ", and: " : "",
		         sSys && sSys->note ? sSys->note : "");
		fatal("That game didn't start", why);
	}
	retro_get_system_av_info(&sAv);
	double rate = sAv.timing.sample_rate > 0 ? sAv.timing.sample_rate : 44100;
	raInit(&sAudio, rate, 48000);
	vcInit(&sVoice, rate);
	sAudioRun = true;
	sAudioThread = sceKernelCreateThread("lingo-audio", audioThread, 0x10000100, 0x10000, 0, 0, NULL);
	if (sAudioThread >= 0) {
		sceKernelStartThread(sAudioThread, 0, NULL);
	}
	loadSram();
	rewindSetup();

	char key[300];
	snprintf(key, sizeof(key), "%s-%s", sSys ? sSys->id : "retro", sGameName);
	lingoPanelGameLoaded(NULL, key);
	lingoPanelSetSource(sSys ? sSys->id : "", sBaseName, (const uint8_t*) sContent, sContentSize);
	sVoiceSystem = isVoiceSystem(sSys ? sSys->id : NULL);
	lingoPanelSetVoice(sVoiceSystem && sVoice.ring);
	lingoShotGame(sGamePath);

	struct SaveWatch watch;
	memset(&watch, 0, sizeof(watch));
	struct LingoMem none;
	memset(&none, 0, sizeof(none));
	struct LingoRegions rs;
	int frame = 0;
	int skipped = 0;
	bool quit = false;
	while (!quit) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		/* Start + Select opens the menu without reaching the game */
		bool menu = (pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT);
		/* holding the rear touch pad rewinds */
		SceTouchData back;
		memset(&back, 0, sizeof(back));
		sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
		bool rewinding = sRewind && back.reportNum > 0;
		if (rewinding) {
			if (rwPop(sRewind, sRwState)) {
				retro_unserialize(sRwState, sRwSize);
			}
			retro_run(); /* draws the restored frame */
		} else {
			retro_run();
			if (sRewind && ++frame % sRwEvery == 0 && retro_serialize(sRwState, sRwSize)) {
				rwPush(sRewind, sRwState);
			}
		}
		timelineMark();

		/* about once a second: has the game finished writing a save? */
		if (frame % 60 == 0) {
			if (swCheck(&watch, retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), retro_get_memory_size(RETRO_MEMORY_SAVE_RAM))) {
				writeSram();
				lingoPanelFrame(&none, 1); /* dirty... */
			}
		}
		collectRegions(&rs);
		lingoPanelSetRegions(&rs);
		if (sTex && sFrameW) {
			lingoTextSetFrame(vita2d_texture_get_datap(sTex), (int) sFrameW, (int) sFrameH, (int) vita2d_texture_get_stride(sTex),
			                  sTexFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 1 : sTexFormat == RETRO_PIXEL_FORMAT_RGB565 ? 2 : 3);
		}
		lingoPanelFrame(&none, 0); /* ...then clean: a finished save */

		/* behind? skip showing a frame or two (the game itself keeps running) */
		if (sAutoSkip && !menu && !rewinding && skipped < 2 && raFill(&sAudio) < RA_RING / 8 && frame > 120) {
			++skipped;
			continue;
		}
		skipped = 0;

		uiFrameBegin();
		enum LingoPanelAction act = LPA_NONE;
		if (sStudy) {
			drawGame(1);
			float x, y, w, h;
			gameRect(&x, &y, &w, &h);
			act = lingoRetroPanelDraw(x, y, w, h, sSys ? sSys->jp : "", sGameName);
		} else {
			uiRect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
			drawGame(1);
			uiPollInput();
			if (IN.released) {
				act = LPA_MENU; /* in full screen, a tap opens the menu */
			}
		}
		if (rewinding) {
			uiRound(SCREEN_W / 2 - 70, 20, 140, 34, 12, C_SHADE);
			uiTextCentered(SCREEN_W / 2, 43, 16, C_YELLOW, "◀◀ Rewind");
		}
		uiFrameEnd();

		if (menu) {
			act = LPA_MENU;
			/* wait for the buttons to be let go */
			do {
				sceCtrlPeekBufferPositive(0, &pad, 1);
			} while (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT));
		}
		if (lingoCheckpointDue()) {
			lingoCheckpointRun();
		}
		switch (act) {
		case LPA_LOOKUP:
			lingoLookupRun();
			break;
		case LPA_KAKU:
			kakuRun();
			break;
		case LPA_TEACH:
			lingoTextTeachRun();
			break;
		case LPA_HEAR:
			playAudio(false);
			break;
		case LPA_HEAR_SLOW:
			playAudio(true);
			break;
		case LPA_TRANSCRIBE:
			transcribe();
			break;
		case LPA_FIX:
			lingoPanelFixRun();
			break;
		case LPA_MENU:
			if (menuRun() == M_QUIT) {
				quit = true;
			}
			break;
		default:
			break;
		}
	}

	writeSram();
	sAudioRun = false;
	if (sAudioThread >= 0) {
		sceKernelWaitThreadEnd(sAudioThread, NULL, NULL);
	}
	retro_unload_game();
	retro_deinit();
	lingoPanelGameUnloaded();
	uiDeinit();
	if (sLog) {
		fclose(sLog);
	}
	sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
	sceKernelExitProcess(0);
	return 0;
}
