/* Lingo: receive game backups from a phone over the local network.
 * A tiny HTTP server serves an upload page; the phone opens it by scanning
 * a QR code, picks files, and they are written to <root>/<system>/.
 * Plain sockets and pthreads, so it runs (and is tested) on a PC too. */
#ifndef PSP2_LINGO_UPLOAD_H
#define PSP2_LINGO_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct LingoUploadSystem {
	const char* id;
	const char* label; /* shown in the phone's picker */
	const char* exts;  /* ".a.b.c" */
};

#define LINGO_UPLOAD_RECENT 8

struct LingoUploadStatus {
	bool active;            /* a file is arriving */
	char name[128];
	char system[16];
	uint64_t received, total;
	int done;               /* files finished this session */
	char recent[LINGO_UPLOAD_RECENT][160]; /* "system/name", newest last */
	int nRecent;
	char error[128];
	int clients;            /* requests served */
};

struct LingoUpload;

/* Starts listening on port (0 = pick one). token must be in every request. */
struct LingoUpload* lingoUploadStart(int port, const char* root, const char* token,
                                     const struct LingoUploadSystem* systems, int nSystems);
int lingoUploadPort(const struct LingoUpload* u);
void lingoUploadGetStatus(struct LingoUpload* u, struct LingoUploadStatus* out);
void lingoUploadStop(struct LingoUpload* u);

/* Helpers, exposed for tests */
bool lingoUploadSafeName(const char* in, char* out, size_t n);
int lingoUrlDecode(const char* in, char* out, size_t n);

#endif
