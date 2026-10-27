/* Lingo: where the study panel's text comes from, for any game.
 *
 * Keeps one character table per game (ux0:data/lingo/tables/<game>.tbl).
 * Until there is one, it learns: every few frames it samples the game's
 * memory and, in a background thread, tries to work out the encoding.
 * The player can also teach it a line. See psp2-lingo-reader.h. */
#include "psp2-lingo-ocr.h"
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
/* checking a guessed table against the dictionary */
static int sJudged, sGood;
static bool sTrusted, sNoAuto;
static uint32_t sFrame;
static int sSamples;
static char sStatus[96];
static char sLastWhy[200];

/* background detection */
static pthread_t sThread;
static volatile int sBusy;   /* 1 while the thread is working */
static volatile int sResult; /* 1 found, -1 not yet */
static bool sThreadStarted;

/* screen reading, for games whose text isn't found in memory */
static struct LingoOcr* sOcr;
static bool sOcrTried;
static const void* sFramePx;
static int sFrameW, sFrameH, sFrameStride, sFrameFmt;
static uint8_t* sLuma;
static int sLumaW, sLumaH, sLumaCap;
static pthread_t sOcrThread;
static volatile int sOcrBusy, sOcrDone;
static struct LingoOcrLine sOcrLines[16];
static int sOcrN;
static uint32_t sOcrCand, sOcrLast;
static int sOcrStable;
static uint32_t sOcrPrev[16];
static int sOcrNPrev;
static uint32_t sOcrSeenAt; /* frame of the last line read off the screen */
static void ocrWait(void);
static void ocrCachePath(char* out, size_t n);

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

static bool guessed(void) {
	return !strcmp(sTable->source, "found automatically") || !strcmp(sTable->source, "Shift-JIS");
}

static void noAutoPath(char* out, size_t n) {
	char t[256];
	tablePath(sKey, t, sizeof(t));
	snprintf(out, n, "%.*s.noauto", (int) strlen(t) - 4, t);
}

static void useTable(void) {
	sJudged = sGood = 0;
	sTrusted = !guessed();
	lingoReaderFree(&sReader);
	lingoReaderInit(&sReader, sTable);
	sHave = true;
	if (sDetect) {
		lingoDetectFree(sDetect);
		sDetect = NULL;
	}
}

/* Shift-JIS is how disc games and computers store text; cartridge consoles
 * use their own tile codes. Mega Drive and PC Engine cover both (Mega CD,
 * CD-ROM²), so they need more evidence. */
static int sjisMin(void) {
	static const char* const DISC[] = { "ps1", "ss", "msx" };
	for (size_t i = 0; i < sizeof(DISC) / sizeof(DISC[0]); ++i) {
		if (!strcmp(sSystem, DISC[i])) {
			return 60;
		}
	}
	if (!strcmp(sSystem, "md") || !strcmp(sSystem, "pce") || !sSystem[0]) {
		return 200;
	}
	return 0;
}

/* A saved table that can't be right for this console: an automatic
 * Shift-JIS guess on a cartridge (earlier versions made these) */
static bool tableFits(void) {
	if (sTable->mode == LCM_SJIS && sjisMin() == 0) {
		return false;
	}
	return true;
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
	ocrWait();
	sOcrN = 0;
	sOcrDone = 0;
	sOcrCand = sOcrLast = 0;
	sOcrStable = 0;
	sOcrNPrev = 0;
	sOcrSeenAt = 0;
	if (sOcr) {
		char cp[256];
		ocrCachePath(cp, sizeof(cp));
		lingoOcrSetCache(sOcr, cp);
	}
	char path[256];
	tablePath(sKey, path, sizeof(path));
	if (lingoTableLoad(sTable, path) && tableFits()) {
		useTable();
		return;
	}
	if (sBase[0]) {
		tablePath(sBase, path, sizeof(path));
		if (lingoTableLoad(sTable, path) && tableFits()) {
			useTable();
			return;
		}
	}
	if (lingoTablePreset(sTable, sSystem, rom, romSize)) {
		useTable();
		return;
	}
	/* a guess for this game turned out to be junk before: read the screen
	 * (and let the player teach a line) instead of guessing again */
	char na[280];
	noAutoPath(na, sizeof(na));
	FILE* f = fopen(na, "r");
	sNoAuto = f != NULL;
	if (f) {
		fclose(f);
		if (sDetect) {
			lingoDetectFree(sDetect);
			sDetect = NULL;
		}
		return;
	}
	if (!sDetect) {
		sDetect = lingoDetectNew();
	}
	lingoDetectSjis(sDetect, sjisMin());
}

static void ocrWait(void) {
	while (sOcrBusy) {
	}
}

static void ocrCachePath(char* out, size_t n) {
	char safe[100];
	size_t k = 0;
	for (const char* p = sKey; *p && k < sizeof(safe) - 1; ++p) {
		char c = *p;
		safe[k++] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
	}
	safe[k] = 0;
	snprintf(out, n, "ux0:data/lingo/glyphs/%s.bin", safe);
}

void lingoTextGameUnloaded(void) {
	while (sBusy) {
	}
	ocrWait();
	if (sOcr) {
		sceIoMkdir("ux0:data/lingo/glyphs", 0777);
		lingoOcrSaveCache(sOcr);
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

void lingoTextSetFrame(const void* pixels, int w, int h, int stride, int format) {
	sFramePx = pixels;
	sFrameW = w;
	sFrameH = h;
	sFrameStride = stride;
	sFrameFmt = format;
}

/* real words in a line, for settling doubtful letters */
static int coverage(const uint32_t* cp, int n, void* user) {
	(void) user;
	const struct LingoDict* d = lingoDictGet();
	if (!d) {
		return 0;
	}
	static struct LingoToken tok[LINGO_MAX_TOKENS];
	int k = lingoTokenize(d, cp, n, tok, LINGO_MAX_TOKENS);
	int s = 0;
	for (int i = 0; i < k; ++i) {
		if (tok[i].nIds) {
			s += tok[i].len * tok[i].len;
		}
	}
	return s;
}

static bool grabLuma(void) {
	if (!sFramePx || sFrameW <= 0 || sFrameH <= 0 || sFrameW > 2048) {
		return false;
	}
	if (sLumaCap < sFrameW * sFrameH) {
		free(sLuma);
		sLuma = malloc((size_t) sFrameW * sFrameH);
		sLumaCap = sLuma ? sFrameW * sFrameH : 0;
		if (!sLuma) {
			return false;
		}
	}
	lingoOcrLuma(sFramePx, sFrameW, sFrameH, sFrameStride, sFrameFmt, sLuma);
	sLumaW = sFrameW;
	sLumaH = sFrameH;
	return true;
}

static void* ocrThread(void* arg) {
	(void) arg;
	sOcrN = lingoOcrRead(sOcr, sLuma, sLumaW, sLumaH, sOcrLines, 16);
	sOcrDone = 1;
	sOcrBusy = 0;
	return NULL;
}

static bool ocrEnsure(void) {
	if (sOcr || sOcrTried) {
		return sOcr != NULL;
	}
	sOcrTried = true;
	sOcr = lingoOcrLoad("app0:lingo/glyphs.bin");
	if (sOcr) {
		lingoOcrSetScorer(sOcr, coverage, NULL);
		char cp[256];
		ocrCachePath(cp, sizeof(cp));
		lingoOcrSetCache(sOcr, cp);
	}
	return sOcr != NULL;
}

/* One step of screen reading. Lines that sit together (a text box) make one
 * message; it is reported once it stops changing. */
static bool ocrStep(struct LingoText* out) {
	if (sFrame < 90 || !sFramePx || !ocrEnsure()) {
		return false;
	}
	bool got = false;
	if (sOcrDone) {
		sOcrDone = 0;
		/* group accepted lines into boxes */
		uint32_t hashes[16];
		int nh = 0;
		int bestFirst = -1, bestLast = -1, bestLetters = 0;
		uint32_t bestHash = 0;
		int i = 0;
		while (i < sOcrN) {
			if (!sOcrLines[i].accepted) {
				++i;
				continue;
			}
			int j = i;
			uint32_t h = sOcrLines[i].hash;
			int letters = sOcrLines[i].len;
			while (j + 1 < sOcrN && sOcrLines[j + 1].accepted &&
			       abs(sOcrLines[j + 1].x - sOcrLines[i].x) <= 3 * sOcrLines[i].pitch &&
			       sOcrLines[j + 1].y - (sOcrLines[j].y + sOcrLines[j].h) <= sOcrLines[j].h * 5 / 2) {
				++j;
				h = h * 31 + sOcrLines[j].hash;
				letters += sOcrLines[j].len;
			}
			if (nh < 16) {
				hashes[nh++] = h;
			}
			/* prefer a box that changed since the last look, then the biggest */
			bool fresh = true;
			for (int k = 0; k < sOcrNPrev; ++k) {
				if (sOcrPrev[k] == h) {
					fresh = false;
				}
			}
			int score = letters + (fresh ? 1000 : 0) + (h == sOcrCand ? 2000 : 0);
			if (score > bestLetters) {
				bestLetters = score;
				bestFirst = i;
				bestLast = j;
				bestHash = h;
			}
			i = j + 1;
		}
		memcpy(sOcrPrev, hashes, sizeof(uint32_t) * (size_t) nh);
		sOcrNPrev = nh;
		if (bestFirst >= 0) {
			if (bestHash == sOcrCand) {
				++sOcrStable;
			} else {
				sOcrCand = bestHash;
				sOcrStable = 0;
			}
			if (sOcrStable >= 1 && bestHash != sOcrLast) {
				sOcrLast = bestHash;
				memset(out, 0, sizeof(*out));
				for (int k = bestFirst; k <= bestLast; ++k) {
					if (!sOcrLines[k].accepted) {
						continue;
					}
					if (out->len && out->len < LINGO_MAX_TEXT) {
						out->cp[out->len++] = '\n';
					}
					for (int c = 0; c < sOcrLines[k].len && out->len < LINGO_MAX_TEXT; ++c) {
						out->cp[out->len++] = sOcrLines[k].cp[c];
					}
				}
				out->start = 0xFD000000u | (uint32_t) (sOcrLines[bestFirst].y & 0xFFFF);
				out->hash = bestHash;
				out->hasCodes = false;
				sOcrSeenAt = sFrame;
				got = true;
			}
		}
	}
	/* look again a few times a second, in the background */
	if (!sOcrBusy && sFrame % 10 == 0 && grabLuma()) {
		sOcrBusy = 1;
		pthread_t th;
		if (pthread_create(&th, NULL, ocrThread, NULL) == 0) {
			pthread_detach(th);
			sOcrThread = th;
		} else {
			ocrThread(NULL);
		}
	}
	if (sFrame % 3600 == 0 && !sOcrBusy) {
		sceIoMkdir("ux0:data/lingo/glyphs", 0777);
		lingoOcrSaveCache(sOcr);
	}
	return got;
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

/* Share of a line's Japanese letters that make dictionary words. */
static float knownShare(const struct LingoText* t, int* letters) {
	int n = 0;
	for (int i = 0; i < t->len; ++i) {
		uint32_t c = t->cp[i];
		n += (c >= 0x3041 && c <= 0x30FA) || (c >= 0x4E00 && c <= 0x9FFF);
	}
	*letters = n;
	const struct LingoDict* d = lingoDictGet();
	if (!d || !n) {
		return 0;
	}
	static struct LingoToken tok[LINGO_MAX_TOKENS];
	int k = lingoTokenize(d, t->cp, t->len, tok, LINGO_MAX_TOKENS);
	int known = 0;
	for (int i = 0; i < k; ++i) {
		/* single letters match too easily to count */
		if (tok[i].nIds && tok[i].len >= 2) {
			known += tok[i].len;
		}
	}
	return (float) known / n;
}

/* A table Lingo guessed must give real Japanese. If the lines it reads are
 * mostly not words, drop it, remember not to guess for this game again, and
 * read the screen instead. Returns true when the table was dropped. */
static bool checkGuess(const struct LingoText* t) {
	int letters;
	float share = knownShare(t, &letters);
	if (letters < 4) {
		return false;
	}
	++sJudged;
	if (share >= 0.5f) {
		++sGood;
	}
	if (sGood >= 6 && sGood * 2 >= sJudged) {
		sTrusted = true;
		return false;
	}
	if (sJudged >= 8 && sGood * 3 < sJudged) {
		char path[256], na[280];
		tablePath(sKey, path, sizeof(path));
		sceIoRemove(path);
		noAutoPath(na, sizeof(na));
		sceIoMkdir("ux0:data/lingo", 0777);
		sceIoMkdir(TABLES, 0777);
		FILE* f = fopen(na, "w");
		if (f) {
			fprintf(f, "the guessed table read %d of %d lines as real words\n", sGood, sJudged);
			fclose(f);
		}
		lingoReaderFree(&sReader);
		sHave = false;
		sNoAuto = true;
		snprintf(sLastWhy, sizeof(sLastWhy), "Its memory didn't read as real words, so Lingo is reading the screen.");
		return true;
	}
	return false;
}

bool lingoTextStep(struct LingoText* out) {
	++sFrame;
	if (sHave) {
		bool got = lingoReaderStep(&sReader, &sRegions, out);
		if (got && !sTrusted && checkGuess(out)) {
			return false;
		}
		return got;
	}
	/* no table yet: read the screen meanwhile, and keep learning from memory */
	bool screen = ocrStep(out);
	if (!sDetect || !sRegions.n || sBusy) {
		return screen;
	}
	if (sResult == 1) {
		/* the thread found a table: take it */
		sResult = 0;
		memcpy(sTable, sFound, sizeof(*sTable));
		saveTable();
		useTable();
		return screen;
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
	return screen;
}

const char* lingoTextStatus(float* progress, bool* reading) {
	*progress = 0;
	*reading = sHave;
	if (sHave) {
		snprintf(sStatus, sizeof(sStatus), "Reading text · %s", sTable->source[0] ? sTable->source : "table");
		*progress = 1;
	} else if (sOcr && sOcrSeenAt && sFrame - sOcrSeenAt < 3600) {
		*reading = true;
		*progress = 1;
		snprintf(sStatus, sizeof(sStatus), "Reading the screen · %d letters learned", lingoOcrLearned(sOcr));
	} else if (sNoAuto && sOcr) {
		*reading = true;
		*progress = 1;
		snprintf(sStatus, sizeof(sStatus), "Reading the screen · waiting for text");
	} else if (sDetect) {
		*progress = lingoDetectConfidence(sDetect);
		snprintf(sStatus, sizeof(sStatus), sBusy ? "Checking what it has seen…" : "Learning how this game writes…");
	} else {
		snprintf(sStatus, sizeof(sStatus), "Text reading is off");
	}
	return sStatus;
}

void lingoTextIgnore(uint32_t start) {
	if (!sHave || (start >> 24) >= LR_MAX_REGIONS) {
		return;
	}
	lingoTableIgnore(sTable, start);
	saveTable();
}

bool lingoTextFix(uint16_t code, uint32_t cp) {
	if (!sHave || code == 0xFFFF) {
		return false;
	}
	lingoTableFix(sTable, code, cp);
	snprintf(sTable->source, sizeof(sTable->source), "%s", "corrected");
	saveTable();
	return true;
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
		} else if (ocrEnsure()) {
			/* not in memory: label the letters on screen instead */
			char why2[200];
			ocrWait();
			if (grabLuma() && lingoOcrTeach(sOcr, sLuma, sLumaW, sLumaH, typed, n, why2, sizeof(why2))) {
				sceIoMkdir("ux0:data/lingo/glyphs", 0777);
				lingoOcrSaveCache(sOcr);
				snprintf(why, sizeof(why), "Not in memory, so Lingo will read this game from the screen. %s", why2);
				ok = true;
				n = 0;
			}
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
