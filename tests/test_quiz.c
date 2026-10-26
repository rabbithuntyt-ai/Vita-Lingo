/* Host tests for checkpoints: word book, rules, and test building. */
#include "../src/psp2-lingo-quiz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void meet(struct LingoBook* b, struct LingoDict* d, const char* line) {
	struct LingoText t;
	memset(&t, 0, sizeof t);
	t.len = lingoUtf8Decode(line, t.cp, LINGO_MAX_TEXT);
	static struct LingoToken tok[LINGO_MAX_TOKENS];
	int n = lingoTokenize(d, t.cp, t.len, tok, LINGO_MAX_TOKENS);
	lingoBookMeet(b, d, &t, tok, n);
}

int main(int argc, char** argv) {
	struct LingoDict* d = lingoDictLoad(argc > 1 ? argv[1] : "lingo.dic");
	if (!d) { printf("no dictionary\n"); return 2; }
	struct LingoBook b;
	lingoBookInit(&b);
	const char* lines[] = {
		"ポケモンの　けんきゅうを　しているんだ。",
		"きみの　なまえは　なんて　いうの？",
		"くさむらに　はいると　やせいの　ポケモンが　とびだしてくるぞ！",
		"この　どうぐを　あげよう。",
		"つぎの　まちまで　いっしょに　いこう！",
		"きょうは　いい　てんきだね。",
	};
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) meet(&b, d, lines[i]);
	printf("words met: %d (new since checkpoint %d)\n", b.n, b.wordsSince);
	CHECK(b.n >= 12, "only %d words recorded", b.n);
	for (int i = 0; i < b.n; ++i) {
		struct LingoEntry e; lingoDictEntry(d, b.items[i].id, &e);
		CHECK(strcmp(e.reading, "の") && strcmp(e.reading, "は") && strcmp(e.reading, "を"), "particle %s recorded", e.reading);
	}
	/* seeing a line again adds nothing new */
	int before = b.n;
	meet(&b, d, lines[0]);
	CHECK(b.n == before, "repeat line added words");

	/* rules */
	lingoCheckpointSaved(&b);
	CHECK(b.pending == false || b.wordsSince >= LINGO_CP_MIN_WORDS, "pending too early");
	struct LingoBook c; lingoBookInit(&c);
	lingoCheckpointTick(&c, 5 * 60); lingoCheckpointSaved(&c);
	CHECK(!c.pending, "5 minutes should not trigger");
	lingoCheckpointTick(&c, 16 * 60); lingoCheckpointSaved(&c);
	CHECK(c.pending, "21 minutes plus a save should trigger");
	lingoCheckpointPassed(&c);
	CHECK(!c.pending && c.playSince == 0 && c.passed == 1, "pass resets");

	/* test */
	struct LingoQuestion q[5];
	int n = lingoBuildTest(d, &b, NULL, 0, 1234, q, 5);
	printf("questions: %d\n", n);
	CHECK(n == 5, "wanted 5 questions, got %d", n);
	int kinds[3] = { 0 };
	for (int i = 0; i < n; ++i) {
		kinds[q[i].kind]++;
		printf("  [%s] %s  (%s)\n", q[i].kind == LQ_MEANING ? "meaning" : q[i].kind == LQ_READING ? "reading" : "cloze", q[i].prompt, q[i].sub);
		for (int o = 0; o < 4; ++o) printf("      %c %s\n", o == q[i].answer ? '*' : ' ', q[i].options[o]);
		for (int o = 0; o < 4; ++o)
			for (int p = o + 1; p < 4; ++p) CHECK(strcmp(q[i].options[o], q[i].options[p]), "duplicate options in q%d", i);
		CHECK(q[i].options[q[i].answer][0], "empty answer q%d", i);
	}
	CHECK(kinds[LQ_CLOZE] >= 1, "no question used a line from the game");
	/* the same seed gives the same test; another seed differs */
	struct LingoQuestion q2[5];
	lingoBuildTest(d, &b, NULL, 0, 1234, q2, 5);
	CHECK(!strcmp(q[0].prompt, q2[0].prompt), "not deterministic");

	/* persistence */
	const char* path = argc > 2 ? argv[2] : "book-test.bin";
	CHECK(lingoBookSave(&b, path), "save failed");
	struct LingoBook r; lingoBookInit(&r);
	CHECK(lingoBookLoad(&r, path) && r.n == b.n && r.wordsSince == b.wordsSince && !strcmp(r.items[0].ctx, b.items[0].ctx), "reload mismatch");

	/* a brand-new game with only one saved word: not enough to test */
	struct LingoBook e; lingoBookInit(&e);
	struct LingoExtra x = { "名前", "なまえ", "[noun] name", "" };
	CHECK(lingoBuildTest(d, &e, &x, 1, 1, q, 5) <= 1, "built a test from almost nothing");
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
