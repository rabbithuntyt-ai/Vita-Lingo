#include "../src/psp2-lingo-covermatch.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(void) {
	const char* html =
		"<tr><td><a href=\"/Nintendo%20-%20Game%20Boy%20Advance/\">Parent</a></td></tr>"
		"<a href=\"Mother%203%20%28Japan%29.png\">Mother 3 (Japan).png</a>"
		"<a href=\"Pocket%20Monsters%20-%20Emerald%20%28Japan%29.png\">x</a>"
		"<a href=\"Pokemon%20-%20Emerald%20Version%20%28USA%2C%20Europe%29.png\">x</a>"
		"<a href=\"Pocket%20Monsters%20-%20Ruby%20%28Japan%29.png\">x</a>"
		"<a href=\"Pocket%20Monsters%20-%20Ruby%20%28Japan%29%20%28Rev%201%29.png\">x</a>"
		"<a href=\"Legend%20of%20Zelda%2C%20The%20-%20The%20Minish%20Cap%20%28USA%29.png\">x</a>"
		"<a href=\"Final%20Fantasy%20USA%20-%20Mystic%20Quest%20%28Japan%29.png\">x</a>"
		"<a href=\"Mother%201%2B2%20%28Japan%29.png\">x</a>"
		"<a href=\"Tom%20%26amp%3B%20Jerry%20%28USA%29.png\">x</a>"
		"<a href=\"readme.txt\">x</a>";
	static char list[8192];
	int n = lingoThumbParseListing(html, list, sizeof list);
	printf("parsed %d names\n", n);
	CHECK(n == 9, "parsed %d", n);
	CHECK(strstr(list, "Mother 3 (Japan)\n") != NULL, "decoding");
	char out[256];
	struct { const char* q; const char* want; } t[] = {
		{ "Mother 3 (Japan).gba", "Mother 3 (Japan)" },
		{ "Pocket Monsters - Emerald (Japan)", "Pocket Monsters - Emerald (Japan)" },
		{ "pocket monsters emerald", "Pocket Monsters - Emerald (Japan)" },
		{ "Pocket Monsters - Ruby", "Pocket Monsters - Ruby (Japan)" },
		{ "The Legend of Zelda - The Minish Cap", "Legend of Zelda, The - The Minish Cap (USA)" },
		{ "Final Fantasy USA - Mystic Quest", "Final Fantasy USA - Mystic Quest (Japan)" },
		{ "Mother 1+2", "Mother 1+2 (Japan)" },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; ++i) {
		bool ok = lingoThumbBestMatch(list, t[i].q, out, sizeof out);
		printf("  %-40s -> %s\n", t[i].q, ok ? out : "(none)");
		CHECK(ok && !strcmp(out, t[i].want), "%s", t[i].q);
	}
	CHECK(!lingoThumbBestMatch(list, "Completely Different Game", out, sizeof out), "false match");
	CHECK(!lingoThumbBestMatch(list, "Mother", out, sizeof out), "too-short prefix matched");
	lingoThumbSafe("Tom & Jerry: The Movie?", out, sizeof out);
	CHECK(!strcmp(out, "Tom _ Jerry_ The Movie_"), "safe %s", out);
	CHECK(!strcmp(lingoThumbFolder("gb", "x.gbc"), "Nintendo - Game Boy Color"), "folder");
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
