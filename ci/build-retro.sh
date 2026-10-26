#!/bin/bash
# Links Lingo's libretro front end with every core built into ./cores-a
# and adds them to out/Lingo.vpk as cores/<id>.self.
set -euxo pipefail
export PATH="$VITASDK/bin:$PATH"
ROOT="$PWD"
CC=arm-vita-eabi-gcc
CFLAGS="-O2 -Wall -Wno-unused-parameter -ffunction-sections -fdata-sections -D_GNU_SOURCE -I$ROOT/retro -I$ROOT/src"
B=/tmp/retro-build; rm -rf $B && mkdir -p $B/obj $B/self/cores
for f in src/*.c retro/retro-util.c; do
	$CC $CFLAGS -c "$f" -o "$B/obj/$(basename "${f%.c}").o"
done
arm-vita-eabi-ar rcs $B/liblingo.a $B/obj/*.o
$CC $CFLAGS -c retro/lingo-retro.c -o $B/lingo-retro.o
LIBS="-lvita2d -lfreetype -lpng -lbz2 -lz -lm -lstdc++ -lpthread \
 -lSceDisplay_stub -lSceGxm_stub -lSceCtrl_stub -lSceTouch_stub -lSceAudio_stub -lSceAppMgr_stub -lSceAppUtil_stub \
 -lSceCommonDialog_stub -lSceSysmodule_stub -lScePgf_stub -lSceRtc_stub -lScePower_stub -lSceNet_stub -lSceNetCtl_stub \
 -lSceHttp_stub -lSceSsl_stub -lSceIme_stub -lSceMotion_stub -lSceKernelDmacMgr_stub"
built=0
for a in cores-a/*.a; do
	id=$(basename "$a" .a)
	if $CC -Wl,-q -Wl,--gc-sections -o $B/$id.elf $B/lingo-retro.o -Wl,--start-group "$a" $B/liblingo.a $LIBS -Wl,--end-group \
		> $B/$id.link.log 2>&1; then
		vita-elf-create $B/$id.elf $B/$id.velf > /dev/null
		vita-make-fself -c $B/$id.velf $B/self/cores/$id.self
		built=$((built+1))
		echo "linked $id"
	else
		echo "LINK FAILED: $id"; tail -40 $B/$id.link.log
	fi
done
cd $B/self && zip -0 -r "$ROOT/out/Lingo.vpk" cores
cd "$ROOT"
ls -la $B/self/cores
echo "cores in VPK: $built"
