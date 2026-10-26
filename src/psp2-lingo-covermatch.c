/* Lingo box art: matching names against libretro thumbnail listings. */
#include "psp2-lingo-covermatch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int endsWith(const char* s, const char* suf) {
	size_t a = strlen(s), b = strlen(suf);
	if (b > a) {
		return 0;
	}
	for (size_t i = 0; i < b; ++i) {
		if (tolower((unsigned char) s[a - b + i]) != suf[i]) {
			return 0;
		}
	}
	return 1;
}

const char* lingoThumbFolder(const char* system, const char* f) {
	f = f ? f : "";
	if (!strcmp(system, "gba")) return "Nintendo - Game Boy Advance";
	if (!strcmp(system, "gb")) return endsWith(f, ".gbc") ? "Nintendo - Game Boy Color" : "Nintendo - Game Boy";
	if (!strcmp(system, "fc")) return endsWith(f, ".fds") ? "Nintendo - Family Computer Disk System" : "Nintendo - Nintendo Entertainment System";
	if (!strcmp(system, "sfc")) return "Nintendo - Super Nintendo Entertainment System";
	if (!strcmp(system, "md")) {
		if (endsWith(f, ".sms")) return "Sega - Master System - Mark III";
		if (endsWith(f, ".gg")) return "Sega - Game Gear";
		if (endsWith(f, ".sg")) return "Sega - SG-1000";
		if (endsWith(f, ".cue") || endsWith(f, ".chd")) return "Sega - Mega-CD - Sega CD";
		return "Sega - Mega Drive - Genesis";
	}
	if (!strcmp(system, "pce")) return (endsWith(f, ".cue") || endsWith(f, ".ccd") || endsWith(f, ".chd")) ? "NEC - PC Engine CD - TurboGrafx-CD" : "NEC - PC Engine - TurboGrafx 16";
	if (!strcmp(system, "ps1")) return "Sony - PlayStation";
	if (!strcmp(system, "ws")) return endsWith(f, ".wsc") ? "Bandai - WonderSwan Color" : "Bandai - WonderSwan";
	if (!strcmp(system, "ngp")) return endsWith(f, ".ngc") ? "SNK - Neo Geo Pocket Color" : "SNK - Neo Geo Pocket";
	if (!strcmp(system, "neogeo")) return "FBNeo - Arcade Games";
	if (!strcmp(system, "msx")) return endsWith(f, ".mx2") ? "Microsoft - MSX2" : "Microsoft - MSX";
	if (!strcmp(system, "vb")) return "Nintendo - Virtual Boy";
	if (!strcmp(system, "pm")) return "Nintendo - Pokemon Mini";
	if (!strcmp(system, "ss")) return "Sega - Saturn";
	return NULL;
}

static int hexv(char c) {
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

int lingoThumbParseListing(const char* html, char* out, size_t outSize) {
	size_t o = 0;
	int n = 0;
	const char* p = html;
	while ((p = strstr(p, "href=\""))) {
		p += 6;
		const char* q = strchr(p, '"');
		if (!q) {
			break;
		}
		size_t len = (size_t) (q - p);
		if (len > 4 && !strncmp(q - 4, ".png", 4) && !memchr(p, '/', len)) {
			char name[512];
			size_t k = 0;
			for (size_t i = 0; i < len - 4 && k + 1 < sizeof(name); ++i) {
				if (p[i] == '%' && i + 2 < len && hexv(p[i + 1]) >= 0 && hexv(p[i + 2]) >= 0) {
					name[k++] = (char) (hexv(p[i + 1]) * 16 + hexv(p[i + 2]));
					i += 2;
				} else if (p[i] == '&' && !strncmp(p + i, "&amp;", 5)) {
					name[k++] = '&';
					i += 4;
				} else {
					name[k++] = p[i];
				}
			}
			name[k] = 0;
			if (o + k + 2 < outSize) {
				memcpy(out + o, name, k);
				o += k;
				out[o++] = '\n';
				++n;
			}
		}
		p = q;
	}
	if (outSize) {
		out[o < outSize ? o : outSize - 1] = 0;
	}
	return n;
}

/* lowercase words only: drop (…) and […] groups, punctuation, "the" */
static void norm(const char* raw, char* out, size_t n) {
	/* "Legend of Zelda, The - …" is No-Intro's way of writing "The Legend of Zelda - …" */
	char in[512];
	size_t k = 0;
	for (const char* p = raw; *p && k + 1 < sizeof(in); ++p) {
		if (!strncmp(p, ", The", 5) && (p[5] == 0 || p[5] == ' ' || p[5] == '(')) {
			p += 4;
			continue;
		}
		in[k++] = *p;
	}
	in[k] = 0;
	/* a file extension is not part of the title */
	char* dot = strrchr(in, '.');
	if (dot && strlen(dot) <= 5 && !strchr(dot, ' ')) {
		*dot = 0;
	}
	size_t o = 0;
	int depth = 0;
	int space = 1;
	for (const char* p = in; *p && o + 1 < n; ++p) {
		unsigned char c = (unsigned char) *p;
		if (c == '+' && o && isdigit((unsigned char) out[o - 1])) {
			out[o++] = '+'; /* keep "1+2" distinct from "1 2" */
			continue;
		}
		if (c == '(' || c == '[') {
			++depth;
			continue;
		}
		if (c == ')' || c == ']') {
			if (depth) {
				--depth;
			}
			continue;
		}
		if (depth) {
			continue;
		}
		if (isalnum(c) || c >= 0x80) {
			out[o++] = (char) tolower(c);
			space = 0;
		} else if (!space) {
			out[o++] = ' ';
			space = 1;
		}
	}
	while (o && out[o - 1] == ' ') {
		--o;
	}
	out[o] = 0;
	/* "legend of zelda, the" and "the legend of zelda" both become "legend of zelda" */
	if (!strncmp(out, "the ", 4)) {
		memmove(out, out + 4, strlen(out + 4) + 1);
	}
	size_t l = strlen(out);
	if (l > 4 && !strcmp(out + l - 4, " the")) {
		out[l - 4] = 0;
	}
}

static int regionScore(const char* name) {
	if (strstr(name, "(Japan)")) return 3;
	if (strstr(name, "Japan")) return 2;
	if (strstr(name, "(USA") || strstr(name, "(World")) return 1;
	return 0;
}

bool lingoThumbBestMatch(const char* list, const char* query, char* out, size_t outSize) {
	char q[256];
	norm(query, q, sizeof(q));
	if (!q[0]) {
		return false;
	}
	int bestScore = -1;
	const char* best = NULL;
	size_t bestLen = 0;
	const char* p = list;
	while (*p) {
		const char* e = strchr(p, '\n');
		size_t len = e ? (size_t) (e - p) : strlen(p);
		char name[512], nn[512];
		if (len < sizeof(name)) {
			memcpy(name, p, len);
			name[len] = 0;
			norm(name, nn, sizeof(nn));
			int score = -1;
			if (!strcmp(nn, q)) {
				score = 100;
			} else if (strlen(q) >= 8 && !strncmp(nn, q, strlen(q)) && (nn[strlen(q)] == ' ')) {
				score = 60; /* the query is the start of a longer title */
			}
			if (score >= 0) {
				score += regionScore(name) * 5 - (strstr(name, "(Beta") || strstr(name, "(Proto") ? 30 : 0) -
				         (strstr(name, "(Rev") ? 1 : 0);
				if (score > bestScore) {
					bestScore = score;
					best = p;
					bestLen = len;
				}
			}
		}
		if (!e) {
			break;
		}
		p = e + 1;
	}
	if (!best || bestLen >= outSize) {
		return false;
	}
	memcpy(out, best, bestLen);
	out[bestLen] = 0;
	return true;
}

void lingoThumbSafe(const char* name, char* out, size_t outSize) {
	size_t o = 0;
	for (const char* p = name; *p && o + 1 < outSize; ++p) {
		out[o++] = strchr("&*/:`<>?\\|", *p) ? '_' : *p;
	}
	out[o] = 0;
}
