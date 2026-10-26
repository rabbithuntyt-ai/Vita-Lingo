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
	struct LingoText out;
	int reports = 0;
	char s[1024];
	uint32_t textPtr = buf, scriptPtr = 0x08000100;
	for (int f = 0; f < 400; ++f) {
		*(uint32_t*) (iwram + (frameCtr - 0x03000000)) = f;
		if (f % 3 == 0) { scriptPtr += 2 + (f % 7); *(uint32_t*) (iwram + (script - 0x03000000)) = scriptPtr; }
		*(uint32_t*) (ewram + (junk - 0x02000000)) = 0x02030000 + f * 2; /* walks zeroed memory */
		if (f >= 20 && f < 20 + msgLen * 2 && f % 2 == 0 && textPtr < buf + msgLen - 1) textPtr++;
		if (f == 200) textPtr = 0x08005000; /* a new message from ROM */
		if (f > 200 && f % 2 == 0 && textPtr < 0x08005000u + (uint32_t) m2 - 1) textPtr++;
		*(uint32_t*) (ewram + (printer - 0x02000000)) = textPtr;
		if (lingoScannerStep(&sc, &mem, &out)) {
			utf8(&out, s, sizeof s);
			printf("  frame %d: report %d at %08X: %s\n", f, reports + 1, out.start, s);
			if (reports == 0) {
				CHECK(out.start == buf, "first message start %08X", out.start);
				CHECK(strstr(s, "オダマキ") && strstr(s, "しているんだ"), "first message text %s", s);
				CHECK(f < 40, "first message found late at frame %d", f);
			} else if (reports == 1) {
				CHECK(out.start == 0x08005000, "second message start %08X", out.start);
				CHECK(!strcmp(s, "きみの　なまえは　なんて　いうの？"), "second message text %s", s);
			}
			++reports;
		}
	}
	CHECK(reports == 2, "expected 2 reports, got %d", reports);
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
	printf("scanner\n"); testScanner();
	if (argc > 1) { printf("dictionary\n"); testDict(argv[1]); }
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
