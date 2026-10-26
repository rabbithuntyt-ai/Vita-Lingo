/* Lingo: the "send games from your phone" screen. Starts Wi-Fi, runs the
 * upload server and shows a QR code for the phone to scan. */
#include "psp2-lingo-systems.h"
#include "psp2-lingo-ui.h"
#include "psp2-lingo-upload.h"
#include "psp2-qrcodegen.h"
#include "psp2-lingo-net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#define GAMES_ROOT "ux0:data/lingo/games"

static void drawQr(const char* text, float x, float y, float size) {
	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
	                          qrcodegen_Mask_AUTO, true)) {
		return;
	}
	int n = qrcodegen_getSize(qr);
	float m = size / (n + 4); /* quiet zone of two modules */
	uiRound(x, y, size, size, 12, RGBA(0xf7, 0xf9, 0xf2, 0xff));
	for (int j = 0; j < n; ++j) {
		for (int i = 0; i < n; ++i) {
			if (qrcodegen_getModule(qr, i, j)) {
				uiRect(x + (i + 2) * m, y + (j + 2) * m, m + 0.5f, m + 0.5f, C_BOARD);
			}
		}
	}
}

static void formatSize(uint64_t b, char* out, size_t n) {
	if (b >= 1024 * 1024) {
		snprintf(out, n, "%.1f MB", b / 1048576.0);
	} else {
		snprintf(out, n, "%u KB", (unsigned) (b / 1024));
	}
}

void lingoUploadRun(void) {
	uiInit();
	static struct LingoUploadSystem sys[32];
	int ns = 0;
	for (int i = 0; i < LINGO_SYSTEM_COUNT && ns < 30; ++i) {
		static char labels[32][96];
		snprintf(labels[ns], sizeof(labels[ns]), "%s (%s)", LINGO_SYSTEMS[i].en, LINGO_SYSTEMS[i].jp);
		sys[ns] = (struct LingoUploadSystem) { LINGO_SYSTEMS[i].id, labels[ns], LINGO_SYSTEMS[i].exts, NULL };
		++ns;
	}
	/* not games: a game's character table, and a bigger speech model */
	sys[ns++] = (struct LingoUploadSystem) { "tables", "Text table for a game (.tbl)", ".tbl", "ux0:data/lingo/tables" };
	sys[ns++] = (struct LingoUploadSystem) { "models", "Speech model (whisper.cpp ggml .bin)", ".bin", "ux0:data/lingo/models" };
	char ip[32] = "";
	bool online = lingoNetUp(ip, sizeof(ip));
	char token[12];
	unsigned seed = (unsigned) sceKernelGetProcessTimeWide() * 2654435761u;
	static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
	for (int i = 0; i < 8; ++i) {
		seed = seed * 1664525 + 1013904223;
		token[i] = alphabet[(seed >> 8) % (sizeof(alphabet) - 1)];
	}
	token[8] = 0;
	struct LingoUpload* up = online ? lingoUploadStart(8080, GAMES_ROOT, token, sys, ns) : NULL;
	if (online && !up) {
		up = lingoUploadStart(0, GAMES_ROOT, token, sys, ns); /* 8080 busy: any port */
	}
	char url[96] = "";
	if (up) {
		snprintf(url, sizeof(url), "http://%s:%d/?k=%s", ip, lingoUploadPort(up), token);
	}
	int lastDone = 0;
	double lastCheck = 0;
	while (true) {
		uiPollInput();
		double now = uiTime();
		static double last;
		float dt = last ? (float) (now - last) : 0;
		last = now;
		uiDustUpdate(dt > 0.1f ? 0.1f : dt);
		if (!up && now - lastCheck > 2) {
			/* Wi-Fi may come up while the screen is open */
			lastCheck = now;
			online = lingoNetUp(ip, sizeof(ip));
			if (online) {
				up = lingoUploadStart(8080, GAMES_ROOT, token, sys, ns);
				if (up) {
					snprintf(url, sizeof(url), "http://%s:%d/?k=%s", ip, lingoUploadPort(up), token);
				}
			}
		}
		struct LingoUploadStatus st;
		memset(&st, 0, sizeof(st));
		if (up) {
			lingoUploadGetStatus(up, &st);
			if (st.done > lastDone) {
				lastDone = st.done;
				uiDust(700, 300, C_GREEN, 30);
			}
		}
		uiFrameBegin();
		uiChalkBackground();
		uiJp(32, 56, 30, C_CHALK, "ゲームを おくる");
		uiText(32, 86, 15, C_SOFT, "Send game backups from your phone. Use dumps of games you own.");
		if (!up) {
			uiTextWrap(32, 170, 560, 20, 30, C_YELLOW, "Connect the Vita to Wi-Fi first (Settings → Network).", 3);
			uiTextWrap(32, 250, 560, 16, 24, C_SOFT,
			           "Your phone needs to be on the same Wi-Fi network. This screen will notice once you're connected.", 4);
		} else {
			drawQr(url, 32, 110, 300);
			float x = 360;
			uiText(x, 140, 18, C_CHALK, "1. Scan the code with your phone's camera.");
			uiText(x, 176, 18, C_CHALK, "2. Pick game files and the console they're for.");
			uiText(x, 212, 18, C_CHALK, "3. Tap Send. They appear here and in Play a game.");
			uiText(x, 256, 14, C_SOFT, "Or type this into the phone's browser:");
			uiText(x, 282, 17, C_YELLOW, url);
			uiText(x, 308, 13, C_SOFT, "Phone and Vita must be on the same Wi-Fi. The code changes each time.");
			if (st.active) {
				char a[32], b[32], line[200];
				formatSize(st.received, a, sizeof(a));
				formatSize(st.total, b, sizeof(b));
				snprintf(line, sizeof(line), "Receiving %s  ·  %s of %s", st.name, a, b);
				uiTextWrap(x, 350, SCREEN_W - x - 32, 16, 22, C_CHALK, line, 1);
				float f = st.total ? (float) st.received / st.total : 0;
				uiRound(x, 362, SCREEN_W - x - 32, 14, 7, C_SHEET);
				uiRound(x, 362, (SCREEN_W - x - 32) * f + 14, 14, 7, C_GREEN);
			} else if (st.error[0]) {
				uiTextWrap(x, 356, SCREEN_W - x - 32, 15, 21, C_RED, st.error, 2);
			}
			float y = 410;
			if (st.nRecent) {
				uiText(x, y, 14, C_SOFT, "Arrived:");
			}
			for (int i = st.nRecent - 1; i >= 0 && y < 520; --i) {
				y += 22;
				char line[200];
				snprintf(line, sizeof(line), "✓ %s", st.recent[i]);
				uiTextWrap(x, y, SCREEN_W - x - 32, 15, 20, C_GREEN, line, 1);
			}
		}
		uiText(32, 536, 14, C_SOFT, "○ Done");
		uiDustDraw();
		uiFrameEnd();
		if (uiBack() && !st.active) {
			break;
		}
	}
	lingoUploadStop(up);
}
