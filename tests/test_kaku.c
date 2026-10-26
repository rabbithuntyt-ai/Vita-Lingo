/* Host tests for the Kaku stroke matcher, run over every character. */
#include "../src/psp2-kaku.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static unsigned rnd = 12345;
static float jitter(float amp) { rnd = rnd * 1103515245 + 12345; return ((rnd >> 8) % 2001 / 1000.0f - 1.0f) * amp; }

/* A "hand-drawn" copy of a stroke: slightly scaled and shifted, wobbly, sampled unevenly. */
static int handDraw(const struct KakuPt* ref, int n, struct KakuPt* out, bool reverse) {
	int k = 0;
	for (int i = 0; i < n; i += 1 + (i % 3 == 0)) {
		struct KakuPt p = ref[reverse ? n - 1 - i : i];
		out[k++] = (struct KakuPt) { 54.5f + (p.x - 54.5f) * 0.94f + 1.5f + jitter(0.8f), 54.5f + (p.y - 54.5f) * 0.96f - 1.0f + jitter(0.8f) };
	}
	struct KakuPt last = ref[reverse ? 0 : n - 1];
	out[k++] = (struct KakuPt) { 54.5f + (last.x - 54.5f) * 0.94f + 1.5f, 54.5f + (last.y - 54.5f) * 0.96f - 1.0f };
	return k;
}

int main(void) {
	static struct KakuPt buf[4096];
	int total = 0, good = 0, revTotal = 0, revFlag = 0, skipTotal = 0, skipCaught = 0, scribbleFalse = 0;
	char missed[2048] = "";
	for (int pass = 0; pass < 2; ++pass) {
		int count = pass ? KAKU_KANJI_COUNT : KAKU_KANA_COUNT;
		for (int ci = 0; ci < count; ++ci) {
			const char* ch = pass ? KAKU_KANJI[ci].ch : KAKU_KANA[ci].ch;
			int first = pass ? KAKU_KANJI[ci].firstStroke : KAKU_KANA[ci].firstStroke;
			int ns = pass ? KAKU_KANJI[ci].nStrokes : KAKU_KANA[ci].nStrokes;
			struct KakuChar* c = kakuCharLoad(first, ns);
			bool allOk = true;
			for (int s = 0; s < ns; ++s) {
				int n;
				const struct KakuPt* ref = kakuCharStroke(c, s, &n);
				int k = handDraw(ref, n, buf, false);
				struct KakuResult r = kakuCheck(c, buf, k, s, KAKU_LENIENCY_NORMAL, false);
				++total;
				if (r.isMatch) ++good; else allOk = false;
				/* backwards */
				float chord = hypotf(ref[0].x - ref[n - 1].x, ref[0].y - ref[n - 1].y);
				if (chord > 15) {
					k = handDraw(ref, n, buf, true);
					r = kakuCheck(c, buf, k, s, KAKU_LENIENCY_NORMAL, false);
					++revTotal;
					if (!r.isMatch && r.backwards) ++revFlag;
				}
				/* drawing the next stroke too early */
				if (s + 1 < ns) {
					int n2;
					const struct KakuPt* ref2 = kakuCharStroke(c, s + 1, &n2);
					k = handDraw(ref2, n2, buf, false);
					r = kakuCheck(c, buf, k, s, KAKU_LENIENCY_NORMAL, false);
					++skipTotal;
					if (!r.isMatch) ++skipCaught;
				}
			}
			/* a zigzag across the box should not pass as stroke 0 */
			int k = 0;
			for (int i = 0; i < 40; ++i) buf[k++] = (struct KakuPt) { 10 + i * 2.2f, (i % 2) ? 20 : 90 };
			if (kakuCheck(c, buf, k, 0, KAKU_LENIENCY_NORMAL, false).isMatch) ++scribbleFalse;
			if (!allOk && strlen(missed) < 2000) strcat(missed, ch);
			kakuCharFree(c);
		}
	}
	printf("strokes accepted:        %d / %d\n", good, total);
	printf("backwards flagged:       %d / %d\n", revFlag, revTotal);
	printf("early strokes rejected:  %d / %d\n", skipCaught, skipTotal);
	printf("zigzags accepted:        %d / %d\n", scribbleFalse, KAKU_KANA_COUNT + KAKU_KANJI_COUNT);
	if (missed[0]) printf("characters with a rejected correct stroke: %s\n", missed);
	if (good < total * 0.98) { printf("FAIL acceptance\n"); ++failures; }
	if (revFlag < revTotal * 0.9) { printf("FAIL backwards detection\n"); ++failures; }
	if (skipCaught < skipTotal * 0.85) { printf("FAIL order detection\n"); ++failures; }
	if (scribbleFalse > 2) { printf("FAIL zigzags\n"); ++failures; }

	/* scheduling */
	struct KakuCard card;
	kakuCardInit(&card, 100);
	kakuCardGrade(&card, KAKU_GOOD, 100);
	int d1 = card.due;
	kakuCardGrade(&card, KAKU_GOOD, d1);
	int d2 = card.due;
	kakuCardGrade(&card, KAKU_GOOD, d2);
	printf("good, good, good: due +%d, +%d, +%d\n", d1 - 100, d2 - d1, (int) card.due - d2);
	if (!(d1 - 100 == 1 && d2 - d1 == 3 && (int) card.due - d2 >= 7)) { printf("FAIL schedule\n"); ++failures; }
	kakuCardGrade(&card, KAKU_AGAIN, card.due);
	if (card.interval != 0 || card.lapses != 1) { printf("FAIL lapse\n"); ++failures; }
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
