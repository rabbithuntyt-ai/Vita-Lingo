/* Lingo libretro front end: rewind.
 *
 * Keeps recent save states in a ring, each stored as the XOR against the
 * state before it, run-length packed (most of a state doesn't change from
 * one capture to the next, so this is small). Stepping back undoes them. */
#ifndef RETRO_REWIND_H
#define RETRO_REWIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct RetroRewind;

/* stateSize from retro_serialize_size(); budget in bytes for the ring. */
struct RetroRewind* rwNew(size_t stateSize, size_t budget);
void rwFree(struct RetroRewind* r);
/* After a frame: store the current state (call every few frames). */
void rwPush(struct RetroRewind* r, const uint8_t* state);
/* Steps back one capture: writes the older state into out. False when empty. */
bool rwPop(struct RetroRewind* r, uint8_t* out);
int rwCount(const struct RetroRewind* r);

#endif
