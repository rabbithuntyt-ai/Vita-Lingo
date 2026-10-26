/* Lingo: where the study panel's text comes from, for any game.
 *
 * Keeps one character table per game (ux0:data/lingo/tables/<game>.tbl).
 * Until there is one, it learns: every few frames it samples the game's
 * memory and, in a background thread, tries to work out the encoding.
 * The player can also teach it a line. See psp2-lingo-reader.h. */
#include "psp2-lingo-reader.h"
#include "psp2-lingo-textsrc.h"
#include "psp2-lingo-ui.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define TABLES "ux0:data/lingo/tables"

static struct LingoTable* sTable;     /* the table in use */
static struct LingoTable* sFound;     /* the detector's result, handed over */
static bool sHave;
static struct LingoReader sReader;
static struct LingoDetect* sDetect;
static struct LingoRegions sRegions;
static char sSystem[16], sKey[160], sBase[160];
static uint32_t sFrame;
static int sSamples;
static char sStatus[96];
static char sLastWhy[200];

/* background detection */
static pthread_t sThread;
static volatile int sBusy;   /* 1 while the thread is working */
static volatile int sResult; /* 1 found, -1 not yet */
static bool sThreadStarted;

static void tablePath(const char* name, char* out, size_t n) {
	char safe[160];
	size_t k = 0;
	for (const char* p = name; *p && k < sizeof(safe) - 1; ++p) {
		char c = *p;
		safe[k++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		             c == '.' || c == ' ' || c == '(' || c == ')')
			? c
			: '_';
	}
	safe[k] = 0;
	snprintf(out, n, TABLES "/%s.tbl", safe);
}

static bool ensureBuffers(void) {
	if (!sTable) {
		sTable = malloc(sizeof(*sTable));
	}
	if (!sFound) {
		sFound = malloc(sizeof(*sFound));
	}
	return sTable && sFound;
}

static void useTable(void) {
	lingoReaderFree(&sReader);
	lingoReaderInit(&sReader, sTable);
	sHave = true;
	if (sDetect) {
		lingoDetectFree(sDetect);
		sDetect = NULL;
	}
}

void lingoTextSetGame(const char* system, const char* key, const char* baseName, const uint8_t* rom, size_t romSize) {
	if (!ensureBuffers()) {
		return;
	}
	/* a detection still running for the last game finishes into nothing */
	while (sBusy) {
		/* rare and short: the thread checks no state of ours */
	}
	sHave = false;
	sFrame = 0;
	sSamples = 0;
	sLastWhy[0] = 0;
	snprintf(sSystem, sizeof(sSystem), "%s", system ? system : "");
	snprintf(sKey, sizeof(sKey), "%s", key ? key : "game");
	snprintf(sBase, sizeof(sBase), "%s", baseName ? baseName : "");
	lingoReaderFree(&sReader);
	char path[256];
	tablePath(sKey, path, sizeof(path));
	if (lingoTableLoad(sTable, path)) {
		useTable();
		return;
	}
	if (sBase[0]) {
		tablePath(sBase, path, sizeof(path));
		if (lingoTableLoad(sTable, path)) {
			useTable();
			return;
		}
	}
	if (lingoTablePreset(sTable, sSystem, rom, romSize)) {
		useTable();
		return;
	}
	if (!sDetect) {
		sDetect = lingoDetectNew();
	}
}

void lingoTextGameUnloaded(void) {
	while (sBusy) {
	}
	lingoReaderFree(&sReader);
	if (sDetect) {
		lingoDetectFree(sDetect);
		sDetect = NULL;
	}
	sHave = false;
}

void lingoTextSetRegions(const struct LingoRegions* rs) {
	sRegions = *rs;
}

static void* detectThread(void* arg) {
	(void) arg;
	char why[200];
	struct LingoRegions rs = sRegions;
	bool ok = lingoDetectFinish(sDetect, &rs, sFound, why, sizeof(why));
	snprintf(sLastWhy, sizeof(sLastWhy), "%s", why);
	sResult = ok ? 1 : -1;
	sBusy = 0;
	return NULL;
}

static void saveTable(void) {
	sceIoMkdir("ux0:data/lingo", 0777);
	sceIoMkdir(TABLES, 0777);
	char path[256];
	tablePath(sKey, path, sizeof(path));
	lingoTableSave(sTable, path);
}

bool lingoTextStep(struct LingoText* out) {
	++sFrame;
	if (sHave) {
		return lingoReaderStep(&sReader, &sRegions, out);
	}
	if (!sDetect || !sRegions.n) {
		return false;
	}
	if (sBusy) {
		return false;
	}
	if (sResult == 1) {
		/* the thread found a table: take it */
		sResult = 0;
		memcpy(sTable, sFound, sizeof(*sTable));
		saveTable();
		useTable();
		return false;
	}
	sResult = 0;
	if (sFrame % 6 == 0) {
		lingoDetectSample(sDetect, &sRegions);
		++sSamples;
	}
	/* try every ten seconds or so once some text has gone by */
	if (sSamples >= 100 && sSamples % 100 == 0 && lingoDetectConfidence(sDetect) >= 0.25f) {
		sBusy = 1;
		if (pthread_create(&sThread, NULL, detectThread, NULL) == 0) {
			pthread_detach(sThread);
			sThreadStarted = true;
		} else {
			detectThread(NULL); /* no thread: do it here */
		}
	}
	return false;
}

const char* lingoTextStatus(float* progress, bool* reading) {
	*progress = 0;
	*reading = sHave;
	if (sHave) {
		snprintf(sStatus, sizeof(sStatus), "Reading text · %s", sTable->source[0] ? sTable->source : "table");
		*progress = 1;
	} else if (sDetect) {
		*progress = lingoDetectConfidence(sDetect);
		snprintf(sStatus, sizeof(sStatus), sBusy ? "Checking what it has seen…" : "Learning how this game writes…");
	} else {
		snprintf(sStatus, sizeof(sStatus), "Text reading is off");
	}
	return sStatus;
}

void lingoTextForget(void) {
	while (sBusy) {
	}
	char path[256];
	tablePath(sKey, path, sizeof(path));
	sceIoRemove(path);
	lingoReaderFree(&sReader);
	sHave = false;
	if (!sDetect) {
		sDetect = lingoDetectNew();
	}
	sSamples = 0;
}

/* ---- a snapshot for bug reports ---- */

/* Saves this frame's memory, the table and what the reader finds in it to
 * ux0:data/lingo/debug/, so a game that won't read can be looked into. */
static int debugDump(char* where, size_t n) {
	sceIoMkdir("ux0:data/lingo", 0777);
	sceIoMkdir("ux0:data/lingo/debug", 0777);
	char safe[100];
	size_t k = 0;
	for (const char* p = sKey; *p && k < sizeof(safe) - 1; ++p) {
		char c = *p;
		safe[k++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
	}
	safe[k] = 0;
	int files = 0;
	for (int i = 0; i < sRegions.n; ++i) {
		char path[256];
		snprintf(path, sizeof(path), "ux0:data/lingo/debug/%s-%s.bin", safe, sRegions.r[i].name);
		FILE* f = fopen(path, "wb");
		if (f) {
			fwrite(sRegions.r[i].data, 1, sRegions.r[i].size, f);
			fclose(f);
			++files;
		}
	}
	char path[256];
	snprintf(path, sizeof(path), "ux0:data/lingo/debug/%s.txt", safe);
	FILE* f = fopen(path, "w");
	if (f) {
		fprintf(f, "game %s (%s), file %s\n", sKey, sSystem, sBase);
		for (int i = 0; i < sRegions.n; ++i) {
			fprintf(f, "region %s: %u bytes\n", sRegions.r[i].name, sRegions.r[i].size);
		}
		fprintf(f, "table: %s\nlast detection note: %s\n", sHave ? sTable->source : "none", sLastWhy);
		if (sHave) {
			static struct LingoText lines[24];
			int nl = lingoReaderFindAll(sTable, &sRegions, lines, 24);
			fprintf(f, "%d lines found now:\n", nl);
			for (int i = 0; i < nl; ++i) {
				char s[LINGO_MAX_TEXT * 4];
				lingoUtf8FromCodepoints(lines[i].cp, lines[i].len, s, sizeof(s));
				fprintf(f, "  [%08x] %s\n", (unsigned) lines[i].start, s);
			}
		}
		fclose(f);
		++files;
	}
	if (sHave) {
		snprintf(path, sizeof(path), "ux0:data/lingo/debug/%s.tbl", safe);
		lingoTableSave(sTable, path);
	}
	snprintf(where, n, "ux0:data/lingo/debug/%s.*", safe);
	return files;
}

/* ---- teaching ---- */

static void teachFrame(const char* title, const char* body, uint32_t col) {
	uiFrameBegin();
	uiChalkBackground();
	uiText(40, 60, 22, C_YELLOW, "TEACH LINGO THIS GAME");
	uiTextCentered(SCREEN_W / 2, 250, 22, col, title);
	uiTextWrap(160, 290, SCREEN_W - 320, 16, 24, C_CHALK, body, 6);
	uiFrameEnd();
}

void lingoTextTeachRun(void) {
	uiInit();
	if (!ensureBuffers()) {
		return;
	}
	static uint32_t typed[48];
	static int n;
	static bool kata;
	while (true) {
		uiPollInput();
		uiFrameBegin();
		uiChalkBackground();
		uiText(24, 34, 15, C_YELLOW, "TEACH LINGO THIS GAME");
		uiTextWrap(24, 58, 560, 14, 20, C_SOFT,
		           "Type the line in the game's text box, in kana, exactly as it appears (small っ, ゛ marks, spaces). "
		           "Eight or more letters works best. The game waits while you type.",
		           3);
		uiRound(24, 128, 912, 82, 14, C_SHEET);
		char s[200];
		lingoUtf8FromCodepoints(typed, n, s, sizeof(s));
		uiJp(40, 182, 30, C_CHALK, s);
		if (sHave) {
			char st[120];
			snprintf(st, sizeof(st), "Now: reading (%s). Teaching again replaces it.", sTable->source);
			uiText(600, 40, 13, C_GREEN, st);
			if (uiButton(760, 64, 176, 44, "Forget it", NULL, C_RED, false)) {
				lingoTextForget();
			}
		}
		bool done = lingoKeyboard(230, typed, &n, 48, &kata, true);
		bool dump = uiButton(596, 64, 156, 44, "Debug snapshot", NULL, C_SOFT, false);
		uiFrameEnd();
		if (dump) {
			char where[160], msg[240];
			int k = debugDump(where, sizeof(where));
			snprintf(msg, sizeof(msg), "Saved %d files to %s. Send them with a note about the game if text won't read.", k, where);
			double start = uiTime();
			while (true) {
				uiPollInput();
				teachFrame("Snapshot saved", msg, C_GREEN);
				if (uiTime() - start > 0.4 && (IN.released || uiOk() || uiBack())) {
					break;
				}
			}
			continue;
		}
		if (uiBack()) {
			return;
		}
		if (!done) {
			continue;
		}
		if (n == 0) {
			return;
		}
		teachFrame("Searching the game's memory…", "", C_CHALK);
		static struct LingoTable taught;
		char why[200];
		bool ok = lingoTeach(&sRegions, typed, n, &taught, why, sizeof(why));
		if (ok) {
			while (sBusy) {
			}
			memcpy(sTable, &taught, sizeof(taught));
			saveTable();
			useTable();
			n = 0;
		}
		double start = uiTime();
		while (true) {
			uiPollInput();
			teachFrame(ok ? "Got it!" : "Not found", why, ok ? C_GREEN : C_RED);
			if (uiTime() - start > 0.4 && (IN.released || uiOk() || uiBack())) {
				break;
			}
		}
		if (ok) {
			return;
		}
	}
}
