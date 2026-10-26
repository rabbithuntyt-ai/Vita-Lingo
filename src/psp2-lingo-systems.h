/* Lingo: the consoles it plays and the picker that launches them. */
#ifndef PSP2_LINGO_SYSTEMS_H
#define PSP2_LINGO_SYSTEMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct LingoSystem {
	const char* id;
	const char* jp;
	const char* en;
	const char* maker;
	const char* core; /* libretro core in app0:cores/<core>.self, NULL for mGBA */
	const char* exts; /* ".a.b.c" */
	const char* note; /* BIOS or speed note, or NULL */
	uint32_t color;
	bool slow;
};

extern const struct LingoSystem LINGO_SYSTEMS[];
extern const int LINGO_SYSTEM_COUNT;

const struct LingoSystem* lingoSystemById(const char* id);
bool lingoSystemAccepts(const struct LingoSystem* s, const char* name);

/* Console picker and ROM browser. Returns true with path set for a Game
 * Boy or GBA game (mGBA runs it); other consoles launch their core and do
 * not return. Returns false if the player backs out. */
bool lingoSystemsRun(char* path, size_t pathSize);

#endif
