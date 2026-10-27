/* Vita Lab: editing taiHEN's config.txt (no Vita dependencies; tested by app/test_config.c). */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Is a line one of ours (or an older vitacompanion)? */
static bool ourLine(const char* line, int len) {
	for (int i = 0; i + 13 <= len; ++i) {
		if (!strncmp(line + i, "vitacompanion", 13)) {
			return true;
		}
	}
	return false;
}

/* Rewrites config text without our lines; with add, puts them back under
 * *KERNEL and *main (adding those sections if missing). */
char* labRewriteConfig(const char* in, bool add, int* outLen) {
	size_t cap = strlen(in) + 512;
	char* out = malloc(cap);
	size_t o = 0;
	bool kernelDone = !add, mainDone = !add;
	const char* p = in;
	while (*p) {
		const char* e = strchr(p, '\n');
		int len = e ? (int) (e - p + 1) : (int) strlen(p);
		int bare = len;
		while (bare && (p[bare - 1] == '\n' || p[bare - 1] == '\r')) {
			--bare;
		}
		if (!ourLine(p, bare)) {
			memcpy(out + o, p, (size_t) len);
			o += (size_t) len;
			if (!e) {
				out[o++] = '\n';
			}
			if (!kernelDone && bare == 7 && !strncmp(p, "*KERNEL", 7)) {
				o += (size_t) sprintf(out + o, "%s\n", KERNEL_PLUGIN);
				kernelDone = true;
			} else if (!mainDone && bare == 5 && !strncmp(p, "*main", 5)) {
				o += (size_t) sprintf(out + o, "%s\n", USER_PLUGIN);
				mainDone = true;
			}
		}
		p += len;
	}
	if (!kernelDone) {
		/* no *KERNEL section: start the file with one */
		char* again = malloc(cap + 64);
		int n = sprintf(again, "*KERNEL\n%s\n", KERNEL_PLUGIN);
		memcpy(again + n, out, o);
		o += (size_t) n;
		free(out);
		out = again;
	}
	if (!mainDone) {
		o += (size_t) sprintf(out + o, "*main\n%s\n", USER_PLUGIN);
	}
	out[o] = 0;
	*outLen = (int) o;
	return out;
}

