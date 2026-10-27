/* Host tests for the Lingo core: decoding, the text-pointer scanner and the tokenizer. */
#include "../src/psp2-lingo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Encode UTF-8 Japanese into Gen 3 bytes using the decoder's own table. */
static int encode(const char* s, uint8_t* out) {
	uint32_t cps[512];
	int n = lingoUtf8Decode(s, cps, 512), k = 0;
	for (int i = 0; i < n; ++i) {
		if (cps[i] == '\n') { out[k++] = 0xFE; continue; }
		int b;
		for (b = 0; b < 0xF7; ++b) if (lingoGen3Char(b) == cps[i]) break;
		if (b == 0xF7) { printf("unencodable U+%04X\n", cps[i]); exit(2); }
		out[k++] = b;
	}
	out[k++] = 0xFF;
	return k;
}

static void utf8(const struct LingoText* t, char* buf, size_t n) { lingoUtf8FromCodepoints(t->cp, t->len, buf, n); }

static void testDecode(void) {
	uint8_t b[256];
	int n = encode("ポケモン　せかいへ　ようこそ！", b);
	uint32_t cp[64];
	int len = lingoDecodeGen3(b, n, cp, 64);
	char s[256];
	lingoUtf8FromCodepoints(cp, len, s, sizeof s);
	CHECK(!strcmp(s, "ポケモン　せかいへ　ようこそ！"), "decode got %s", s);
	/* control codes: colour change (FC 01 xx) and a placeholder (FD 01) are skipped / starred */
	uint8_t c[] = { 0xFC, 0x01, 0x02, 0x1A, 0x01, 0xFD, 0x01, 0xFE, 0x0A, 0x0F, 0xFF };
	len = lingoDecodeGen3(c, sizeof c, cp, 64);
	lingoUtf8FromCodepoints(cp, len, s, sizeof s);
	CHECK(!strcmp(s, "はあ＊\nこそ"), "control codes got %s", s);
	uint8_t bad[] = { 0x01, 0x02, 0xF8, 0xFF };
	CHECK(lingoDecodeGen3(bad, sizeof bad, cp, 64) < 0, "invalid byte accepted");
	uint8_t unterminated[] = { 0x01, 0x02, 0x03 };
	CHECK(lingoDecodeGen3(unterminated, sizeof unterminated, cp, 64) < 0, "unterminated accepted");
}

/* Simulates a game: a message in an EWRAM buffer, a text printer whose
 * current-character pointer advances one byte every other frame, plus noise:
 * a frame counter, a script pointer jumping by command lengths, and a
 * pointer walking over non-text data. */
static void testScanner(void) {
	static uint8_t ewram[0x40000], iwram[0x8000], rom[0x10000];
	static struct LingoScanner sc;
	struct LingoMem mem = { ewram, iwram, rom, sizeof rom };
	lingoScannerReset(&sc);
	const uint32_t buf = 0x02021FC4, printer = 0x020201B0, frameCtr = 0x03001000, script = 0x03000E00, junk = 0x02010000;
	uint8_t msg[512];
	int msgLen = encode("やあ！　ぼくは　オダマキ。\nポケモンの　けんきゅうを　しているんだ。", msg);
	memset(ewram + (buf - 0x02000000) - 8, 0xFF, 8);
	memcpy(ewram + (buf - 0x02000000), msg, msgLen);
	/* ROM: some script bytecode with 4-byte pointers, and a second message */
	for (int i = 0; i < 0x1000; ++i) rom[i] = (uint8_t) (i * 37 + 11);
	uint8_t msg2[256];
	int m2 = encode("きみの　なまえは　なんて　いうの？", msg2);
	rom[0x4FFF] = 0xFF;
	memcpy(rom + 0x5000, msg2, m2);
	/* the printer struct around the pointer: windowId, fontId, no callback, active */
	ewram[printer - 0x02000000 + 4] = 0;
	ewram[printer - 0x02000000 + 5] = 1;
	ewram[printer - 0x02000000 + 0x1B] = 1;
	/* a decoy: a music-track-like pointer walking a byte at a time over bytes
	 * that read as kana, outside any text printer. It must never be reported. */
	const uint32_t decoy = 0x03007100;
	uint8_t msgD[256];
	int mD = encode("ここは　ちがう　ぶんしょう　です。", msgD);
	rom[0x6FFF] = 0xFF;
	memcpy(rom + 0x7000, msgD, mD);
	uint32_t decoyPtr = 0x08007000;
	struct LingoText out;
	int reports = 0, pages = 0;
	char s[1024], last1[1024] = "", last2[1024] = "";
	uint32_t firstStart = 0, lastStart = 0;
	int firstFrame = -1;
	uint32_t textPtr = buf, scriptPtr = 0x08000100;
	for (int f = 0; f < 400; ++f) {
		*(uint32_t*) (iwram + (frameCtr - 0x03000000)) = f;
		if (f % 3 == 0) { scriptPtr += 2 + (f % 7); *(uint32_t*) (iwram + (script - 0x03000000)) = scriptPtr; }
		*(uint32_t*) (ewram + (junk - 0x02000000)) = 0x02030000 + f * 2; /* walks zeroed memory */
		if (f >= 20 && f < 20 + msgLen * 2 && f % 2 == 0 && textPtr < buf + msgLen - 1) textPtr++;
		if (f == 200) textPtr = 0x08005000; /* a new message from ROM */
		if (f > 200 && f % 2 == 0 && textPtr < 0x08005000u + (uint32_t) m2 - 1) textPtr++;
		*(uint32_t*) (ewram + (printer - 0x02000000)) = textPtr;
		if (decoyPtr < 0x08007000u + (uint32_t) mD - 1) decoyPtr++;
		else decoyPtr = 0x08007000;
		*(uint32_t*) (iwram + (decoy - 0x03000000)) = decoyPtr;
		if (lingoScannerStep(&sc, &mem, &out)) {
			utf8(&out, s, sizeof s);
			CHECK(!strstr(s, "ちがう"), "decoy pointer reported: %s", s);
			/* the box types out live: one report per change, same start while it grows */
			if (out.start != lastStart) {
				++pages;
				lastStart = out.start;
				if (pages == 1) { firstStart = out.start; firstFrame = f; }
			}
			snprintf(pages == 1 ? last1 : last2, sizeof last1, "%s", s);
			if (getenv("SCAN_VERBOSE")) printf("    f%d %08X %s\n", f, out.start, s);
			++reports;
		}
	}
	printf("  %d live updates over %d messages\n  last of first: %s\n  last of second: %s\n", reports, pages, last1, last2);
	CHECK(pages == 2, "expected 2 messages, got %d", pages);
	CHECK(firstStart == buf, "first message start %08X", firstStart);
	CHECK(firstFrame >= 0 && firstFrame < 30, "first message shown late at frame %d", firstFrame);
	CHECK(!strcmp(last1, "やあ！　ぼくは　オダマキ。\nポケモンの　けんきゅうを　しているんだ。"), "first message as the box shows it: %s", last1);
	CHECK(!strcmp(last2, "きみの　なまえは　なんて　いうの？"), "second message as the box shows it: %s", last2);
	CHECK(reports > 20, "should update as it types (%d updates)", reports);

	/* a player mashing A: three characters type, then the rest appears at once */
	uint8_t msg3[256];
	int m3 = encode("さあ　いこう！", msg3);
	rom[0x5FFF] = 0xFF;
	memcpy(rom + 0x6000, msg3, m3);
	textPtr = 0x08006000;
	*(uint32_t*) (ewram + (printer - 0x02000000)) = textPtr;
	lingoScannerStep(&sc, &mem, &out);
	bool got = false;
	for (int f = 0; f < 10; ++f) {
		if (f < 3) textPtr++;
		else textPtr = 0x08006000 + m3 - 1;
		*(uint32_t*) (ewram + (printer - 0x02000000)) = textPtr;
		if (lingoScannerStep(&sc, &mem, &out)) {
			utf8(&out, s, sizeof s);
			got = !strcmp(s, "さあ　いこう！");
		}
	}
	CHECK(got, "skipped message not captured");
}


/* Ruby/Sapphire: a Window struct whose textIndex walks while its text pointer
 * stays put; then a second message printed into the same buffer. */
static void testRubyWindow(void) {
	static uint8_t ewram[0x40000], iwram[0x8000], rom[0x10000];
	static struct LingoScanner sc;
	struct LingoMem mem = { ewram, iwram, rom, sizeof rom };
	lingoScannerReset(&sc);
	const uint32_t win = 0x02030100, buf = 0x02021FC4;
	uint8_t* w = ewram + (win - 0x02000000);
	w[0] = 1; w[1] = 1; w[2] = 1;
	*(uint32_t*) (w + 0x20) = buf;
	*(uint32_t*) (w + 0x24) = 0x06000000;
	*(uint32_t*) (w + 0x28) = 0x0600E800;
	*(uint32_t*) (w + 0x2C) = 0x08000400;
	const char* texts[2] = { "おかあさん：　あら　おかえり！\nひっこしの　にもつは　かたづいた？", "２かいの　じぶんの　へやも\nみてきたら？" };
	char last[2][1024] = { "", "" };
	int which = -1, f = 0;
	struct LingoText out;
	char s[1024];
	for (int m = 0; m < 2; ++m) {
		uint8_t msg[512];
		int n = encode(texts[m], msg);
		memset(ewram + (buf - 0x02000000) - 8, 0xFF, 8);
		memcpy(ewram + (buf - 0x02000000), msg, n);
		ewram[buf - 0x02000000 + n] = 0xFF;
		for (int idx = 0; idx <= n; ++f) {
			/* nothing printing for a while between messages: the overworld */
			if (f % 2 == 0 && (f > 30 || m == 0)) ++idx;
			*(uint32_t*) (w + 0x1C) = ((uint32_t) (idx < n ? idx : n) << 16) | (uint32_t) (f & 0xFF);
			if (lingoScannerStep(&sc, &mem, &out)) {
				utf8(&out, s, sizeof s);
				snprintf(last[m], sizeof last[m], "%s", s);
				which = m;
			}
		}
		f = 0;
	}
	printf("  ruby window: %s | %s\n", last[0], last[1]);
	CHECK(which == 1, "second message in the same buffer not reported");
	CHECK(!strcmp(last[0], texts[0]), "ruby first message: %s", last[0]);
	CHECK(!strcmp(last[1], texts[1]), "ruby second message: %s", last[1]);
}

static void showTokens(const struct LingoDict* d, const char* text, const char* expect) {
	uint32_t cp[256];
	int n = lingoUtf8Decode(text, cp, 256);
	struct LingoToken tok[64];
	int nt = lingoTokenize(d, cp, n, tok, 64);
	char line[2048] = "", seg[256];
	for (int i = 0; i < nt; ++i) {
		lingoUtf8FromCodepoints(cp + tok[i].start, tok[i].len, seg, sizeof seg);
		if (tok[i].len == 1 && (cp[tok[i].start] == 0x3000)) continue;
		strcat(line, seg);
		strcat(line, "|");
	}
	printf("  %s -> %s\n", text, line);
	CHECK(!strcmp(line, expect), "tokens for %s: got %s want %s", text, line, expect);
	for (int i = 0; i < nt; ++i) {
		if (!tok[i].nIds) continue;
		struct LingoEntry e;
		lingoDictEntry(d, tok[i].ids[0], &e);
		lingoUtf8FromCodepoints(cp + tok[i].start, tok[i].len, seg, sizeof seg);
		printf("      %s = %s【%s】 %s%s%s\n", seg, e.headword, e.reading, e.gloss, tok[i].reason[0][0] ? "  <" : "", tok[i].reason[0]);
	}
}

/* All readings offered for a word, joined with '/', and the reason for the first. */
static const char* firstHead(const struct LingoDict* d, const char* word, char* reasonOut) {
	uint32_t cp[64];
	int n = lingoUtf8Decode(word, cp, 64);
	struct LingoToken tok[8];
	int nt = lingoTokenize(d, cp, n, tok, 8);
	static char buf[512];
	buf[0] = 0;
	if (nt != 1 || !tok[0].nIds) return buf;
	for (int k = 0; k < tok[0].nIds; ++k) {
		struct LingoEntry e;
		lingoDictEntry(d, tok[0].ids[k], &e);
		strcat(buf, "/"); strcat(buf, e.reading);
	}
	strcat(buf, "/");
	if (reasonOut) strcpy(reasonOut, tok[0].reason[0]);
	return buf;
}

static void testDict(const char* path) {
	struct LingoDict* d = lingoDictLoad(path);
	CHECK(d, "dictionary %s failed to load", path);
	if (!d) return;
	printf("  %zu entries\n", lingoDictEntryCount(d));
	showTokens(d, "ポケモン　せかいへ　ようこそ！", "ポケモン|せかい|へ|ようこそ|！|");
	showTokens(d, "きみの　なまえは　なんて　いうの？", "きみ|の|なまえ|は|なんて|いう|の|？|");
	showTokens(d, "ポケモンの　けんきゅうを　しているんだ。", "ポケモン|の|けんきゅう|を|している|んだ|。|");
	struct { const char* in; const char* base; } forms[] = {
		{ "たべなかった", "たべる" }, { "いきました", "いく" }, { "よんで", "よむ" }, { "きました", "くる" },
		{ "たかくない", "たかい" }, { "みたい", "みる" }, { "かえろう", "かえる" }, { "まって", "まつ" },
		{ "たたかって", "たたかう" }, { "にげられない", "にげる" }, { "がんばって", "がんばる" }, { "いって", "いく" },
	};
	for (size_t i = 0; i < sizeof forms / sizeof forms[0]; ++i) {
		char why[64] = "";
		const char* got = firstHead(d, forms[i].in, why);
		printf("  %s -> %s (%s)\n", forms[i].in, got, why);
		char want[64]; snprintf(want, sizeof want, "/%s/", forms[i].base);
		CHECK(strstr(got, want) != NULL, "%s offered %s, want %s among them", forms[i].in, got, forms[i].base);
	}
	lingoDictFree(d);
}

int main(int argc, char** argv) {
	printf("decode\n"); testDecode();
	printf("scanner\n"); testScanner(); testRubyWindow();
	if (argc > 1) { printf("dictionary\n"); testDict(argv[1]); }
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
