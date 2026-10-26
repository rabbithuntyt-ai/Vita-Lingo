/* Lingo box art: matching game names against libretro's thumbnail lists. */
#ifndef PSP2_LINGO_COVERMATCH_H
#define PSP2_LINGO_COVERMATCH_H

#include <stdbool.h>
#include <stddef.h>

/* libretro's thumbnail folder for a console and file (by extension), e.g.
 * "Nintendo - Game Boy Advance". NULL if none. */
const char* lingoThumbFolder(const char* system, const char* fileName);

/* Pulls the .png names out of an Apache directory listing, URL-decoded and
 * without the extension, one per line into out. Returns how many. */
int lingoThumbParseListing(const char* html, char* out, size_t outSize);

/* Best name in the newline-separated list for a game title or file name,
 * preferring Japanese releases. Returns false if nothing is close enough. */
bool lingoThumbBestMatch(const char* list, const char* query, char* out, size_t outSize);

/* How libretro names thumbnail files: reserved characters (& * slash colon backtick < > ? backslash |) become _. */
void lingoThumbSafe(const char* name, char* out, size_t outSize);

#endif
