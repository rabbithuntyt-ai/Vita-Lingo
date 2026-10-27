/* Vita Lab: installs the remote-control plugins (a fork of vitacompanion with
 * screen capture, performance stats and uninstall added) into taiHEN, shows
 * the address the PC app connects to, and takes them out again.
 *
 * Changes to taiHEN's config.txt are made line by line: only lines naming
 * vitacompanion are added or removed, and the first change keeps a backup
 * (config.txt.vitalab-backup) next to it. */
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include "config.h"


#define WHITE RGBA8(0xEE, 0xEE, 0xE8, 0xFF)
#define SOFT RGBA8(0x9A, 0xA8, 0xA0, 0xFF)
#define GREEN RGBA8(0x7C, 0xD9, 0x9A, 0xFF)
#define RED RGBA8(0xFF, 0x8A, 0x7A, 0xFF)
#define YELLOW RGBA8(0xF4, 0xD3, 0x6B, 0xFF)
#define BG RGBA8(0x1B, 0x26, 0x24, 0xFF)
#define CARD RGBA8(0x26, 0x34, 0x31, 0xFF)

static vita2d_pgf* font;
static char message[256] = "";
static uint32_t messageColor = SOFT;

static bool exists(const char* path) {
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

/* taiHEN reads ux0:tai/config.txt when it exists, otherwise ur0:tai/config.txt */
static const char* configPath(void) {
	return exists("ux0:tai/config.txt") ? "ux0:tai/config.txt" : "ur0:tai/config.txt";
}

static char* readAll(const char* path, int* len) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		*len = 0;
		return NULL;
	}
	int size = (int) sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	char* buf = malloc((size_t) size + 1);
	int n = buf ? sceIoRead(fd, buf, (SceSize) size) : -1;
	sceIoClose(fd);
	if (n < 0) {
		free(buf);
		*len = 0;
		return NULL;
	}
	buf[n] = 0;
	*len = n;
	return buf;
}

static bool writeAll(const char* path, const char* data, int len) {
	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s.vitalab-new", path);
	SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) {
		return false;
	}
	bool ok = sceIoWrite(fd, data, (SceSize) len) == len;
	sceIoClose(fd);
	if (!ok) {
		sceIoRemove(tmp);
		return false;
	}
	sceIoRemove(path);
	return sceIoRename(tmp, path) >= 0;
}

static bool copyFile(const char* from, const char* to) {
	int len;
	char* d = readAll(from, &len);
	if (!d) {
		return false;
	}
	bool ok = writeAll(to, d, len);
	free(d);
	return ok;
}

static int configState(void) {
	int len;
	char* c = readAll(configPath(), &len);
	if (!c) {
		return -1;
	}
	bool k = strstr(c, KERNEL_PLUGIN) != NULL, u = strstr(c, USER_PLUGIN) != NULL;
	free(c);
	return k && u ? 1 : 0;
}

static bool setConfig(bool add) {
	const char* path = configPath();
	int len;
	char* c = readAll(path, &len);
	if (!c) {
		snprintf(message, sizeof(message), "Couldn't read %s.", path);
		messageColor = RED;
		return false;
	}
	char backup[256];
	snprintf(backup, sizeof(backup), "%s.vitalab-backup", path);
	if (!exists(backup)) {
		writeAll(backup, c, len);
	}
	int n;
	char* out = labRewriteConfig(c, add, &n);
	bool ok = writeAll(path, out, n);
	free(c);
	free(out);
	if (!ok) {
		snprintf(message, sizeof(message), "Couldn't write %s.", path);
		messageColor = RED;
	}
	return ok;
}

static void install(void) {
	sceIoMkdir("ur0:tai", 0777);
	if (!copyFile("app0:plugins/vitacompanion_kernel.skprx", KERNEL_PLUGIN) ||
	    !copyFile("app0:plugins/vitacompanion.suprx", USER_PLUGIN)) {
		snprintf(message, sizeof(message), "Couldn't copy the plugins to ur0:tai.");
		messageColor = RED;
		return;
	}
	if (setConfig(true)) {
		snprintf(message, sizeof(message), "Installed. Reboot to start remote control.");
		messageColor = GREEN;
	}
}

static void uninstall(void) {
	if (setConfig(false)) {
		sceIoRemove(KERNEL_PLUGIN);
		sceIoRemove(USER_PLUGIN);
		snprintf(message, sizeof(message), "Removed. Reboot to unload the plugins.");
		messageColor = YELLOW;
	}
}

/* is the remote-control plugin answering right now? */
static bool running(char* version, size_t n) {
	int s = sceNetSocket("vitalab_probe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if (s < 0) {
		return false;
	}
	int tmo = 500 * 1000;
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
	SceNetSockaddrIn a;
	memset(&a, 0, sizeof(a));
	a.sin_family = SCE_NET_AF_INET;
	a.sin_port = sceNetHtons(1338);
	sceNetInetPton(SCE_NET_AF_INET, "127.0.0.1", &a.sin_addr);
	bool ok = false;
	if (sceNetConnect(s, (SceNetSockaddr*) &a, sizeof(a)) >= 0 && sceNetSend(s, "version\n", 8, 0) == 8) {
		char buf[128];
		int r = sceNetRecv(s, buf, sizeof(buf) - 1, 0);
		if (r > 0) {
			buf[r] = 0;
			buf[strcspn(buf, "\r\n")] = 0;
			snprintf(version, n, "%s", buf);
			ok = true;
		}
	}
	sceNetSocketClose(s);
	return ok;
}

static bool button(float x, float y, float w, float h, const char* label, uint32_t col, bool focus, bool tapped, float tx,
                   float ty) {
	vita2d_draw_rectangle(x, y, w, h, focus ? RGBA8(0x34, 0x46, 0x42, 0xFF) : CARD);
	vita2d_draw_rectangle(x, y + h - 4, w, 4, col);
	int tw = vita2d_pgf_text_width(font, 1.1f, label);
	vita2d_pgf_draw_text(font, (int) (x + (w - tw) / 2), (int) (y + h / 2 + 9), col, 1.1f, label);
	return tapped && tx >= x && tx < x + w && ty >= y && ty < y + h;
}

int main(void) {
	vita2d_init();
	vita2d_set_clear_color(BG);
	font = vita2d_load_default_pgf();
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	static char netMem[256 * 1024];
	SceNetInitParam np = { netMem, sizeof(netMem), 0 };
	sceNetInit(&np);
	sceNetCtlInit();
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

	int focus = 0;
	uint32_t prevButtons = 0;
	bool wasDown = false;
	char version[128] = "";
	bool live = false;
	int frame = 0;
	while (true) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t pressed = pad.buttons & ~prevButtons;
		prevButtons = pad.buttons;
		SceTouchData touch;
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		bool down = touch.reportNum > 0;
		static float tx, ty;
		if (down) {
			tx = touch.report[0].x / 2.0f;
			ty = touch.report[0].y / 2.0f;
		}
		bool tapped = wasDown && !down;
		wasDown = down;
		if (pressed & SCE_CTRL_DOWN) {
			focus = (focus + 1) % 3;
		}
		if (pressed & SCE_CTRL_UP) {
			focus = (focus + 2) % 3;
		}
		if (frame++ % 120 == 0) {
			live = running(version, sizeof(version));
		}

		char ip[32] = "not on Wi-Fi";
		SceNetCtlInfo info;
		if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0 && info.ip_address[0]) {
			snprintf(ip, sizeof(ip), "%s", info.ip_address);
		}
		int cfg = configState();

		vita2d_start_drawing();
		vita2d_clear_screen();
		vita2d_pgf_draw_text(font, 40, 60, WHITE, 2.0f, "Vita Lab");
		vita2d_pgf_draw_text(font, 40, 92, SOFT, 1.0f, "Remote control, screenshots and performance stats for testing homebrew from a PC.");

		vita2d_draw_rectangle(40, 116, 880, 170, CARD);
		char line[256];
		snprintf(line, sizeof(line), "Plugins in %s: %s", configPath(), cfg == 1 ? "yes" : cfg == 0 ? "no" : "can't read it");
		vita2d_pgf_draw_text(font, 60, 150, cfg == 1 ? GREEN : SOFT, 1.0f, line);
		snprintf(line, sizeof(line), "Running now: %s", live ? version : "no (install, then reboot)");
		vita2d_pgf_draw_text(font, 60, 182, live ? GREEN : SOFT, 1.0f, line);
		snprintf(line, sizeof(line), "This Vita's address: %s", ip);
		vita2d_pgf_draw_text(font, 60, 214, WHITE, 1.0f, line);
		vita2d_pgf_draw_text(font, 60, 246, SOFT, 0.9f,
		                     live ? "On your PC run vitalab.exe; it finds this Vita by itself (same Wi-Fi)."
		                          : "Ports used: 1337 FTP, 1338 commands, 1339 screen, 1340 discovery.");
		vita2d_pgf_draw_text(font, 60, 274, SOFT, 0.85f,
		                     "Only use on a network you trust: anyone on it can control the Vita while this runs.");

		bool b0 = button(40, 310, 420, 56, cfg == 1 ? "Reinstall plugins" : "Install plugins", GREEN, focus == 0, tapped, tx, ty) ||
		          (focus == 0 && (pressed & SCE_CTRL_CROSS));
		bool b1 = button(40, 378, 420, 56, "Remove plugins", RED, focus == 1, tapped, tx, ty) || (focus == 1 && (pressed & SCE_CTRL_CROSS));
		bool b2 = button(40, 446, 420, 56, "Reboot now", YELLOW, focus == 2, tapped, tx, ty) || (focus == 2 && (pressed & SCE_CTRL_CROSS));
		vita2d_pgf_draw_text(font, 500, 340, SOFT, 0.9f, "If the Vita ever fails to boot after this,");
		vita2d_pgf_draw_text(font, 500, 364, SOFT, 0.9f, "hold L while it starts: taiHEN then skips");
		vita2d_pgf_draw_text(font, 500, 388, SOFT, 0.9f, "all plugins, and you can remove them here.");
		vita2d_pgf_draw_text(font, 500, 430, SOFT, 0.85f, "Built on vitacompanion (MIT) by devnoname120");
		vita2d_pgf_draw_text(font, 500, 452, SOFT, 0.85f, "and contributors.");
		vita2d_pgf_draw_text(font, 40, 530, messageColor, 1.0f, message);
		vita2d_end_drawing();
		vita2d_swap_buffers();

		if (b0) {
			install();
		} else if (b1) {
			uninstall();
		} else if (b2) {
			scePowerRequestColdReset();
		}
	}
	return 0;
}
