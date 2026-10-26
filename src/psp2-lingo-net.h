/* Lingo: Wi-Fi, HTTP and box art on the Vita. */
#ifndef PSP2_LINGO_NET_H
#define PSP2_LINGO_NET_H

#include <stdbool.h>
#include <stddef.h>

struct vita2d_texture;

/* Starts networking if needed; true with the Vita's address when on Wi-Fi. */
bool lingoNetUp(char* ip, size_t n);

/* Box art for a game from libretro's thumbnail server, cached on the card.
 * Never blocks: returns NULL until the picture has arrived (or if there is
 * none). name may be a title or a file name. */
struct vita2d_texture* lingoCover(const char* system, const char* name);

/* Downloads url to a file (via path.part, so a cut-off download never
 * looks complete). progress goes 0..1; set *cancel to stop. Blocks. */
bool lingoNetDownload(const char* url, const char* path, volatile float* progress, volatile int* cancel);

/* Why the last download failed (empty after a success). */
const char* lingoNetLastError(void);

#endif
