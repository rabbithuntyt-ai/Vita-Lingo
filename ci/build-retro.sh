#!/bin/bash
# Links Lingo's libretro front end with every core built into ./cores-a
# and adds them to out/Lingo.vpk as cores/<id>.self.
set -euxo pipefail
export PATH="$VITASDK/bin:$PATH"
ROOT="$PWD"
CC=arm-vita-eabi-gcc
CFLAGS="-O2 -Wall -Wno-unused-parameter -ffunction-sections -fdata-sections -D_GNU_SOURCE -I$ROOT/retro -I$ROOT/src -I$VITASDK/arm-vita-eabi/include/freetype2"
B=/tmp/retro-build; rm -rf $B && mkdir -p $B/obj $B/self/cores
for f in src/*.c retro/retro-util.c retro/retro-zip.c retro/retro-options.c retro/retro-rewind.c retro/retro-voice.c; do
	$CC $CFLAGS -c "$f" -o "$B/obj/$(basename "${f%.c}").o"
done
arm-vita-eabi-ar rcs $B/liblingo.a $B/obj/*.o

# libretro-common: statically linked cores leave it to the front end
LRC_SHA=927f51fcb35ae771568680673b0a6e8af2ac24b0
rm -rf /tmp/lrc && git init -q /tmp/lrc && git -C /tmp/lrc remote add origin https://github.com/libretro/libretro-common
git -C /tmp/lrc fetch -q --depth 1 origin $LRC_SHA && git -C /tmp/lrc checkout -q FETCH_HEAD
mkdir -p $B/lrc
for d in compat streams file vfs string rthreads encodings time lists hash memmap; do
	for f in /tmp/lrc/$d/*.c; do
		$CC -O2 -DVITA -D__vita__ -DHAVE_THREADS -DHAVE_ZLIB -I/tmp/lrc/include -c "$f" -o "$B/lrc/${d}_$(basename "${f%.c}").o" \
			2> /dev/null || echo "libretro-common: skipped $d/$(basename "$f")"
	done
done
arm-vita-eabi-ar rcs $B/libretro-common.a $B/lrc/*.o
$CC $CFLAGS -c retro/lingo-retro.c -o $B/lingo-retro.o

# whisper.cpp (MIT) for voiced lines on the CD consoles; everything else links a stub
WH=/tmp/whisper
rm -rf $WH && git clone -q --depth 1 --branch v1.5.4 https://github.com/ggerganov/whisper.cpp $WH
WFLAGS="-O3 -mcpu=cortex-a9 -mfpu=neon -mfp16-format=ieee -DNDEBUG -D_GNU_SOURCE -I$WH"
mkdir -p $B/wh
for f in ggml ggml-alloc ggml-backend ggml-quants; do
	$CC $WFLAGS -std=gnu11 -c $WH/$f.c -o $B/wh/$f.o
done
arm-vita-eabi-g++ $WFLAGS -std=gnu++11 -c $WH/whisper.cpp -o $B/wh/whisper.o
arm-vita-eabi-g++ $WFLAGS -std=gnu++11 -I$ROOT/retro -c retro/retro-whisper.cpp -o $B/wh/retro-whisper.o
arm-vita-eabi-ar rcs $B/libwhisper.a $B/wh/*.o
$CC $CFLAGS -c retro/retro-whisper-stub.c -o $B/whisper-stub.o
VOICE_CORES="pcsx_rearmed yabause mednafen_pce_fast genesis_plus_gx"
# the speech model: OpenAI Whisper "tiny" (MIT), converted and quantised by the whisper.cpp project
mkdir -p $B/self/lingo
curl -sSfL --retry 3 -o $B/self/lingo/whisper.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q5_1.bin \
	|| { echo "speech model download failed: shipping without it"; rm -f $B/self/lingo/whisper.bin; }
ls -la $B/self/lingo || true
LIBS="-lvita2d -lfreetype -lpng -lbz2 -lcurl -lzstd -lssl -lcrypto -lz -lm -lstdc++ -lpthread \
 -lSceDisplay_stub -lSceGxm_stub -lSceCtrl_stub -lSceTouch_stub -lSceAudio_stub -lSceAppMgr_stub -lSceAppUtil_stub \
 -lSceCommonDialog_stub -lSceSysmodule_stub -lScePgf_stub -lSceRtc_stub -lScePower_stub -lSceNet_stub -lSceNetCtl_stub \
 -lSceHttp_stub -lSceSsl_stub -lSceIme_stub -lSceMotion_stub -lSceKernelDmacMgr_stub"
# vita-elf-create sometimes finds no room for its module data after the code
# segment (it depends on where that segment happens to end); a little padding
# moves the boundary, so a failing link is retried with some.
for pad in 0 4096 12288; do
	printf 'const char lingo_pad_%d[%d] __attribute__((used)) = {1};\n' $pad $((pad > 0 ? pad : 1)) > $B/pad$pad.c
	$CC -O2 -c $B/pad$pad.c -o $B/pad$pad.o
done
built=0
for a in cores-a/*.a; do
	id=$(basename "$a" .a)
	ok=0
	speech=$B/whisper-stub.o
	case " $VOICE_CORES " in *" $id "*) speech=$B/libwhisper.a ;; esac
	for pad in 0 4096 12288; do
		if $CC -Wl,-q -o $B/$id.elf $B/lingo-retro.o $B/pad$pad.o -Wl,--start-group "$a" $speech $B/liblingo.a $B/libretro-common.a $LIBS -Wl,--end-group \
			> $B/$id.link.log 2>&1 && vita-elf-create $B/$id.elf $B/$id.velf >> $B/$id.link.log 2>&1 \
			&& vita-make-fself -c $B/$id.velf $B/self/cores/$id.self >> $B/$id.link.log 2>&1; then
			ok=1
			break
		fi
	done
	if [ $ok = 1 ]; then
		built=$((built+1))
		echo "linked $id (padding $pad)"
	else
		echo "LINK FAILED: $id"; tail -40 $B/$id.link.log; rm -f $B/self/cores/$id.self
	fi
done
cd $B/self && zip -0 -r "$ROOT/out/Lingo.vpk" cores lingo
cd "$ROOT"
ls -la $B/self/cores
echo "cores in VPK: $built"
echo "Cores included: $built of $(ls cores-a/*.a | wc -l)" >> out/NOTES.md
test "$built" -gt 0
