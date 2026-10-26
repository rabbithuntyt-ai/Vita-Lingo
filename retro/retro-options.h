/* Lingo libretro front end: core options (the emulator's own settings).
 *
 * Cores describe their settings in one of three libretro formats
 * (SET_VARIABLES, SET_CORE_OPTIONS, SET_CORE_OPTIONS_V2); they are kept
 * here in one shape, shown in the menu and saved per core in
 * ux0:data/lingo/options/<core>.cfg. */
#ifndef RETRO_OPTIONS_H
#define RETRO_OPTIONS_H

#include "libretro.h"

#include <stdbool.h>

#define RO_MAX 160
#define RO_MAX_VALUES 64

struct RetroOption {
	char key[64];
	char desc[80];
	char info[160];
	int nValues;
	char* values[RO_MAX_VALUES]; /* value strings */
	char* labels[RO_MAX_VALUES]; /* what to show (value if no label) */
	int current;
	int def;
	bool visible;
};

struct RetroOptions {
	struct RetroOption opt[RO_MAX];
	int n;
	bool changed; /* for GET_VARIABLE_UPDATE */
	char path[256];
};

void roInit(struct RetroOptions* o, const char* savePath);
void roFree(struct RetroOptions* o);
/* From each libretro format. */
void roSetVariables(struct RetroOptions* o, const struct retro_variable* vars);
void roSetDefinitions(struct RetroOptions* o, const struct retro_core_option_definition* defs);
void roSetV2(struct RetroOptions* o, const struct retro_core_options_v2* v2);
void roSetVisible(struct RetroOptions* o, const char* key, bool visible);
/* The current value for GET_VARIABLE, or NULL. */
const char* roGet(const struct RetroOptions* o, const char* key);
/* Step an option to its next / previous value; saves. */
void roStep(struct RetroOptions* o, int index, int dir);
void roReset(struct RetroOptions* o);
bool roLoad(struct RetroOptions* o);
bool roSave(const struct RetroOptions* o);

#endif
