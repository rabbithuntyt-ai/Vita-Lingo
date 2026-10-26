/* Lingo: the consoles, the system picker and the ROM browser. Game Boy and
 * GBA go to mGBA in this app; everything else launches the matching
 * libretro core packed alongside it in app0:cores/. */
#include "psp2-lingo-systems.h"
#include "psp2-lingo-net.h"
#include "psp2-lingo-shot.h"
#include "psp2-lingo-ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <vita2d.h>

const struct LingoSystem LINGO_SYSTEMS[] = {
	{ "gba", "ゲームボーイアドバンス", "Game Boy Advance", "Nintendo", NULL, ".gba", NULL, C_BLUE, false },
	{ "gb", "ゲームボーイ", "Game Boy / Color", "Nintendo", NULL, ".gb.gbc", NULL, C_GREEN, false },
	{ "fc", "ファミコン", "Famicom / NES", "Nintendo", "fceumm", ".nes.fds.unf.unif",
	  "Disk System games need disksys.rom", C_RED, false },
	{ "sfc", "スーパーファミコン", "Super Famicom", "Nintendo", "snes9x2005", ".sfc.smc.fig.swc", NULL, C_PINK, false },
	{ "md", "メガドライブ", "Mega Drive · Master System · Game Gear", "Sega", "genesis_plus_gx",
	  ".md.gen.smd.bin.sms.gg.sg.68k.cue.chd", "Mega CD needs its BIOS (bios_CD_J.bin)", C_BLUE, false },
	{ "pce", "PCエンジン", "PC Engine / CD-ROM²", "NEC", "mednafen_pce_fast", ".pce.cue.ccd.chd",
	  "CD games need syscard3.pce", C_YELLOW, false },
	{ "ps1", "プレイステーション", "PlayStation", "Sony", "pcsx_rearmed", ".cue.bin.img.pbp.chd.iso",
	  "Runs without a BIOS; scph5500.bin improves compatibility", C_SOFT, false },
	{ "ws", "ワンダースワン", "WonderSwan / Color", "Bandai", "mednafen_wswan", ".ws.wsc.pc2", NULL, C_GREEN, false },
	{ "ngp", "ネオジオポケット", "Neo Geo Pocket / Color", "SNK", "mednafen_ngp", ".ngp.ngc", NULL, C_RED, false },
	{ "neogeo", "ネオジオ・アーケード", "Neo Geo & arcade", "SNK and others", "fbneo", ".zip.7z",
	  "Neo Geo games need neogeo.zip", C_YELLOW, false },
	{ "msx", "MSX", "MSX / MSX2", "ASCII and others", "fmsx", ".rom.mx1.mx2.dsk.cas",
	  "Needs the MSX system ROMs (MSX.ROM and friends)", C_PINK, false },
	{ "vb", "バーチャルボーイ", "Virtual Boy", "Nintendo", "mednafen_vb", ".vb.vboy", "Runs slowly on the Vita", C_RED, true },
	{ "pm", "ポケモンミニ", "Pokémon mini", "Nintendo", "pokemini", ".min", NULL, C_YELLOW, false },
	{ "ss", "セガサターン", "Sega Saturn", "Sega", "yabause", ".cue.iso.ccd.chd",
	  "Experimental and slow on the Vita; needs saturn_bios.bin", C_BLUE, true },
};
const int LINGO_SYSTEM_COUNT = sizeof(LINGO_SYSTEMS) / sizeof(LINGO_SYSTEMS[0]);

const struct LingoSystem* lingoSystemById(const char* id) {
	for (int i = 0; i < LINGO_SYSTEM_COUNT; ++i) {
		if (!strcmp(LINGO_SYSTEMS[i].id, id)) {
			return &LINGO_SYSTEMS[i];
		}
	}
	return NULL;
}

bool lingoSystemAccepts(const struct LingoSystem* s, const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	char ext[12];
	size_t n = strlen(dot);
	if (n >= sizeof(ext)) {
		return false;
	}
	for (size_t i = 0; i <= n; ++i) {
		char c = dot[i];
		ext[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
	}
	/* match a whole extension inside the list, e.g. ".gb" must not match ".gba" */
	const char* p = s->exts;
	while ((p = strstr(p, ext))) {
		char after = p[strlen(ext)];
		if (after == 0 || after == '.') {
			return true;
		}
		++p;
	}
	return false;
}

/* ---- ROM browser ---- */

struct Entry {
	char name[256];
	bool dir;
};

static int cmpEntry(const void* a, const void* b) {
	const struct Entry* x = a;
	const struct Entry* y = b;
	if (x->dir != y->dir) {
		return x->dir ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

#define LIST_W 600

static int listDir(const char* path, const struct LingoSystem* sys, struct Entry** out) {
	int cap = 128, n = 0;
	struct Entry* e = malloc(cap * sizeof(*e));
	if (!path[0]) {
		static const char* const roots[] = { "ux0:", "uma0:", "imc0:", "ur0:" };
		for (int i = 0; i < 4; ++i) {
			SceUID d = sceIoDopen(roots[i]);
			if (d >= 0) {
				sceIoDclose(d);
				snprintf(e[n].name, sizeof(e[n].name), "%s", roots[i]);
				e[n++].dir = true;
			}
		}
		*out = e;
		return n;
	}
	SceUID d = sceIoDopen(path);
	if (d < 0) {
		*out = e;
		return 0;
	}
	SceIoDirent de;
	while (sceIoDread(d, &de) > 0) {
		if (de.d_name[0] == '.') {
			continue;
		}
		bool dir = SCE_S_ISDIR(de.d_stat.st_mode);
		if (!dir && !lingoSystemAccepts(sys, de.d_name)) {
			continue;
		}
		if (n == cap) {
			cap *= 2;
			e = realloc(e, cap * sizeof(*e));
		}
		snprintf(e[n].name, sizeof(e[n].name), "%s", de.d_name);
		e[n++].dir = dir;
	}
	sceIoDclose(d);
	qsort(e, n, sizeof(*e), cmpEntry);
	*out = e;
	return n;
}

static void lastDirPath(const struct LingoSystem* s, char* out, size_t n) {
	snprintf(out, n, "ux0:data/lingo/lastdir-%s.txt", s->id);
}

/* Returns true with path filled when a game was picked. */
static bool browse(const struct LingoSystem* sys, char* path, size_t pathSize) {
	char dir[512] = "";
	char lp[64];
	lastDirPath(sys, lp, sizeof(lp));
	FILE* f = fopen(lp, "r");
	if (f) {
		if (fgets(dir, sizeof(dir), f)) {
			dir[strcspn(dir, "\r\n")] = 0;
		}
		fclose(f);
	}
	if (!dir[0]) {
		/* games sent from a phone land here */
		char sent[128];
		snprintf(sent, sizeof(sent), "ux0:data/lingo/games/%s", sys->id);
		SceUID d = sceIoDopen(sent);
		if (d >= 0) {
			sceIoDclose(d);
			snprintf(dir, sizeof(dir), "%s", sent);
		}
	}
	struct Entry* list = NULL;
	int n = listDir(dir, sys, &list);
	if (dir[0] && n == 0) {
		/* the remembered folder is gone */
		dir[0] = 0;
		free(list);
		n = listDir(dir, sys, &list);
	}
	int sel = 0;
	float scroll = 0;
	float dragFrom = 0;
	bool dragging = false;
	while (true) {
		uiPollInput();
		int go = -1;
		bool up = uiBack();
		if (IN.buttonsPressed & SCE_CTRL_DOWN) {
			sel = sel < n - 1 ? sel + 1 : sel;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			sel = sel > 0 ? sel - 1 : 0;
		}
		if (uiOk() && n) {
			go = sel;
		}
		float top = 104, bottom = 520, rowH = 46;
		if (sel * rowH - scroll > bottom - top - rowH) {
			scroll = sel * rowH - (bottom - top - rowH);
		}
		if (sel * rowH < scroll) {
			scroll = sel * rowH;
		}
		if (IN.pressed && IN.y > top && IN.y < bottom) {
			dragFrom = IN.y + scroll;
			dragging = true;
		}
		if (dragging && IN.down) {
			scroll = dragFrom - IN.y;
		}
		if (!IN.down) {
			dragging = false;
		}
		scroll = fmaxf(0, fminf(fmaxf(0, n * rowH - (bottom - top)), scroll));

		uiFrameBegin();
		uiChalkBackground();
		uiJp(32, 48, 26, sys->color, sys->jp);
		uiText(32 + uiJpW(26, sys->jp) + 14, 46, 16, C_SOFT, sys->en);
		uiTextWrap(32, 80, 700, 14, 18, C_MUTED, dir[0] ? dir : "Choose a drive", 1);
		if (sys->note) {
			uiText(SCREEN_W - 32 - uiTextW(13, sys->note), 80, 13, C_YELLOW, sys->note);
		}
		vita2d_set_clip_rectangle(0, (int) top, SCREEN_W, (int) bottom);
		vita2d_enable_clipping();
		for (int i = 0; i < n; ++i) {
			float y = top + i * rowH - scroll;
			if (y + rowH < top || y > bottom) {
				continue;
			}
			if (i == sel) {
				uiRound(24, y + 2, LIST_W - 24, rowH - 4, 10, uiAlpha(C_YELLOW, 0.16f));
			}
			uiText(44, y + 30, 17, list[i].dir ? C_YELLOW : C_CHALK, list[i].dir ? "▶" : "•");
			uiTextWrap(70, y + 30, LIST_W - 90, 17, 22, list[i].dir ? C_YELLOW : C_CHALK, list[i].name, 1);
			if (IN.released && fabsf(IN.y - IN.sy) < 12 && IN.y > y && IN.y < y + rowH && IN.y > top && IN.y < bottom) {
				go = i;
			}
		}
		vita2d_disable_clipping();
		if (!n) {
			uiTextCentered(LIST_W / 2, 280, 17, C_SOFT, "Nothing here for this console.");
		} else if (!list[sel].dir) {
			/* the game's picture: box art if Lingo has it, else a frame from your own play */
			float bx = LIST_W + 24, bw = SCREEN_W - bx - 24, by = top + 4, bh = bottom - top - 8;
			uiRound(bx, by, bw, bh, 14, C_SHEET);
			vita2d_texture* art = lingoCover(sys->id, list[sel].name);
			bool shot = false;
			if (!art) {
				art = lingoShotTexture(list[sel].name);
				shot = art != NULL;
			}
			if (art) {
				float tw = vita2d_texture_get_width(art), th = vita2d_texture_get_height(art);
				float k = fminf((bw - 24) / tw, (bh - 60) / th);
				vita2d_draw_texture_scale(art, bx + (bw - tw * k) / 2, by + 12 + (bh - 60 - th * k) / 2, k, k);
				uiTextCentered(bx + bw / 2, by + bh - 18, 12, C_MUTED, shot ? "from your last session" : "box art");
			} else {
				uiJpCentered(bx + bw / 2, by + bh / 2, 60, uiAlpha(sys->color, 0.35f), sys->jp);
				uiTextWrap(bx + 16, by + bh - 40, bw - 32, 12, 16, C_MUTED,
				           "No picture yet. Box art loads on Wi-Fi; otherwise Lingo keeps a frame after a minute of play.", 2);
			}
		}
		uiText(32, 536, 14, C_SOFT, "✕ Open    ○ Up a folder    Tap to choose");
		uiFrameEnd();

		if (up) {
			if (!dir[0]) {
				free(list);
				return false;
			}
			char* slash = strrchr(dir, '/');
			if (slash) {
				*slash = 0;
			} else {
				dir[0] = 0;
			}
			free(list);
			n = listDir(dir, sys, &list);
			sel = 0;
			scroll = 0;
		} else if (go >= 0 && go < n) {
			char next[768];
			if (!dir[0]) {
				snprintf(next, sizeof(next), "%s", list[go].name);
			} else if (dir[strlen(dir) - 1] == ':') {
				snprintf(next, sizeof(next), "%s%s", dir, list[go].name);
			} else {
				snprintf(next, sizeof(next), "%s/%s", dir, list[go].name);
			}
			if (list[go].dir) {
				snprintf(dir, sizeof(dir), "%s", next);
				free(list);
				n = listDir(dir, sys, &list);
				sel = 0;
				scroll = 0;
			} else {
				f = fopen(lp, "w");
				if (f) {
					fprintf(f, "%s\n", dir);
					fclose(f);
				}
				snprintf(path, pathSize, "%s", next);
				free(list);
				return true;
			}
		}
	}
}

/* ---- system picker ---- */

static void message(const char* title, const char* body) {
	double start = uiTime();
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiTextCentered(480, 220, 24, C_RED, title);
		uiTextWrap(160, 270, 640, 16, 24, C_CHALK, body, 6);
		uiTextCentered(480, 500, 15, C_SOFT, "Tap to go back");
		uiFrameEnd();
		if (uiTime() - start > 0.5 && (IN.released || uiBack() || uiOk())) {
			return;
		}
	}
}

bool lingoSystemsRun(char* path, size_t pathSize) {
	uiInit();
	int focus = 0;
	double start = uiTime();
	while (true) {
		uiPollInput();
		double now = uiTime();
		int cols = 4;
		if (IN.buttonsPressed & SCE_CTRL_RIGHT) {
			focus = (focus + 1) % LINGO_SYSTEM_COUNT;
		}
		if (IN.buttonsPressed & SCE_CTRL_LEFT) {
			focus = (focus + LINGO_SYSTEM_COUNT - 1) % LINGO_SYSTEM_COUNT;
		}
		if (IN.buttonsPressed & SCE_CTRL_DOWN) {
			focus = focus + cols < LINGO_SYSTEM_COUNT ? focus + cols : focus;
		}
		if (IN.buttonsPressed & SCE_CTRL_UP) {
			focus = focus >= cols ? focus - cols : focus;
		}
		uiFrameBegin();
		uiChalkBackground();
		uiJp(32, 54, 30, C_CHALK, "なにで あそぶ？");
		uiText(32, 82, 15, C_SOFT, "Pick a console. Every one gets the lookup keyboard, checkpoints and Kaku.");
		int pick = -1;
		float cw = 218, ch = 98;
		for (int i = 0; i < LINGO_SYSTEM_COUNT; ++i) {
			const struct LingoSystem* s = &LINGO_SYSTEMS[i];
			float x = 24 + (i % cols) * (cw + 12), y = 100 + (i / cols) * (ch + 10);
			float pop = fminf(1, (float) (now - start) * 5 - i * 0.05f);
			if (pop <= 0) {
				continue;
			}
			y += (1 - pop) * 16;
			bool held = uiHeld(x, y, cw, ch);
			uiRound(x, y + (held ? 2 : 0), cw, ch, 14, held ? C_SHEET_HI : C_SHEET);
			uiRoundOutline(x, y + (held ? 2 : 0), cw, ch, 14, i == focus ? 3 : 2, i == focus ? C_YELLOW : uiAlpha(s->color, 0.6f));
			float px = 22;
			while (px > 13 && uiJpW(px, s->jp) > cw - 24) {
				px -= 1;
			}
			uiJp(x + 12, y + 40, px, s->color, s->jp);
			uiTextWrap(x + 12, y + 66, cw - 24, 12, 15, C_CHALK, s->en, 2);
			if (s->slow) {
				uiText(x + cw - 50, y + 20, 11, C_YELLOW, "slow");
			}
			if (uiTapped(x, y, cw, ch) || (i == focus && uiOk())) {
				pick = i;
			}
		}
		uiText(32, 536, 14, C_SOFT, "○ Back");
		uiFrameEnd();
		if (uiBack()) {
			return false;
		}
		if (pick < 0) {
			continue;
		}
		const struct LingoSystem* s = &LINGO_SYSTEMS[pick];
		if (!browse(s, path, pathSize)) {
			start = uiTime();
			continue;
		}
		if (!s->core) {
			return true; /* mGBA takes it from here */
		}
		char self[128];
		snprintf(self, sizeof(self), "app0:cores/%s.self", s->core);
		char* argv[] = { path, (char*) s->id, NULL };
		int r = sceAppMgrLoadExec(self, argv, NULL);
		char why[160];
		snprintf(why, sizeof(why), "Could not start the %s emulator (error 0x%08X). Reinstalling Lingo may fix this.", s->en, r);
		message("That didn't start", why);
	}
}
