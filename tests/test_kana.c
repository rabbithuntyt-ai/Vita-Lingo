#include "../src/psp2-lingo-kana.h"
#include "../src/psp2-lingo.h"
#include <stdio.h>
#include <string.h>
static int failures;
static void expect(uint32_t (*f)(uint32_t), const char* in, const char* want) {
	uint32_t a, b; lingoUtf8Decode(in, &a, 1); lingoUtf8Decode(want, &b, 1);
	uint32_t got = f(a);
	char g[8] = { 0 }; lingoUtf8Encode(got, g);
	if (got != b) { printf("FAIL %s -> %s, want %s\n", in, g, want); ++failures; }
}
int main(void) {
	const char* marks[][2] = { { "か", "が" }, { "が", "か" }, { "ち", "ぢ" }, { "つ", "づ" }, { "づ", "つ" }, { "と", "ど" },
		{ "は", "ば" }, { "ば", "ぱ" }, { "ぱ", "は" }, { "ほ", "ぼ" }, { "ぽ", "ほ" }, { "う", "ゔ" }, { "カ", "ガ" }, { "ハ", "バ" },
		{ "ヒ", "ビ" }, { "ピ", "ヒ" }, { "あ", "あ" }, { "ん", "ん" }, { "っ", "っ" }, { "そ", "ぞ" } };
	for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; ++i) expect(lingoKanaCycleMarks, marks[i][0], marks[i][1]);
	const char* sm[][2] = { { "つ", "っ" }, { "っ", "つ" }, { "や", "ゃ" }, { "ょ", "よ" }, { "あ", "ぁ" }, { "ツ", "ッ" }, { "ヨ", "ョ" }, { "か", "か" }, { "わ", "ゎ" } };
	for (unsigned i = 0; i < sizeof sm / sizeof sm[0]; ++i) expect(lingoKanaToggleSmall, sm[i][0], sm[i][1]);
	expect(lingoKanaSwapScript, "あ", "ア"); expect(lingoKanaSwapScript, "ン", "ん"); expect(lingoKanaSwapScript, "ー", "ー");
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
