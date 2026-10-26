/* Lingo's libretro front end for the Vita. One copy is linked with each core
 * (app0:cores/<core>.self). Started from Lingo's title screen with
 * argv[1] = game path and argv[2] = console id; returns to it on quit.
 *
 * Plays the game, and around it: the study side panel (lookup, Kaku),
 * checkpoints when the game saves, save states, and a small menu. */
#include "libretro.h"
#include "retro-util.h"

#include "../src/psp2-lingo-systems.h"
#include "../src/psp2-lingo-ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
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

static const struct LingoSystem* sSys;
static char sGamePath[1024];
static char sGameName[256];
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
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable*) data)->value = NULL; /* every core option at its default */
		return false;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool*) data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned*) data = 0;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
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
}

static int16_t sOne[64 * 2];
static size_t sOneN;

static size_t audioBatch(const int16_t* data, size_t frames) {
	raPush(&sAudio, data, frames);
	return frames;
}

static void audioSample(int16_t l, int16_t r) {
	sOne[sOneN * 2] = l;
	sOne[sOneN * 2 + 1] = r;
	if (++sOneN == 64) {
		raPush(&sAudio, sOne, sOneN);
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

/* ---- screens ---- */

static void gameRect(float* x, float* y, float* w, float* h) {
	float aspect = sAv.geometry.aspect_ratio > 0 ? sAv.geometry.aspect_ratio
	                                             : (sFrameH ? (float) sFrameW / sFrameH : 4.0f / 3.0f);
	float areaW = sStudy ? 600 : SCREEN_W, areaH = sStudy ? 520 : SCREEN_H;
	float ox = sStudy ? 12 : 0, oy = sStudy ? 12 : 0;
	float hh = areaH, ww = hh * aspect;
	if (ww > areaW) {
		ww = areaW;
		hh = ww / aspect;
	}
	*x = ox + (sStudy ? 0 : (areaW - ww) / 2);
	*y = oy + (areaH - hh) / 2;
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

enum MenuChoice { M_RESUME, M_QUIT };

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

static enum MenuChoice menuRun(void) {
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiRect(0, 0, SCREEN_W, SCREEN_H, C_BOARD);
		drawGame(0.25f);
		uiJp(60, 70, 26, sSys ? sSys->color : C_CHALK, sSys ? sSys->jp : "");
		uiTextWrap(60, 102, 840, 15, 20, C_SOFT, sGameName, 1);
		int pick = -1;
		const char* items[] = { "Resume", sStudy ? "Full screen" : "Study layout", "Save state", "Load state",
			                    "Look up a word", "Kaku practice", "Quit to Lingo" };
		for (int i = 0; i < 7; ++i) {
			float x = 60 + (i % 2) * 430, y = 130 + (i / 2) * 82;
			if (uiButton(x, y, 410, 66, items[i], NULL, i == 6 ? C_RED : C_YELLOW, false)) {
				pick = i;
			}
		}
		uiText(60, 534, 14, C_SOFT, "○ Resume    Start + Select opens this menu");
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
			lingoLookupRun();
			return M_RESUME;
		case 5:
			kakuRun();
			return M_RESUME;
		case 6:
			return M_QUIT;
		default:
			break;
		}
	}
}

static void fatal(const char* title, const char* body) {
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(SCREEN_W / 2, 200, 24, C_RED, title);
		uiTextWrap(140, 250, 680, 16, 24, C_CHALK, body, 8);
		uiTextCentered(SCREEN_W / 2, 500, 15, C_SOFT, "Tap to go back to Lingo");
		uiFrameEnd();
		if (IN.released || uiBack() || uiOk()) {
			sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
			sceKernelExitProcess(0);
		}
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
	sLog = fopen(DATA "/core.log", "w");
	vita2d_init();
	vita2d_set_vblank_wait(1);
	uiInit();
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	if (argc < 3 || !argv[1] || !argv[2]) {
		fatal("Nothing to play", "Start games from Lingo's title screen.");
	}
	snprintf(sGamePath, sizeof(sGamePath), "%s", argv[1]);
	sSys = lingoSystemById(argv[2]);
	const char* base = strrchr(sGamePath, '/');
	base = base ? base + 1 : strchr(sGamePath, ':') ? strchr(sGamePath, ':') + 1 : sGamePath;
	snprintf(sGameName, sizeof(sGameName), "%s", base);
	char* dot = strrchr(sGameName, '.');
	if (dot) {
		*dot = 0;
	}

	retro_set_environment(environment);
	retro_set_video_refresh(video);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);
	retro_init();
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	retro_get_system_info(&info);

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = sGamePath;
	if (!info.need_fullpath) {
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
	raInit(&sAudio, sAv.timing.sample_rate > 0 ? sAv.timing.sample_rate : 44100, 48000);
	sAudioRun = true;
	sAudioThread = sceKernelCreateThread("lingo-audio", audioThread, 0x10000100, 0x10000, 0, 0, NULL);
	if (sAudioThread >= 0) {
		sceKernelStartThread(sAudioThread, 0, NULL);
	}
	loadSram();

	char key[300];
	snprintf(key, sizeof(key), "%s-%s", sSys ? sSys->id : "retro", sGameName);
	lingoPanelGameLoaded(NULL, key);

	struct SaveWatch watch;
	memset(&watch, 0, sizeof(watch));
	struct LingoMem none;
	memset(&none, 0, sizeof(none));
	int frame = 0;
	bool quit = false;
	while (!quit) {
		/* Start + Select opens the menu without reaching the game */
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		bool menu = (pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT);

		retro_run();

		/* about once a second: has the game finished writing a save? */
		if (++frame % 60 == 0) {
			if (swCheck(&watch, retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), retro_get_memory_size(RETRO_MEMORY_SAVE_RAM))) {
				writeSram();
				lingoPanelFrame(&none, 1); /* dirty... */
			}
		}
		lingoPanelFrame(&none, 0); /* ...then clean: a finished save */

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
	sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
	sceKernelExitProcess(0);
	return 0;
}
