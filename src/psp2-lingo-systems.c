/* Lingo: the consoles, the system picker and the ROM browser. Game Boy and
 * GBA go to mGBA in this app; everything else launches the matching
 * libretro core packed alongside it in app0:/. */
#include "psp2-lingo-systems.h"
#include "psp2-lingo-net.h"
#include "psp2-lingo-shot.h"
#include "psp2-lingo-ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
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
		if (!dir && !(sys ? lingoSystemAccepts(sys, de.d_name) : lingoSystemFor(NULL, de.d_name) != NULL)) {
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
	snprintf(out, n, "ux0:data/lingo/lastdir-%s.txt", s ? s->id : "any");
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
		snprintf(sent, sizeof(sent), "ux0:data/lingo/games%s%s", sys ? "/" : "", sys ? sys->id : "");
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
		uiJp(32, 48, 26, sys ? sys->color : C_CHALK, sys ? sys->jp : "メモリーカード");
		uiText(32 + uiJpW(26, sys ? sys->jp : "メモリーカード") + 14, 46, 16, C_SOFT, sys ? sys->en : "Games anywhere on the memory card");
		uiTextWrap(32, 80, 700, 14, 18, C_MUTED, dir[0] ? dir : "Choose a drive", 1);
		if (sys && sys->note) {
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
			const struct LingoSystem* fs = sys ? sys : lingoSystemFor(NULL, list[sel].name);
			vita2d_texture* art = fs ? lingoCover(fs->id, list[sel].name) : NULL;
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
				if (fs) {
					uiJpCentered(bx + bw / 2, by + bh / 2, 60, uiAlpha(fs->color, 0.35f), fs->jp);
				}
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

/* ---- which console a file is for ---- */

static bool readAt(const char* path, long off, void* buf, size_t n) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	bool ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
	fclose(f);
	return ok;
}

static bool memHas(const uint8_t* b, size_t n, const char* s) {
	size_t l = strlen(s);
	for (size_t i = 0; i + l <= n; ++i) {
		if (!memcmp(b + i, s, l)) {
			return true;
		}
	}
	return false;
}

/* A disc image's first data: Saturn, Mega CD or PlayStation (or NULL) */
static const char* discKind(const char* path) {
	uint8_t b[0x9400];
	memset(b, 0, sizeof(b));
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	size_t n = fread(b, 1, sizeof(b), f);
	fclose(f);
	if (memHas(b, n < 64 ? n : 64, "SEGA SEGASATURN")) {
		return "ss";
	}
	if (memHas(b, n < 64 ? n : 64, "SEGADISCSYSTEM") || memHas(b, n < 64 ? n : 64, "SEGA SEGAKATANA")) {
		return "md";
	}
	if (memHas(b, n, "PLAYSTATION") || memHas(b, n, "Sony Computer Entertainment")) {
		return "ps1";
	}
	return NULL;
}

/* A .cue: open its first file and look; PC Engine CDs start with an audio
 * track and put their data on track 2. */
static const char* cueKind(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return "ps1";
	}
	char line[512], first[512] = "", t1[32] = "", t2[32] = "";
	int tracks = 0;
	while (fgets(line, sizeof(line), f)) {
		char* p = line;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (!strncasecmp(p, "FILE", 4) && !first[0]) {
			char* q1 = strchr(p, '"');
			char* q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			if (q1 && q2) {
				*q2 = 0;
				snprintf(first, sizeof(first), "%s", q1 + 1);
			}
		} else if (!strncasecmp(p, "TRACK", 5)) {
			char kind[32] = "";
			sscanf(p, "%*s %*s %31s", kind);
			if (tracks == 0) {
				snprintf(t1, sizeof(t1), "%s", kind);
			} else if (tracks == 1) {
				snprintf(t2, sizeof(t2), "%s", kind);
			}
			++tracks;
		}
	}
	fclose(f);
	if (first[0]) {
		char data[700];
		const char* slash = strrchr(path, '/');
		int dl = slash ? (int) (slash - path) : 0;
		snprintf(data, sizeof(data), "%.*s/%s", dl, path, first);
		const char* k = discKind(data);
		if (k) {
			return k;
		}
	}
	if (!strncasecmp(t1, "AUDIO", 5) && !strncasecmp(t2, "MODE1", 5)) {
		return "pce";
	}
	return "ps1";
}

/* the folder names people use, as a hint for formats several consoles share */
static const char* folderHint(const char* path) {
	static const char* const H[][2] = {
		{ "saturn", "ss" }, { "pce", "pce" }, { "pc engine", "pce" }, { "pcengine", "pce" }, { "turbografx", "pce" },
		{ "megacd", "md" }, { "mega cd", "md" }, { "segacd", "md" }, { "sega cd", "md" }, { "megadrive", "md" },
		{ "genesis", "md" }, { "psx", "ps1" }, { "ps1", "ps1" }, { "playstation", "ps1" }, { "msx", "msx" },
		{ "neogeo", "neogeo" }, { "arcade", "neogeo" }, { "fbneo", "neogeo" },
	};
	char low[512];
	size_t n = strlen(path);
	if (n >= sizeof(low)) {
		n = sizeof(low) - 1;
	}
	for (size_t i = 0; i < n; ++i) {
		low[i] = (char) tolower((unsigned char) path[i]);
	}
	low[n] = 0;
	char* name = strrchr(low, '/');
	if (name) {
		*name = 0; /* folders only */
	}
	for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); ++i) {
		if (strstr(low, H[i][0])) {
			return H[i][1];
		}
	}
	return NULL;
}

static bool extIs(const char* name, const char* ext) {
	const char* dot = strrchr(name, '.');
	return dot && !strcasecmp(dot, ext);
}

/* Which console runs this file. path may be NULL to decide by name alone
 * (formats shared by several consoles then get the most likely one). */
const struct LingoSystem* lingoSystemFor(const char* path, const char* name) {
	const char* hint = path ? folderHint(path) : NULL;
	if (extIs(name, ".cue") || extIs(name, ".ccd") || extIs(name, ".iso") || extIs(name, ".img") || extIs(name, ".chd")) {
		const char* k = NULL;
		if (path && extIs(name, ".cue")) {
			k = cueKind(path);
		} else if (path && (extIs(name, ".iso") || extIs(name, ".img"))) {
			k = discKind(path);
		}
		if (!k || (!strcmp(k, "ps1") && hint && (!strcmp(hint, "pce") || !strcmp(hint, "ss") || !strcmp(hint, "md")))) {
			k = hint && strcmp(hint, "msx") && strcmp(hint, "neogeo") ? hint : (k ? k : "ps1");
		}
		return lingoSystemById(k);
	}
	if (extIs(name, ".bin")) {
		/* a Mega Drive cartridge says SEGA at 0x100; otherwise a disc track */
		uint8_t b[16];
		if (path && readAt(path, 0x100, b, sizeof(b))) {
			if (!memcmp(b, "SEGA", 4) || !memcmp(b + 1, "SEGA", 4)) {
				return lingoSystemById("md");
			}
			const char* k = discKind(path);
			if (k) {
				return lingoSystemById(k);
			}
		}
		return lingoSystemById(hint && !strcmp(hint, "ps1") ? "ps1" : "md");
	}
	if (extIs(name, ".zip") || extIs(name, ".7z")) {
		/* a zipped cartridge: the name of the file inside says which console */
		uint8_t h[30];
		char inner[256];
		if (path && extIs(name, ".zip") && readAt(path, 0, h, sizeof(h)) && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) {
			unsigned nl = h[26] | (h[27] << 8);
			if (nl && nl < sizeof(inner) && readAt(path, 30, inner, nl)) {
				inner[nl] = 0;
				for (int i = 0; i < LINGO_SYSTEM_COUNT; ++i) {
					const struct LingoSystem* s = &LINGO_SYSTEMS[i];
					if (strcmp(s->id, "neogeo") && lingoSystemAccepts(s, inner) && !extIs(inner, ".zip")) {
						return s;
					}
				}
			}
		}
		return lingoSystemById("neogeo");
	}
	for (int i = 0; i < LINGO_SYSTEM_COUNT; ++i) {
		if (lingoSystemAccepts(&LINGO_SYSTEMS[i], name)) {
			return &LINGO_SYSTEMS[i];
		}
	}
	return NULL;
}

/* ---- the library: every game in one folder ---- */

#define GAMES_DIR "ux0:data/lingo/games"
#define MAX_GAMES 600

struct Game {
	char path[400];
	char file[128];
	char title[128];
	const struct LingoSystem* sys;
	bool panelReads; /* a Pokémon game the panel reads line by line */
};

static struct Game* sGames;
static int sNGames;

/* "Pocket Monsters - Ruby (Japan) (Rev 1) [!].gba" → "Pocket Monsters - Ruby" */
static void cleanTitle(const char* file, char* out, size_t n) {
	snprintf(out, n, "%s", file);
	char* dot = strrchr(out, '.');
	if (dot) {
		*dot = 0;
	}
	char* cut = NULL;
	for (char* p = out; *p; ++p) {
		if ((*p == '(' || *p == '[') && p > out) {
			cut = p;
			break;
		}
	}
	if (cut) {
		*cut = 0;
	}
	size_t l = strlen(out);
	while (l && (out[l - 1] == ' ' || out[l - 1] == '_' || out[l - 1] == '-')) {
		out[--l] = 0;
	}
	for (char* p = out; *p; ++p) {
		if (*p == '_') {
			*p = ' ';
		}
	}
	if (!out[0]) {
		snprintf(out, n, "%s", file);
	}
}

static bool gen3Pokemon(const char* path) {
	char code[4];
	if (!readAt(path, 0xAC, code, 4)) {
		return false;
	}
	static const char* const C[] = { "AXVJ", "AXPJ", "BPEJ", "BPRJ", "BPGJ" };
	for (int i = 0; i < 5; ++i) {
		if (!memcmp(code, C[i], 4)) {
			return true;
		}
	}
	return false;
}

/* a disc's track files are listed by its .cue/.ccd/.m3u: show only that */
static bool dirHasSheet(const char* dir) {
	SceUID d = sceIoDopen(dir);
	if (d < 0) {
		return false;
	}
	SceIoDirent de;
	bool has = false;
	while (!has && sceIoDread(d, &de) > 0) {
		has = extIs(de.d_name, ".cue") || extIs(de.d_name, ".ccd") || extIs(de.d_name, ".m3u");
	}
	sceIoDclose(d);
	return has;
}

static void scanDir(const char* dir, int depth) {
	SceUID d = sceIoDopen(dir);
	if (d < 0) {
		return;
	}
	bool sheet = dirHasSheet(dir);
	SceIoDirent de;
	while (sNGames < MAX_GAMES && sceIoDread(d, &de) > 0) {
		if (de.d_name[0] == '.') {
			continue;
		}
		char full[400];
		snprintf(full, sizeof(full), "%s/%s", dir, de.d_name);
		if (SCE_S_ISDIR(de.d_stat.st_mode)) {
			if (depth < 3) {
				scanDir(full, depth + 1);
			}
			continue;
		}
		if (sheet && (extIs(de.d_name, ".bin") || extIs(de.d_name, ".img") || extIs(de.d_name, ".sub"))) {
			continue;
		}
		if (extIs(de.d_name, ".part") || extIs(de.d_name, ".sav") || extIs(de.d_name, ".srm") || extIs(de.d_name, ".txt")) {
			continue;
		}
		if (!lingoSystemFor(NULL, de.d_name)) {
			continue;
		}
		struct Game* g = &sGames[sNGames];
		memset(g, 0, sizeof(*g));
		snprintf(g->path, sizeof(g->path), "%s", full);
		snprintf(g->file, sizeof(g->file), "%s", de.d_name);
		g->sys = lingoSystemFor(full, de.d_name);
		if (!g->sys) {
			continue;
		}
		cleanTitle(de.d_name, g->title, sizeof(g->title));
		g->panelReads = !strcmp(g->sys->id, "gba") && gen3Pokemon(full);
		++sNGames;
	}
	sceIoDclose(d);
}

static int cmpGame(const void* a, const void* b) {
	return strcasecmp(((const struct Game*) a)->title, ((const struct Game*) b)->title);
}

static void scanGames(void) {
	if (!sGames) {
		sGames = malloc(sizeof(*sGames) * MAX_GAMES);
	}
	sNGames = 0;
	sceIoMkdir("ux0:data/lingo", 0777);
	sceIoMkdir(GAMES_DIR, 0777);
	scanDir(GAMES_DIR, 0);
	qsort(sGames, (size_t) sNGames, sizeof(*sGames), cmpGame);
}

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

/* the consoles that work, as a row of chips; returns the y under them */
static float consoleChips(float x0, float y, float w) {
	float x = x0;
	for (int i = 0; i < LINGO_SYSTEM_COUNT; ++i) {
		const struct LingoSystem* s = &LINGO_SYSTEMS[i];
		char label[80];
		snprintf(label, sizeof(label), "%s%s", s->en, s->slow ? " (slow)" : "");
		float cw = uiTextW(11, label) + 14;
		if (x + cw > x0 + w) {
			x = x0;
			y += 22;
		}
		uiRound(x, y, cw, 19, 7, uiAlpha(s->color, 0.15f));
		uiText(x + 7, y + 14, 11, s->color, label);
		x += cw + 5;
	}
	return y + 22;
}

static void drawGameCard(const struct Game* g, float x, float y, float w, float h, bool focus) {
	const struct LingoSystem* s = g->sys;
	uiRound(x + 5, y + 8, w, h, 22, RGBA(0, 0, 0, 0x55));
	uiRound(x, y, w, h, 22, C_SHEET);
	uiRoundOutline(x, y, w, h, 22, focus ? 3 : 2, focus ? s->color : C_LINE);
	/* the picture: box art, else a frame from your own play, else chalk */
	float bh = h - 132;
	uiRound(x + 14, y + 14, w - 28, bh, 16, uiAlpha(s->color, 0.12f));
	vita2d_texture* art = focus ? lingoCover(s->id, g->file) : NULL;
	bool shot = false;
	if (!art && focus) {
		art = lingoShotTexture(g->file);
		shot = art != NULL;
	}
	if (art) {
		float tw = vita2d_texture_get_width(art), th = vita2d_texture_get_height(art);
		float k = fminf((w - 40) / tw, (bh - 8) / th);
		vita2d_draw_texture_scale(art, x + w / 2 - tw * k / 2, y + 18 + (bh - 8 - th * k) / 2, k, k);
	} else {
		float px = 46;
		while (px > 16 && uiJpW(px, s->jp) > w - 60) {
			px -= 2;
		}
		uiJpCentered(x + w / 2, y + 14 + bh / 2 + px / 3, px, uiAlpha(s->color, 0.45f), s->jp);
	}
	/* title, shrunk to fit */
	float tpx = 22;
	while (tpx > 13 && uiTextW(tpx, g->title) > w - 36) {
		tpx -= 1;
	}
	char t[128];
	snprintf(t, sizeof(t), "%s", g->title);
	while (strlen(t) > 4 && uiTextW(tpx, t) > w - 36) {
		t[strlen(t) - 1] = 0;
	}
	uiTextCentered(x + w / 2, y + bh + 48, tpx, C_CHALK, t);
	/* chips: console, and how the study panel reads it */
	const char* chips[2] = { s->en, g->panelReads ? "Study panel reads every line" : "Study panel learns its text" };
	uint32_t cols[2] = { s->color, g->panelReads ? C_GREEN : C_BLUE };
	float total = 0;
	for (int i = 0; i < 2; ++i) {
		total += uiTextW(12, chips[i]) + 18 + (i ? 6 : 0);
	}
	float cx = x + (w - fminf(total, w - 28)) / 2, cy = y + bh + 62;
	for (int i = 0; i < 2; ++i) {
		float cw = uiTextW(12, chips[i]) + 18;
		uiRound(cx, cy, cw, 24, 8, uiAlpha(cols[i], 0.16f));
		uiText(cx + 9, cy + 17, 12, cols[i], chips[i]);
		cx += cw + 6;
	}
	const char* foot = s->note ? s->note : (shot ? "Picture: from your last session" : (art ? "Box art: libretro-thumbnails" : NULL));
	if (foot) {
		uiTextCentered(x + w / 2, y + h - 18, 12, s->note ? C_YELLOW : C_MUTED, foot);
	}
}

static void saveLast(const char* path) {
	FILE* f = fopen("ux0:data/lingo/lastgame.txt", "w");
	if (f) {
		fprintf(f, "%s\n", path);
		fclose(f);
	}
}

static int findLast(void) {
	char last[400] = "";
	FILE* f = fopen("ux0:data/lingo/lastgame.txt", "r");
	if (f) {
		if (fgets(last, sizeof(last), f)) {
			last[strcspn(last, "\r\n")] = 0;
		}
		fclose(f);
	}
	for (int i = 0; i < sNGames; ++i) {
		if (!strcmp(sGames[i].path, last)) {
			return i;
		}
	}
	return 0;
}

/* Start a game: true (path set) when mGBA runs it, otherwise its core
 * starts and this does not return; false with a message if it can't. */
static bool launch(const char* file, const struct LingoSystem* s, char* path, size_t pathSize) {
	snprintf(path, pathSize, "%s", file);
	saveLast(file);
	if (!s->core) {
		return true; /* mGBA takes it from here */
	}
	char self[128];
	snprintf(self, sizeof(self), "app0:/%s.self", s->core);
	char* argv[] = { path, (char*) s->id, NULL };
	int r = sceAppMgrLoadExec(self, argv, NULL);
	char why[200];
	snprintf(why, sizeof(why), "Could not start the %s emulator (error 0x%08X). Reinstalling Lingo may fix this.", s->en, r);
	message("That didn't start", why);
	return false;
}

bool lingoSystemsRun(char* path, size_t pathSize) {
	uiInit();
	scanGames();
	int cur = findLast();
	float pos = (float) cur, drag = 0;
	double last = uiTime(), lastScan = last;
	while (true) {
		uiPollInput();
		double now = uiTime();
		float dt = (float) (now - last);
		last = now;
		if (now - lastScan > 4 && !IN.down) {
			/* games copied over while the screen is open */
			char keep[400] = "";
			if (cur < sNGames) {
				snprintf(keep, sizeof(keep), "%s", sGames[cur].path);
			}
			int before = sNGames;
			scanGames();
			lastScan = now;
			if (sNGames != before) {
				for (int i = 0; i < sNGames; ++i) {
					if (!strcmp(sGames[i].path, keep)) {
						cur = i;
					}
				}
			}
			if (cur >= sNGames) {
				cur = sNGames ? sNGames - 1 : 0;
			}
		}
		if (IN.buttonsPressed & (SCE_CTRL_RIGHT | SCE_CTRL_R1)) {
			cur = cur < sNGames - 1 ? cur + 1 : cur;
		}
		if (IN.buttonsPressed & (SCE_CTRL_LEFT | SCE_CTRL_L1)) {
			cur = cur > 0 ? cur - 1 : cur;
		}
		if (IN.down && IN.sy > 130 && IN.sy < 510) {
			drag = IN.x - IN.sx;
		}
		if (IN.released && fabsf(drag) > 60) {
			cur += drag < 0 ? 1 : -1;
			cur = cur < 0 ? 0 : (cur >= sNGames ? (sNGames ? sNGames - 1 : 0) : cur);
		}
		if (!IN.down) {
			drag *= 0.6f;
		}
		pos += (cur - pos) * fminf(1, dt * 10);
		int play = -1;
		bool other = false;

		uiFrameBegin();
		uiChalkBackground();
		uiJp(36, 50, 30, C_CHALK, "あそぶ");
		char sub[160];
		snprintf(sub, sizeof(sub), "%d game%s in %s/ — any of these consoles:", sNGames, sNGames == 1 ? "" : "s", GAMES_DIR);
		uiText(36 + uiJpW(30, "あそぶ") + 16, 46, 14, C_SOFT, sub);
		float top = consoleChips(36, 60, SCREEN_W - 72) + 8;
		float cw = 440, ch = 508 - top;
		if (!sNGames) {
			uiRound(140, top + 10, SCREEN_W - 280, ch - 20, 22, C_SHEET);
			uiJpCentered(SCREEN_W / 2, top + 90, 40, uiAlpha(C_CHALK, 0.2f), "からっぽ");
			uiTextWrap(180, top + 140, SCREEN_W - 360, 16, 24, C_CHALK,
			           "Put your games in ux0:data/lingo/games/ — ROMs, disc images (.cue with its .bin files, .iso, .chd) "
			           "or .zip, from any console above. Subfolders are fine. Lingo works out which console each one is for.",
			           6);
			uiTextWrap(180, top + 300, SCREEN_W - 360, 14, 20, C_SOFT,
			           "You can also send games from your phone with Send games on the title screen. Use backups of games you own.", 3);
		} else {
			for (int i = 0; i < sNGames; ++i) {
				float off = (i - pos) * (cw + 30) + drag;
				float x = SCREEN_W / 2 - cw / 2 + off;
				if (x > SCREEN_W || x + cw < 0) {
					continue;
				}
				drawGameCard(&sGames[i], x, top, cw, ch, i == cur);
				if (uiTapped(x, top, cw, ch) && fabsf(drag) < 10) {
					if (i == cur) {
						play = i;
					} else {
						cur = i;
					}
				}
			}
			if (sNGames <= 40) {
				for (int i = 0; i < sNGames; ++i) {
					uiCircle(SCREEN_W / 2 - (sNGames - 1) * 9 + i * 18, 522, i == cur ? 5 : 3.5f, i == cur ? C_YELLOW : C_LINE);
				}
			} else {
				char n[32];
				snprintf(n, sizeof(n), "%d / %d", cur + 1, sNGames);
				uiTextCentered(SCREEN_W / 2, 527, 13, C_SOFT, n);
			}
		}
		uiText(36, 540, 14, C_SOFT, sNGames ? "✕ Play    ◀ ▶ Browse    ○ Back" : "○ Back");
		const char* ol = "△ Elsewhere on the card";
		float ow = uiTextW(13, ol) + 20;
		if (uiTapped(SCREEN_W - 36 - ow, 520, ow, 30)) {
			other = true;
		}
		uiRound(SCREEN_W - 36 - ow, 520, ow, 28, 9, C_SHEET);
		uiText(SCREEN_W - 26 - ow, 539, 13, C_SOFT, ol);
		uiFrameEnd();

		if (uiBack()) {
			return false;
		}
		if (uiOk() && sNGames) {
			play = cur;
		}
		if (IN.buttonsPressed & SCE_CTRL_TRIANGLE) {
			other = true;
		}
		if (play >= 0) {
			if (launch(sGames[play].path, sGames[play].sys, path, pathSize)) {
				return true;
			}
			continue;
		}
		if (other) {
			/* a game kept somewhere else */
			char picked[512];
			if (browse(NULL, picked, sizeof(picked))) {
				const char* name = strrchr(picked, '/');
				name = name ? name + 1 : picked;
				const struct LingoSystem* s = lingoSystemFor(picked, name);
				if (s && launch(picked, s, path, pathSize)) {
					return true;
				}
			}
		}
	}
}
