# Lingo for PS Vita

Play Game Boy Advance games in Japanese and learn the words as you go, fully offline.

Lingo is [mGBA](https://mgba.io)'s Vita port with two things added:

- **A study panel.** The game sits at 2x on the left. Whenever someone talks, their line appears on the right, split into words. Tap a word for its dictionary entry, including what form a verb or adjective is in (たべなかった ← negative, past). Star it to keep it.
- **Kaku practice.** Write hiragana, katakana and 79 everyday kanji on the touchscreen, with every stroke checked for shape, direction and order. There are also reading quizzes with look-alike traps, and spaced review of the words you starred in games.

![LiveArea](art/pic0.png)

## Install

1. Download `Lingo.vpk` from the latest [release](../../releases).
2. Install it with VitaShell.
3. Put your own GBA cartridge dumps anywhere on `ux0:` and pick one from the file browser. No games are included.

The first time a game runs, Lingo uses the **Study panel** screen mode. Square cycles screen modes, and the setting is under mGBA's Screen mode option.

## Which games show text

The panel reads text straight from the game's memory, with no OCR and no internet. It understands the text engine and Japanese character set of the Pokémon Gen 3 games:

| Game | Code |
| --- | --- |
| ポケットモンスター ルビー / サファイア | AXVJ / AXPJ |
| ポケットモンスター エメラルド | BPEJ |
| ポケットモンスター ファイアレッド / リーフグリーン | BPRJ / BPGJ |

Those games use only kana with spaces between phrases, which suits a beginner. Other games will run normally, but the panel will most likely stay empty.

How it finds the text: every frame it looks through RAM for a pointer that steps through Japanese text one character at a time, which is the game's text printer typing a message. It then decodes the whole message. Nothing is hard-coded to a particular game version.

## Where things are saved

| File | What |
| --- | --- |
| `ux0:data/lingo/saved-words.tsv` | Starred words with meaning and the line they came from. Imports straight into Anki. |
| `ux0:data/lingo/review.bin` | Kaku review schedule |
| `ux0:data/mGBA/` | Game saves and settings, shared with regular mGBA |

## Building

GitHub Actions builds everything on every push to `main`. It installs VitaSDK, builds the dictionary, runs the host tests, applies `patches/` to a pinned mGBA commit, and publishes `Lingo.vpk` as a release. See `ci/build.sh`.

```
src/          Lingo and Kaku (copied into mGBA's src/platform/psp2)
patches/      the changes to mGBA's Vita port
tools/        dictionary, Kaku data and art generators
tests/        host tests: text decoding, memory scanner, tokenizer, stroke matcher
data/kaku/    Kaku's stroke and kanji data
```

Run the tests on a PC:

```
python3 tools/build_dict.py jamdict.db lingo.dic
gcc -O2 -o test_lingo tests/test_lingo.c src/psp2-lingo-text.c src/psp2-lingo-dict.c && ./test_lingo lingo.dic
gcc -O2 -o test_kaku tests/test_kaku.c src/psp2-kaku-core.c src/psp2-kaku-data.c -lm && ./test_kaku
```

## Credits

See [LICENSES.txt](LICENSES.txt). mGBA (MPL 2.0), JMdict and KANJIDIC (EDRDG, CC BY-SA 4.0), KanjiVG (CC BY-SA 3.0), Klee One (OFL), and stroke matching after Hanzi Writer (MIT). Lingo and Kaku by Troy Stokoe.
