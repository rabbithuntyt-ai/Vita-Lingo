#!/bin/bash
# Lingo: let the front end see the PPU's pattern tables and nametables (the
# game's font and the letters on screen) as RETRO_MEMORY_VIDEO_RAM:
# 0x0000-0x1FFF the pattern tables as currently banked, 0x2000-0x27FF NTARAM.
set -euo pipefail
F=src/drivers/libretro/libretro.c
sed -i -E 's/^void \*retro_get_memory_data\(unsigned type\)/void *lingo_orig_memory_data(unsigned type)/' $F
sed -i -E 's/^size_t retro_get_memory_size\(unsigned type\)/size_t lingo_orig_memory_size(unsigned type)/' $F
grep -q lingo_orig_memory_data $F && grep -q lingo_orig_memory_size $F
cat >> $F <<'C'

/* ---- Lingo ---- */
#include "../../cart.h"
#include "../../ppu.h"
static uint8_t lingo_vram[0x2800];
void *lingo_orig_memory_data(unsigned type);
size_t lingo_orig_memory_size(unsigned type);
void *retro_get_memory_data(unsigned type)
{
   if (type == RETRO_MEMORY_VIDEO_RAM)
   {
      unsigned a;
      for (a = 0; a < 0x2000; a++)
         lingo_vram[a] = VPage[a >> 10] ? VPage[a >> 10][a] : 0;
      memcpy(lingo_vram + 0x2000, NTARAM, 0x800);
      return lingo_vram;
   }
   return lingo_orig_memory_data(type);
}
size_t retro_get_memory_size(unsigned type)
{
   if (type == RETRO_MEMORY_VIDEO_RAM)
      return sizeof(lingo_vram);
   return lingo_orig_memory_size(type);
}
C
