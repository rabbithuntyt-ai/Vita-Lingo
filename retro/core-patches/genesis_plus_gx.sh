#!/bin/bash
# Lingo: expose the VDP's 64 KB of video memory (font tiles and the tile maps
# on screen) as RETRO_MEMORY_VIDEO_RAM. Words are stored in the host's byte order.
set -euo pipefail
F=libretro/libretro.c
sed -i -E 's/^void \*retro_get_memory_data\(unsigned id\)/void *lingo_orig_memory_data(unsigned id)/' $F
sed -i -E 's/^size_t retro_get_memory_size\(unsigned id\)/size_t lingo_orig_memory_size(unsigned id)/' $F
grep -q lingo_orig_memory_data $F && grep -q lingo_orig_memory_size $F
cat >> $F <<'C'

/* ---- Lingo ---- */
void *lingo_orig_memory_data(unsigned id);
size_t lingo_orig_memory_size(unsigned id);
void *retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_VIDEO_RAM)
      return vram;
   return lingo_orig_memory_data(id);
}
size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_VIDEO_RAM)
      return 0x10000;
   return lingo_orig_memory_size(id);
}
C
