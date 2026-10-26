#!/bin/bash
# Builds mGBA's Vita port with the Lingo study layer and packs the VPK into ./out
set -euxo pipefail
export PATH="$VITASDK/bin:$PATH"
ROOT="$PWD"
MGBA_COMMIT=$(cat mgba.commit)
rm -rf build mgba out && mkdir -p out
git init -q mgba && git -C mgba remote add origin https://github.com/mgba-emu/mgba
git -C mgba fetch -q --depth 1 origin "$MGBA_COMMIT" && git -C mgba checkout -q FETCH_HEAD
if [ -d src ]; then cp -v src/* mgba/src/platform/psp2/; fi
if [ -d patches ]; then for p in patches/*.patch; do git -C mgba apply --verbose "$ROOT/$p"; done; fi
mkdir build && cd build
cmake ../mgba -DCMAKE_TOOLCHAIN_FILE=../mgba/src/platform/psp2/CMakeToolchain.vitasdk -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
ls -la
cp ./*.vpk "$ROOT/out/"
