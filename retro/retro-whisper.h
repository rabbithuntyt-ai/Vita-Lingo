/* Lingo libretro front end: offline speech recognition (whisper.cpp). Cores
 * without voice acting link retro-whisper-stub.c instead. */
#ifndef RETRO_WHISPER_H
#define RETRO_WHISPER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A model is on the card (or already loaded). */
bool lingoWhisperAvailable(void);
/* Japanese text for 16 kHz mono audio, into out. Returns its length, or -1
 * with a reason in out. progress (0..100) is updated while it works. */
/* prompt: recent text from the game (names, style) to steer recognition; may be NULL. */
int lingoWhisperTranscribe(const int16_t* pcm16k, int n, const char* prompt, char* out, int outLen, volatile int* progress);
void lingoWhisperUnload(void);
const char* lingoWhisperModel(void);

#ifdef __cplusplus
}
#endif

#endif
