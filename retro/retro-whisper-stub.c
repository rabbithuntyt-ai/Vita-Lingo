/* Lingo libretro front end: no speech recognition (consoles without voice acting). */
#include "retro-whisper.h"

#include <stdio.h>

bool lingoWhisperAvailable(void) {
	return false;
}

int lingoWhisperTranscribe(const int16_t* pcm16k, int n, const char* prompt, char* out, int outLen, volatile int* progress) {
	(void) pcm16k;
	(void) prompt;
	(void) n;
	(void) progress;
	snprintf(out, (size_t) outLen, "Speech recognition isn't included for this console.");
	return -1;
}

void lingoWhisperUnload(void) {
}

const char* lingoWhisperModel(void) {
	return "";
}
