/* Kana input rules for the lookup keyboard: dakuten, handakuten, small kana. */
#ifndef PSP2_LINGO_KANA_H
#define PSP2_LINGO_KANA_H

#include <stdint.h>

/* か→が→か, は→ば→ぱ→は, う→ゔ. Returns cp unchanged if it takes no marks. */
uint32_t lingoKanaCycleMarks(uint32_t cp);
/* つ⇄っ, や⇄ゃ, あ⇄ぁ ... Returns cp unchanged if it has no small form. */
uint32_t lingoKanaToggleSmall(uint32_t cp);
/* Hiragana ⇄ katakana for one character (ー unchanged). */
uint32_t lingoKanaSwapScript(uint32_t cp);

#include <stdbool.h>
#include <stddef.h>

/* Hepburn romaji for kana (hiragana or katakana; other characters are kept
 * as they are, kanji dropped). particle: read は as wa, へ as e, を as o. */
int lingoRomaji(const uint32_t* cp, int n, bool particle, char* out, size_t size);

#endif
