/* Lingo libretro front end: games inside .zip files. */
#ifndef RETRO_ZIP_H
#define RETRO_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if the file starts like a zip archive. */
bool retroZipIs(const char* path);
/* Inflates the game inside: the first file with one of exts (".a.b"), or
 * the largest file. name gets its name. On failure why says what happened. */
bool retroZipExtract(const char* path, const char* exts, uint8_t** data, size_t* size, char* name, size_t nameLen,
                     char* why, size_t whyLen);

#endif
