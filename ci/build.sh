#!/bin/bash
# Builds mGBA's Vita port with the Lingo study layer and packs the VPK into ./out
set -euxo pipefail
export PATH="$VITASDK/bin:$PATH"
ROOT="$PWD"
MGBA_COMMIT=$(cat mgba.commit)
rm -rf build mgba out assets && mkdir -p out assets

# 1. Host tests for the parts that don't need a Vita
JMD_URL=https://files.pythonhosted.org/packages/97/a5/075928aed2b3b70459fc1db396397dfa6714d266c143c51af9b648551a4e/jamdict_data-1.5.tar.gz
curl -sSL -o /tmp/jamdict.tgz "$JMD_URL"
echo "$(cat ci/jamdict.sha256)  /tmp/jamdict.tgz" | sha256sum -c -
tar xzf /tmp/jamdict.tgz -C /tmp jamdict_data-1.5/jamdict_data/jamdict.db.xz
xz -dc /tmp/jamdict_data-1.5/jamdict_data/jamdict.db.xz > /tmp/jamdict.db
python3 tools/build_dict.py /tmp/jamdict.db assets/lingo.dic
READER="src/psp2-lingo-reader.c src/psp2-lingo-bigram.c src/psp2-lingo-sjis.c"
gcc -O2 -Wall -o /tmp/test_lingo tests/test_lingo.c src/psp2-lingo-text.c src/psp2-lingo-dict.c $READER -lm
/tmp/test_lingo assets/lingo.dic
gcc -O2 -Wall -Wno-format-truncation -o /tmp/test_reader tests/test_reader.c src/psp2-lingo-text.c $READER -lm
/tmp/test_reader
gcc -O2 -Wall -Wno-unused-result -o /tmp/test_frontend tests/test_frontend.c retro/retro-zip.c retro/retro-options.c \
	retro/retro-rewind.c retro/retro-voice.c -lz -lm
/tmp/test_frontend
gcc -O2 -Wall -o /tmp/test_kaku tests/test_kaku.c src/psp2-kaku-core.c src/psp2-kaku-data.c -lm
/tmp/test_kaku
gcc -O2 -Wall -o /tmp/test_rom tests/test_rom.c src/psp2-lingo-rom.c src/psp2-lingo-text.c $READER -lm
/tmp/test_rom
gcc -O2 -Wall -o /tmp/test_quiz tests/test_quiz.c src/psp2-lingo-quiz.c src/psp2-lingo-dict.c src/psp2-lingo-text.c $READER -lm
/tmp/test_quiz assets/lingo.dic /tmp/lingo-book-test.bin
for t in kana retro covers; do
	extra=""; [ $t = kana ] && extra="src/psp2-lingo-kana.c src/psp2-lingo-text.c"
	[ $t = retro ] && extra="retro/retro-util.c -lm"
	[ $t = covers ] && extra="src/psp2-lingo-covermatch.c"
	gcc -O2 -Wall -o /tmp/test_$t tests/test_$t.c $extra && /tmp/test_$t
done
gcc -O2 -Wall -Wno-unused-result -o /tmp/test_upload tests/test_upload.c src/psp2-lingo-upload.c -lpthread
mkdir -p /tmp/uptest && /tmp/test_upload /tmp/uptest

# 2. Assets: font, art, licences
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/google/fonts /tmp/gfonts
git -C /tmp/gfonts sparse-checkout set --no-cone /ofl/kleeone/ /ofl/mplusrounded1c/
cp /tmp/gfonts/ofl/kleeone/KleeOne-SemiBold.ttf assets/klee.ttf
cp /tmp/gfonts/ofl/mplusrounded1c/MPLUSRounded1c-Medium.ttf assets/ui.ttf
# Mozilla's root certificates (MPL 2.0), as published by the curl project, for box art downloads
curl -sSfL -o assets/cacert.pem https://curl.se/ca/cacert.pem
grep -q "BEGIN CERTIFICATE" assets/cacert.pem
cp art/*.png assets/
cp LICENSES.txt assets/

# 3. mGBA with Lingo
git init -q mgba && git -C mgba remote add origin https://github.com/mgba-emu/mgba
git -C mgba fetch -q --depth 1 origin "$MGBA_COMMIT" && git -C mgba checkout -q FETCH_HEAD
cp -v src/*.c src/*.h mgba/src/platform/psp2/
for p in patches/*.patch; do git -C mgba apply --verbose "$ROOT/$p"; done
mkdir build && cd build
cmake ../mgba -DCMAKE_TOOLCHAIN_FILE=../mgba/src/platform/psp2/CMakeToolchain.vitasdk -DCMAKE_BUILD_TYPE=Release \
	-DLINGO_ASSETS="$ROOT/assets"
make -j"$(nproc)" 2>&1 | grep -v "^\[" || true
make -j"$(nproc)"
VPK=$(find . -name "*.vpk" | head -1)
cp "$VPK" "$ROOT/out/Lingo.vpk"
cd "$ROOT"
unzip -l out/Lingo.vpk
SIZE=$(du -h out/Lingo.vpk | cut -f1)
cat > out/NOTES.md <<NOTES
Lingo for PS Vita: mGBA with an offline Japanese study panel and Kaku practice.

Install Lingo.vpk with VitaShell. Put your GBA game dumps anywhere on ux0: and pick them from the file browser.
VPK size: $SIZE. Built from $(git rev-parse --short HEAD) on mGBA $(cut -c1-7 mgba.commit).
NOTES
