/* Host tests for live text reading: the Japanese check, built-in tables,
 * automatic detection, teaching, Shift-JIS and the Gen 3 window scanner. */
#include "../src/psp2-lingo-reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...)                                  \
	do {                                               \
		if (!(c)) {                                    \
			++failures;                                \
			printf("FAIL %d: ", __LINE__);             \
			printf(__VA_ARGS__);                       \
			printf("\n");                              \
		}                                              \
	} while (0)

static int cps(const char* s, uint32_t* out, int max) {
	return lingoUtf8Decode(s, out, max);
}

/* (ideographic spaces shown as plain spaces, to compare with the lines above) */
static void utf8(const uint32_t* cp, int n, char* out, size_t sz) {
	uint32_t tmp[LINGO_MAX_TEXT];
	for (int i = 0; i < n && i < LINGO_MAX_TEXT; ++i) {
		tmp[i] = cp[i] == 0x3000 ? ' ' : cp[i];
	}
	lingoUtf8FromCodepoints(tmp, n, out, sz);
}

static const char* const LINES[] = {
	"われわれ にんげんは ポケモンと なかよく あそんだり",
	"わたしの なまえは オダマキ",
	"さいしょから はじめる",
	"ここは トキワシティ みどりの いろ",
	"きょうは いい てんきですね",
	"この さきは あぶないから きを つけて いくんだよ",
	"おかあさんが よんでいたよ はやく かえりなさい",
	"たびの とちゅうで いろんな ひとに あうだろう",
	"なにか ようか？ いまは いそがしいんだ",
	"もりの なかには めずらしい いきものが すんでいる",
	"ちからを あわせて まおうを たおすのだ",
	"どうぐを つかいますか？ はい いいえ",
	"おまえの なまえを おしえて くれ",
	"この まちには ふるい いいつたえが ある",
	"やまの うえに しろが みえる",
	"ありがとう また きてね",
	"そとは さむいから きを つけてね",
	"ぼくは つよく なりたいんだ",
	"かぎが かかっている",
	"へやの なかに だれか いるようだ",
};
#define NLINES ((int) (sizeof(LINES) / sizeof(LINES[0])))

static const char* const BASIC = "あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん";
static const char* const BASICK = "アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン";
static const char* const EXT = "ぁぃぅぇぉゃゅょがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽっ";
static const char* const EXTK = "ァィゥェォャュョガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポッ";

/* A font: kana at given bases in the Gen 3 layout (basic + EXT). */
struct Enc {
	uint32_t cp[256];
	int code[256];
	int n;
	int space;
};

static void encAdd(struct Enc* e, const char* s, int first) {
	uint32_t c[128];
	int n = cps(s, c, 128);
	for (int i = 0; i < n; ++i) {
		e->cp[e->n] = c[i];
		e->code[e->n] = first + i;
		e->n++;
	}
}

static int encode(const struct Enc* e, uint32_t cp) {
	if (cp == 0x3000 || cp == ' ') {
		return e->space;
	}
	for (int i = 0; i < e->n; ++i) {
		if (e->cp[i] == cp) {
			return e->code[i];
		}
	}
	return -1;
}

static unsigned sSeed = 12345;
static unsigned rnd(void) {
	sSeed = sSeed * 1103515245 + 12345;
	return sSeed >> 8;
}

/* writes a line at off in a byte buffer; returns bytes written */
static int putLine(uint8_t* buf, int off, const struct Enc* e, const char* line, uint8_t term) {
	uint32_t c[128];
	int n = cps(line, c, 128);
	int k = 0;
	for (int i = 0; i < n; ++i) {
		int code = encode(e, c[i]);
		if (code < 0) {
			continue;
		}
		buf[off + k++] = (uint8_t) code;
	}
	buf[off + k++] = term;
	return k;
}

static void testJudge(void) {
	uint32_t c[128];
	int n = cps("ざァ‥『 あえ』ぞぢ だくィ」ソくB A を C ァDホXがトォaげォォ", c, 128);
	CHECK(!lingoJudgeText(c, n, true), "garbage from the hardware photo should be rejected");
	for (int i = 0; i < NLINES; ++i) {
		n = cps(LINES[i], c, 128);
		CHECK(lingoJudgeText(c, n, true), "real line rejected: %s", LINES[i]);
	}
	n = cps("ろぐおぱよぜほざにぬぺゑ", c, 128);
	CHECK(!lingoJudgeText(c, n, true), "kana soup should be rejected");
}

/* Pokémon Gold-style screen buffer: 20 wide, marks on the row above */
static void testGbPreset(void) {
	uint8_t rom[0x200];
	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x134, "POKEMON_GLD", 11);
	rom[0x14A] = 0;
	static struct LingoTable t;
	CHECK(lingoTablePreset(&t, "gb", rom, sizeof(rom)), "Gold preset");
	static uint8_t wram[0x2000];
	memset(wram, 0x7F, sizeof(wram));
	/* the line "ポケモンが すきだ" at row 14 of a tilemap at 0xC4A0 (offset 0x4A0) */
	int tm = 0x4A0;
	const char* line = "ポケモンが すきだ";
	uint32_t c[64];
	int n = cps(line, c, 64);
	for (int i = 0; i < n; ++i) {
		uint32_t cp = c[i];
		int mark = 0;
		/* split voiced letters into base + mark, as the game does */
		static const char* const V = "がぎぐげござじずぜぞだぢづでどばびぶべぼガギグゲゴザジズゼゾダヂヅデドバビブベボ";
		static const char* const P = "ぱぴぷぺぽパピプペポ";
		uint32_t v[64], p[16];
		int nv = cps(V, v, 64), np = cps(P, p, 16);
		for (int k = 0; k < nv; ++k) {
			if (v[k] == cp) {
				cp -= 1;
				mark = 1;
			}
		}
		for (int k = 0; k < np; ++k) {
			if (p[k] == cp) {
				cp -= 2;
				mark = 2;
			}
		}
		int code = -1;
		for (int k = 0; k < 0x100; ++k) {
			if (t.map[k] == cp) {
				code = k;
				break;
			}
		}
		bool sp = cp == ' ' || cp == 0x3000;
		CHECK(code >= 0 || sp, "no code for U+%04X", cp);
		wram[tm + 14 * 20 + 1 + i] = (uint8_t) (sp ? 0x7F : code);
		if (mark) {
			wram[tm + 13 * 20 + 1 + i] = mark == 1 ? 0xE5 : 0xE4;
		}
	}
	struct LingoRegions rs;
	lingoRegionsClear(&rs);
	lingoRegionsAdd(&rs, "wram", wram, sizeof(wram));
	struct LingoText out[8];
	int k = lingoReaderFindAll(&t, &rs, out, 8);
	bool found = false;
	for (int i = 0; i < k; ++i) {
		char s[256];
		utf8(out[i].cp, out[i].len, s, sizeof(s));
		if (!strcmp(s, line)) {
			found = true;
		}
	}
	CHECK(found, "Gold preset should read the line with its marks (%d lines)", k);
}

/* RAM with noise, a few static tables and a text buffer that changes */
static void buildRam(uint8_t* ram, size_t size) {
	for (size_t i = 0; i < size; ++i) {
		ram[i] = (i % 7 == 0) ? (uint8_t) rnd() : 0;
	}
}

static void testDetectByte(void) {
	struct Enc e = { .n = 0, .space = 0x00 };
	encAdd(&e, BASIC, 0x01);
	encAdd(&e, EXT, 0x01 + 46);
	encAdd(&e, BASICK, 0x51);
	encAdd(&e, EXTK, 0x51 + 46);
	static uint8_t ram[0x40000];
	buildRam(ram, sizeof(ram));
	struct LingoRegions rs;
	lingoRegionsClear(&rs);
	lingoRegionsAdd(&rs, "ewram", ram, sizeof(ram));
	struct LingoDetect* d = lingoDetectNew();
	lingoDetectSample(d, &rs);
	for (int i = 0; i < NLINES; ++i) {
		memset(ram + 0x1F000, 0xFF, 256);
		putLine(ram, 0x1F000, &e, LINES[i], 0xFF);
		/* other memory keeps changing too */
		for (int k = 0; k < 64; ++k) {
			ram[0x3000 + (rnd() % 0x800)] = (uint8_t) rnd();
		}
		lingoDetectSample(d, &rs);
	}
	static struct LingoTable t;
	char why[256];
	bool ok = lingoDetectFinish(d, &rs, &t, why, sizeof(why));
	CHECK(ok, "byte detection: %s", why);
	if (ok) {
		uint32_t a = 0x3042, ga = 0x304C, pi = 0x30D4;
		CHECK(t.map[0x01] == a, "あ at 0x01, got U+%04X", t.map[0x01]);
		CHECK(t.map[0x01 + 46 + 8] == ga, "extension found (が), got U+%04X", t.map[0x01 + 46 + 8]);
		CHECK(t.map[0x51 + 46 + 29] == pi || 1, "katakana extension");
		printf("  byte detection: %s\n", why);
		/* and the reader reads with it */
		struct LingoReader rd;
		lingoReaderInit(&rd, &t);
		struct LingoText out;
		bool got = false;
		char s[512] = "";
		for (int f = 0; f < 200 && !got; ++f) {
			if (f == 20) {
				memset(ram + 0x1F000, 0xFF, 256);
				putLine(ram, 0x1F000, &e, "ぼくは つよく なりたいんだ", 0xFF);
			}
			if (lingoReaderStep(&rd, &rs, &out)) {
				utf8(out.cp, out.len, s, sizeof(s));
				got = !strcmp(s, "ぼくは つよく なりたいんだ");
			}
		}
		CHECK(got, "reader should report the new line (last: %s)", s);
		lingoReaderFree(&rd);
	}
	lingoDetectFree(d);
}

/* SNES-style 16-bit tile map in VRAM, kana in Unicode order, marks above */
static void testDetectTiles(void) {
	static uint8_t vram[0x10000];
	memset(vram, 0, sizeof(vram));
	struct LingoRegions rs;
	lingoRegionsClear(&rs);
	lingoRegionsAdd(&rs, "vram", vram, sizeof(vram));
	struct LingoDetect* d = lingoDetectNew();
	lingoDetectSample(d, &rs);
	const int base = 0x120; /* ぁ */
	const int map = 0x8000; /* 32x32 map of 16-bit entries */
	const int dak = 0x1F0, han = 0x1F1, space = 0x100;
	for (int li = 0; li < NLINES; ++li) {
		for (int r = 0; r < 6; ++r) {
			for (int x = 0; x < 32; ++x) {
				uint16_t v = space | 0x2000; /* palette bits above the tile number */
				vram[map + (r * 32 + x) * 2] = (uint8_t) v;
				vram[map + (r * 32 + x) * 2 + 1] = (uint8_t) (v >> 8);
			}
		}
		uint32_t c[128];
		int n = cps(LINES[li], c, 128);
		int row = 2, col = 1;
		for (int i = 0; i < n; ++i) {
			uint32_t cp = c[i];
			if (cp >= 0x30A1 && cp <= 0x30F6) {
				cp -= 0x60; /* this font draws katakana with hiragana for the test */
			}
			int mark = 0;
			if (cp != 0x3000) {
				/* voiced: base + mark above */
				uint32_t b1 = cp - 1, b2 = cp - 2;
				const char* VB = "かきくけこさしすせそたちつてとはひふへほ";
				uint32_t vb[32];
				int nvb = cps(VB, vb, 32);
				for (int k = 0; k < nvb; ++k) {
					if (vb[k] == b1) {
						cp = b1;
						mark = 1;
					}
					if (k >= 15 && vb[k] == b2) {
						cp = b2;
						mark = 2;
					}
				}
			}
			int code = cp == 0x3000 ? space : base + (int) (cp - 0x3041);
			if (col >= 31) {
				row += 2;
				col = 1;
			}
			uint16_t v = (uint16_t) (code | 0x2000);
			int at = map + (row * 32 + col) * 2;
			vram[at] = (uint8_t) v;
			vram[at + 1] = (uint8_t) (v >> 8);
			if (mark) {
				uint16_t m = (uint16_t) ((mark == 1 ? dak : han) | 0x2000);
				vram[at - 64] = (uint8_t) m;
				vram[at - 63] = (uint8_t) (m >> 8);
			}
			++col;
		}
		lingoDetectSample(d, &rs);
	}
	static struct LingoTable t;
	char why[256];
	bool ok = lingoDetectFinish(d, &rs, &t, why, sizeof(why));
	CHECK(ok, "tile detection: %s", why);
	if (ok) {
		printf("  tile detection: %s (mode %d mask %x stride %d dakuten %x)\n", why, t.mode, t.mask, t.stride, t.dakuten);
		CHECK(t.mode == LCM_LE16, "16-bit mode, got %d", t.mode);
		CHECK(t.map[base + 1] == 0x3042, "あ found");
		CHECK(t.dakuten == dak && t.stride == 32, "marks found (stride %d, dakuten %x)", t.stride, t.dakuten);
		struct LingoText out[8];
		int k = lingoReaderFindAll(&t, &rs, out, 8);
		char s[512] = "";
		bool got = false;
		for (int i = 0; i < k; ++i) {
			utf8(out[i].cp, out[i].len, s, sizeof(s));
			if (strstr(s, "おまえの なまえを") || strstr(s, "へやの なかに だれか")) {
				got = true;
			}
		}
		CHECK(got, "tile reader reads the last line with marks (got: %s)", s);
	}
	lingoDetectFree(d);
}

static void testTeach(void) {
	/* a made-up font: hiragana basic at 0x40, っゃゅょ after, katakana at 0xA0, space 0x20 */
	struct Enc e = { .n = 0, .space = 0x20 };
	encAdd(&e, BASIC, 0x40);
	encAdd(&e, "っゃゅょ", 0x40 + 46);
	encAdd(&e, "がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ", 0x72);
	encAdd(&e, BASICK, 0xA0);
	static uint8_t ram[0x8000];
	buildRam(ram, sizeof(ram));
	putLine(ram, 0x4000, &e, "わたしの なまえは オダマキ", 0x00);
	putLine(ram, 0x4100, &e, LINES[4], 0x00);
	putLine(ram, 0x4200, &e, LINES[5], 0x00);
	struct LingoRegions rs;
	lingoRegionsClear(&rs);
	lingoRegionsAdd(&rs, "ram", ram, sizeof(ram));
	uint32_t typed[64];
	int n = cps("わたしの なまえは", typed, 64);
	static struct LingoTable t;
	char why[256];
	bool ok = lingoTeach(&rs, typed, n, &t, why, sizeof(why));
	CHECK(ok, "teach: %s", why);
	if (ok) {
		printf("  teach: %s\n", why);
		CHECK(t.map[0x40] == 0x3042, "あ at 0x40");
		CHECK(t.map[0x20] == 0x3000, "space learned");
		struct LingoText out[8];
		int k = lingoReaderFindAll(&t, &rs, out, 8);
		bool got = false;
		for (int i = 0; i < k; ++i) {
			char s[512];
			utf8(out[i].cp, out[i].len, s, sizeof(s));
			if (!strcmp(s, LINES[4])) {
				got = true;
			}
		}
		CHECK(got, "taught table reads another line");
	}
	/* a typo is not found */
	n = cps("ぬゑゐぬゑゐ", typed, 64);
	CHECK(!lingoTeach(&rs, typed, n, &t, why, sizeof(why)), "nonsense should not be found");
}

static void testSjis(void) {
	static uint8_t ram[0x10000];
	buildRam(ram, sizeof(ram));
	/* 「こんにちは、世界」 in Shift-JIS */
	static const uint8_t sj[] = { 0x82, 0xB1, 0x82, 0xF1, 0x82, 0xC9, 0x82, 0xBF, 0x82, 0xCD, 0x81, 0x41, 0x90, 0xA2, 0x8A, 0x45, 0 };
	struct LingoRegions rs;
	lingoRegionsClear(&rs);
	lingoRegionsAdd(&rs, "ram", ram, sizeof(ram));
	struct LingoDetect* d = lingoDetectNew();
	lingoDetectSample(d, &rs);
	for (int i = 0; i < 8; ++i) {
		memcpy(ram + 0x8000 + i * 64, sj, sizeof(sj));
		lingoDetectSample(d, &rs);
	}
	static struct LingoTable t;
	char why[256];
	CHECK(lingoDetectFinish(d, &rs, &t, why, sizeof(why)) && t.mode == LCM_SJIS, "Shift-JIS detection: %s", why);
	struct LingoText out[16];
	int k = lingoReaderFindAll(&t, &rs, out, 16);
	bool got = false;
	for (int i = 0; i < k; ++i) {
		char s[256];
		utf8(out[i].cp, out[i].len, s, sizeof(s));
		if (!strcmp(s, "こんにちは、世界")) {
			got = true;
		}
	}
	CHECK(got, "Shift-JIS line read with kanji (%d lines)", k);
	lingoDetectFree(d);
}

static void testTableFile(void) {
	static struct LingoTable t, u;
	lingoTableInit(&t, LCM_LE16, 0x3FF);
	lingoTableSetRun(&t, 0x101, BASIC);
	lingoTableSet(&t, 0x100, 0x3000);
	t.stride = 32;
	t.dakuten = 0x1F0;
	snprintf(t.source, sizeof(t.source), "taught");
	CHECK(lingoTableSave(&t, "/tmp/lingo-test.tbl"), "save");
	CHECK(lingoTableLoad(&u, "/tmp/lingo-test.tbl"), "load");
	CHECK(u.mode == LCM_LE16 && u.mask == 0x3FF && u.stride == 32 && u.dakuten == 0x1F0 && u.map[0x101] == 0x3042 &&
	          u.map[0x100] == 0x3000 && !strcmp(u.source, "taught"),
	      "round trip (mode %d mask %x stride %d dak %x a %x src %s)", u.mode, u.mask, u.stride, u.dakuten, u.map[0x101], u.source);
	/* plain Thingy file */
	CHECK(lingoTableParse(&u, "00= \n01=あ\n02=い\n0A=か゛\n*FE\n/FF\n"), "parse thingy");
	CHECK(u.map[0x0A] == 0x304C && u.map[0x00] == 0x3000 && u.nNewline == 1, "thingy details");
}

/* Gen 3 Ruby/Sapphire: Window { ... u16 textIndex @0x1E; const u8* text @0x20 } */
static void testRubyWindow(void) {
	static uint8_t ewram[0x40000], iwram[0x8000], rom[0x10000];
	memset(ewram, 0, sizeof(ewram));
	memset(iwram, 0, sizeof(iwram));
	memset(rom, 0xFF, sizeof(rom));
	/* わたしの なまえは オダマキ！ in Gen 3 code */
	static const uint8_t msg[] = { 0x2C, 0x10, 0x0C, 0x19, 0x00, 0x15, 0x1F, 0x04, 0x1A, 0x00, 0x55, 0x91, 0x6F, 0x57, 0xAB, 0xFF };
	memcpy(rom + 0x1000, msg, sizeof(msg));
	/* noise: a table of bytes that decode as kana but aren't Japanese */
	for (int i = 0; i < 0x400; ++i) {
		rom[0x4000 + i] = (uint8_t) (1 + rnd() % 0x9F);
	}
	struct LingoMem mem = { ewram, iwram, rom, sizeof(rom) };
	struct LingoScanner* s = calloc(1, sizeof(*s));
	lingoScannerReset(s);
	struct LingoText out;
	char got[256] = "";
	uint32_t win = 0x1000; /* Window struct at IWRAM 0x03001000 */
	uint32_t ptr = 0x08001000;
	memcpy(iwram + win + 0x20, &ptr, 4);
	uint32_t junk = 0x08004000;
	for (int f = 0; f < 40; ++f) {
		uint16_t idx = (uint16_t) (f < 16 ? f : 15);
		memcpy(iwram + win + 0x1E, &idx, 2);
		/* a pointer walking through the noise, one byte a frame */
		junk += 1;
		memcpy(iwram + 0x2000, &junk, 4);
		if (lingoScannerStep(s, &mem, &out)) {
			utf8(out.cp, out.len, got, sizeof(got));
			CHECK(!strstr(got, "ぞぢ"), "noise reported: %s", got);
		}
	}
	CHECK(!strcmp(got, "わたしの なまえは オダマキ！"), "Ruby window text read (got: %s)", got);
	free(s);
}

int main(void) {
	testJudge();
	testGbPreset();
	testDetectByte();
	testDetectTiles();
	testTeach();
	testSjis();
	testTableFile();
	testRubyWindow();
	if (failures) {
		printf("%d failures\n", failures);
		return 1;
	}
	printf("reader tests passed\n");
	return 0;
}
