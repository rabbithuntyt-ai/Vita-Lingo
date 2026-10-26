/* Lingo: text drawing (FreeType rasterised, cached in our own atlas pages). */
#ifndef PSP2_LINGO_FONT_H
#define PSP2_LINGO_FONT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct UiFont;

/* Loads a TrueType/OpenType file. Characters missing from one loaded font
 * are drawn from another loaded font. */
struct UiFont* uiFontLoad(const char* path);
void uiFontFree(struct UiFont* font);
/* y is the baseline; em is the font size in pixels. Returns the advance width. */
float uiFontDraw(struct UiFont* font, float x, float y, float em, uint32_t color, const char* s);
float uiFontWidth(struct UiFont* font, float em, const char* s);
float uiFontAscent(struct UiFont* font, float em);

#endif
