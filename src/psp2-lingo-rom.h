/* Lingo: Pokémon names and sprites read from the player's own ROM. */
#ifndef PSP2_LINGO_ROM_H
#define PSP2_LINGO_ROM_H

#include "psp2-lingo.h"

#define LINGO_MAX_SPECIES 450

struct LingoRom {
	const uint8_t* rom;
	size_t size;
	size_t names; /* file offsets of the tables, 0 if not found */
	uint32_t pics;
	uint32_t pals;
	int nSpecies;
	bool ready;
};

/* Finds the species name, front sprite and palette tables. */
bool lingoRomIndex(const uint8_t* rom, size_t size, struct LingoRom* r);
/* Species index for a name written in katakana (as decoded from the game), or -1. */
int lingoRomFindSpecies(const struct LingoRom* r, const uint32_t* name, int len);
int lingoRomSpeciesName(const struct LingoRom* r, int species, uint32_t* out);
/* The first 64x64 frame of a species' front sprite, as ABGR pixels. */
bool lingoRomSprite(const struct LingoRom* r, int species, uint32_t* rgba);

int lingoLz77(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstMax);

#endif
