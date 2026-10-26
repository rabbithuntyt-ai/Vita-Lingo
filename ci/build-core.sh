#!/bin/bash
# Builds one libretro core as a static library for the Vita: ci/build-core.sh <id> <outdir>
set -euxo pipefail
export PATH="$VITASDK/bin:$PATH"
ID="$1"; OUT="$2"
read -r _ REPO SHA DIR MK < <(grep -v '^#' retro/cores.txt | awk -v id="$ID" '$1==id')
mkdir -p "$OUT" /tmp/core && cd /tmp/core
rm -rf "$ID" && git init -q "$ID" && cd "$ID"
git remote add origin "https://github.com/libretro/$REPO"
git fetch -q --depth 1 origin "$SHA" && git checkout -q FETCH_HEAD
git submodule update --init --recursive --depth 1 || true
cd "$DIR"
# small fix-ups some cores need for the Vita's libc
case "$ID" in
	yabause) sed -i '1i #include <limits.h>\n#ifndef PATH_MAX\n#define PATH_MAX 1024\n#endif' libretro.c ;;
esac
make -f "$MK" platform=vita -j"$(nproc)"
LIB=$(find . -maxdepth 3 -name "*_libretro_vita.a" | head -1)
test -n "$LIB"
cp "$LIB" "$OUT/$ID.a"
ls -la "$OUT"
