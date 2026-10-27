/* Lingo: Wi-Fi start-up and box art downloads.
 *
 * Downloads go through libcurl with OpenSSL and the Mozilla certificate list
 * shipped in the app (app0:lingo/cacert.pem), so certificates are checked
 * against current roots. The Vita's own HTTPS library only knows the roots
 * its firmware shipped with, which no longer cover most sites; that is why
 * box art never appeared before. */
#include "psp2-lingo-covermatch.h"
#include "psp2-lingo-net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <vita2d.h>

#define COVERS "ux0:data/lingo/covers"
#define THUMBS "https://thumbnails.libretro.com/"

static bool sNet, sHttp;
static void* sNetMem;

bool lingoNetUp(char* ip, size_t n) {
	if (!sNet) {
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
		sNetMem = malloc(1024 * 1024);
		SceNetInitParam p = { sNetMem, 1024 * 1024, 0 };
		int r = sceNetInit(&p);
		if (r < 0 && r != (int) 0x80410201) {
			return false;
		}
		sceNetCtlInit();
		sNet = true;
	}
	SceNetCtlInfo info;
	memset(&info, 0, sizeof(info));
	if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0 || !info.ip_address[0] ||
	    !strcmp(info.ip_address, "0.0.0.0")) {
		return false;
	}
	if (ip) {
		snprintf(ip, n, "%s", info.ip_address);
	}
	return true;
}

#define CA_FILE "app0:lingo/cacert.pem"

/* The root certificates, read by Lingo itself one at a time and added to
 * each connection's store from curl's SSL_CTX callback. Handing curl the
 * whole list (as a file, then as a memory blob) failed on the Vita: one
 * certificate its OpenSSL can't take aborted the lot ("error adding trust
 * anchors"). One by one, a bad one is just skipped. */
static X509** sRoots;
static int sNRoots, sNRootsBad;
static bool sRootsRead;

static void readRoots(void) {
	if (sRootsRead) {
		return;
	}
	sRootsRead = true;
	FILE* f = fopen(CA_FILE, "rb");
	if (!f) {
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* d = n > 0 ? malloc((size_t) n + 1) : NULL;
	if (!d || fread(d, 1, (size_t) n, f) != (size_t) n) {
		free(d);
		fclose(f);
		return;
	}
	fclose(f);
	d[n] = 0;
	int cap = 0;
	static const char BEGIN[] = "-----BEGIN CERTIFICATE-----", END[] = "-----END CERTIFICATE-----";
	for (char* p = strstr(d, BEGIN); p; p = strstr(p, BEGIN)) {
		char* e = strstr(p, END);
		if (!e) {
			break;
		}
		e += sizeof(END) - 1;
		BIO* bio = BIO_new_mem_buf(p, (int) (e - p));
		X509* x = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
		if (bio) {
			BIO_free(bio);
		}
		if (x) {
			if (sNRoots == cap) {
				cap = cap ? cap * 2 : 160;
				X509** r = realloc(sRoots, (size_t) cap * sizeof(*r));
				if (!r) {
					X509_free(x);
					break;
				}
				sRoots = r;
			}
			sRoots[sNRoots++] = x;
		} else {
			++sNRootsBad;
		}
		ERR_clear_error();
		p = e;
	}
	free(d);
}

static CURLcode addRoots(CURL* c, void* sslctx, void* user) {
	(void) c;
	(void) user;
	X509_STORE* store = SSL_CTX_get_cert_store((SSL_CTX*) sslctx);
	if (!store) {
		return CURLE_OK;
	}
	for (int i = 0; i < sNRoots; ++i) {
		X509_STORE_add_cert(store, sRoots[i]); /* "already there" is fine */
	}
	ERR_clear_error();
	return CURLE_OK;
}

static void tls(CURL* c) {
	readRoots();
	curl_easy_setopt(c, CURLOPT_CAINFO, NULL);
	curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
	curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, addRoots);
	curl_easy_setopt(c, CURLOPT_SSL_CTX_DATA, NULL);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
}

/* Is r a failure to check the site's certificate (as opposed to no network)? */
static bool certTrouble(CURLcode r) {
	return r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CACERT_BADFILE || r == CURLE_SSL_CERTPROBLEM ||
	       r == CURLE_SSL_CONNECT_ERROR || r == CURLE_SSL_ISSUER_ERROR;
}

/* OpenSSL looks for random numbers in /dev/urandom and the like, which the
 * Vita doesn't have, so every secure connection failed with "insufficient
 * randomness". Its random numbers come from the Vita's hardware generator
 * instead (sceKernelGetRandomNumber, at most 64 bytes a call). */
static int vitaRandom(unsigned char* out, int n) {
	while (n > 0) {
		int k = n > 64 ? 64 : n;
		if (sceKernelGetRandomNumber(out, (SceSize) k) < 0) {
			return 0;
		}
		out += k;
		n -= k;
	}
	return 1;
}

static int vitaRandomSeed(const void* buf, int n) {
	(void) buf;
	(void) n;
	return 1;
}

static int vitaRandomAdd(const void* buf, int n, double entropy) {
	(void) buf;
	(void) n;
	(void) entropy;
	return 1;
}

static int vitaRandomStatus(void) {
	return 1;
}

static RAND_METHOD sVitaRand = {
	.seed = vitaRandomSeed,
	.bytes = vitaRandom,
	.cleanup = NULL,
	.add = vitaRandomAdd,
	.pseudorand = vitaRandom,
	.status = vitaRandomStatus,
};

static void seedRandom(void) {
	RAND_set_rand_method(&sVitaRand);
	/* and seed OpenSSL's own generator too, in case something bypasses the method */
	unsigned char seed[256];
	if (vitaRandom(seed, sizeof(seed))) {
		RAND_seed(seed, sizeof(seed));
	}
	memset(seed, 0, sizeof(seed));
}

static bool httpUp(void) {
	if (sHttp) {
		return true;
	}
	if (!lingoNetUp(NULL, 0)) {
		return false;
	}
	seedRandom();
	if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
		return false;
	}
	seedRandom(); /* again: initialising OpenSSL can reset the method */
	sHttp = true;
	return true;
}

struct Buf {
	char* d;
	size_t n, cap;
	bool failed;
};

static size_t onData(void* p, size_t size, size_t count, void* user) {
	struct Buf* b = user;
	size_t len = size * count;
	if (b->n + len + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256 * 1024;
		while (cap < b->n + len + 1) {
			cap *= 2;
		}
		if (cap > 16 * 1024 * 1024) {
			b->failed = true;
			return 0;
		}
		char* d = realloc(b->d, cap);
		if (!d) {
			b->failed = true;
			return 0;
		}
		b->d = d;
		b->cap = cap;
	}
	memcpy(b->d + b->n, p, len);
	b->n += len;
	b->d[b->n] = 0;
	return len;
}

static char sNetError[160];

const char* lingoNetLastError(void) {
	return sNetError;
}

/* GET url into a malloc'd, NUL-terminated buffer. NULL on any failure,
 * including a certificate that doesn't check out. */
static char* httpGet(const char* url, size_t* len) {
	*len = 0;
	if (!httpUp()) {
		snprintf(sNetError, sizeof(sNetError), "not connected to Wi-Fi");
		return NULL;
	}
	CURL* c = curl_easy_init();
	if (!c) {
		return NULL;
	}
	struct Buf b = { NULL, 0, 0, false };
	char err[CURL_ERROR_SIZE] = "";
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 4L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Lingo/1.1 (PS Vita)");
	tls(c);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 12L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (r != CURLE_OK || code != 200 || b.failed) {
		if (certTrouble(r)) {
			snprintf(sNetError, sizeof(sNetError), "%s (%d certificates loaded, %d unreadable)", err[0] ? err : curl_easy_strerror(r),
			         sNRoots, sNRootsBad);
		} else {
			snprintf(sNetError, sizeof(sNetError), "%s (HTTP %ld)", err[0] ? err : curl_easy_strerror(r), code);
		}
		free(b.d);
		return NULL;
	}
	sNetError[0] = 0;
	*len = b.n;
	if (!b.d) {
		b.d = calloc(1, 1);
	}
	return b.d;
}

struct Dl {
	FILE* f;
	volatile float* progress;
	volatile int* cancel;
};

static size_t onFile(void* p, size_t size, size_t count, void* user) {
	struct Dl* d = user;
	return fwrite(p, size, count, d->f) * size;
}

static int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
	(void) ut;
	(void) un;
	struct Dl* d = user;
	if (d->progress && total > 0) {
		*d->progress = (float) now / (float) total;
	}
	return d->cancel && *d->cancel ? 1 : 0;
}

bool lingoNetDownload(const char* url, const char* path, volatile float* progress, volatile int* cancel) {
	if (!httpUp()) {
		snprintf(sNetError, sizeof(sNetError), "not connected to Wi-Fi");
		return false;
	}
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	struct Dl d = { fopen(tmp, "wb"), progress, cancel };
	if (!d.f) {
		snprintf(sNetError, sizeof(sNetError), "can't write to the memory card");
		return false;
	}
	CURL* c = curl_easy_init();
	char err[CURL_ERROR_SIZE] = "";
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onFile);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Lingo/1.1 (PS Vita)");
	tls(c);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1000L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	bool ok = fclose(d.f) == 0 && r == CURLE_OK && code == 200;
	if (ok) {
		sceIoRemove(path);
		ok = sceIoRename(tmp, path) >= 0;
	} else {
		snprintf(sNetError, sizeof(sNetError), "%s (HTTP %ld)", err[0] ? err : curl_easy_strerror(r), code);
		sceIoRemove(tmp);
	}
	return ok;
}

static void urlEncode(const char* in, char* out, size_t n) {
	size_t o = 0;
	for (const unsigned char* p = (const unsigned char*) in; *p && o + 4 < n; ++p) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) {
			out[o++] = (char) *p;
		} else {
			o += (size_t) snprintf(out + o, n - o, "%%%02X", *p);
		}
	}
	out[o] = 0;
}

static bool exists(const char* path) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return false;
	}
	sceIoClose(fd);
	return true;
}

static bool writeFile(const char* path, const void* data, size_t len) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	fclose(f);
	if (ok) {
		sceIoRemove(path);
		ok = sceIoRename(tmp, path) >= 0;
	}
	return ok;
}

static char* readFile(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* d = malloc(n + 1);
	if (d && fread(d, 1, n, f) == (size_t) n) {
		d[n] = 0;
	} else {
		free(d);
		d = NULL;
	}
	fclose(f);
	return d;
}

/* ---- the cache and the worker ---- */

struct Job {
	char folder[96];
	char query[200];
	char png[400];
	char none[400];
};

#define QUEUE 24
static struct Job sQueue[QUEUE];
static volatile int sQHead, sQTail;
static SceUID sQLock = -1, sWorker = -1;

static void cachePaths(const char* folder, const char* query, char* png, char* none, size_t n) {
	char safe[200];
	lingoThumbSafe(query, safe, sizeof(safe));
	snprintf(png, n, COVERS "/%s/%s.png", folder, safe);
	snprintf(none, n, COVERS "/%s/%s.none", folder, safe);
}

static void runJob(const struct Job* j) {
	char dir[200], listPath[260], enc[400], url[700];
	snprintf(dir, sizeof(dir), COVERS "/%s", j->folder);
	sceIoMkdir("ux0:data/lingo", 0777);
	sceIoMkdir(COVERS, 0777);
	sceIoMkdir(dir, 0777);
	snprintf(listPath, sizeof(listPath), "%s/_list.txt", dir);
	char* list = readFile(listPath);
	if (!list) {
		urlEncode(j->folder, enc, sizeof(enc));
		snprintf(url, sizeof(url), THUMBS "%s/Named_Boxarts/", enc);
		size_t len;
		char* html = httpGet(url, &len);
		if (!html) {
			return; /* offline, server down, or not verifiable: try again later */
		}
		list = malloc(len + 1);
		if (list) {
			lingoThumbParseListing(html, list, len + 1);
			writeFile(listPath, list, strlen(list));
		}
		free(html);
		if (!list) {
			return;
		}
	}
	char name[400];
	bool found = lingoThumbBestMatch(list, j->query, name, sizeof(name));
	free(list);
	if (!found) {
		writeFile(j->none, "", 0);
		return;
	}
	char safe[400], fenc[200];
	lingoThumbSafe(name, safe, sizeof(safe));
	urlEncode(j->folder, fenc, sizeof(fenc));
	urlEncode(safe, enc, sizeof(enc));
	snprintf(url, sizeof(url), THUMBS "%s/Named_Boxarts/%s.png", fenc, enc);
	size_t len;
	char* png = httpGet(url, &len);
	if (png && len > 8 && !memcmp(png, "\x89PNG", 4)) {
		writeFile(j->png, png, len);
	} else if (png) {
		writeFile(j->none, "", 0);
	}
	free(png);
}

static int worker(SceSize args, void* argp) {
	(void) args;
	(void) argp;
	while (true) {
		struct Job j;
		bool have = false;
		sceKernelLockMutex(sQLock, 1, NULL);
		if (sQTail != sQHead) {
			j = sQueue[sQTail % QUEUE];
			sQTail++;
			have = true;
		}
		sceKernelUnlockMutex(sQLock, 1);
		if (have) {
			runJob(&j);
		} else {
			sceKernelDelayThread(200 * 1000);
		}
	}
	return 0;
}

static void enqueue(const char* folder, const char* query, const char* png, const char* none) {
	if (sQLock < 0) {
		sQLock = sceKernelCreateMutex("lingo-covers", 0, 0, NULL);
		sWorker = sceKernelCreateThread("lingo-covers", worker, 0x10000100 + 20, 0x10000, 0, 0, NULL);
		if (sWorker >= 0) {
			sceKernelStartThread(sWorker, 0, NULL);
		}
	}
	sceKernelLockMutex(sQLock, 1, NULL);
	for (int i = sQTail; i != sQHead; ++i) {
		if (!strcmp(sQueue[i % QUEUE].png, png)) {
			sceKernelUnlockMutex(sQLock, 1);
			return;
		}
	}
	if (sQHead - sQTail < QUEUE) {
		struct Job* j = &sQueue[sQHead % QUEUE];
		snprintf(j->folder, sizeof(j->folder), "%s", folder);
		snprintf(j->query, sizeof(j->query), "%s", query);
		snprintf(j->png, sizeof(j->png), "%s", png);
		snprintf(j->none, sizeof(j->none), "%s", none);
		sQHead++;
	}
	sceKernelUnlockMutex(sQLock, 1);
}

/* a few textures kept; the least recently used goes first */
#define TEXCACHE 10
static struct {
	char png[400];
	vita2d_texture* tex;
	uint64_t used;
} sTex[TEXCACHE];
static uint64_t sClock;
static double sLastNetTry = -100;

struct vita2d_texture* lingoCover(const char* system, const char* name) {
	const char* folder = lingoThumbFolder(system, name);
	if (!folder || !name || !name[0]) {
		return NULL;
	}
	char png[400], none[400];
	cachePaths(folder, name, png, none, sizeof(png));
	++sClock;
	for (int i = 0; i < TEXCACHE; ++i) {
		if (sTex[i].tex && !strcmp(sTex[i].png, png)) {
			sTex[i].used = sClock;
			return sTex[i].tex;
		}
	}
	if (exists(png)) {
		int slot = 0;
		for (int i = 0; i < TEXCACHE; ++i) {
			if (!sTex[i].tex) {
				slot = i;
				break;
			}
			if (sTex[i].used < sTex[slot].used) {
				slot = i;
			}
		}
		if (sTex[slot].tex) {
			vita2d_wait_rendering_done();
			vita2d_free_texture(sTex[slot].tex);
		}
		sTex[slot].tex = vita2d_load_PNG_file(png);
		snprintf(sTex[slot].png, sizeof(sTex[slot].png), "%s", png);
		sTex[slot].used = sClock;
		return sTex[slot].tex;
	}
	if (exists(none)) {
		return NULL;
	}
	/* not cached: fetch in the background when on Wi-Fi (checked at most every 30 s) */
	double now = sceKernelGetProcessTimeWide() / 1e6;
	if (now - sLastNetTry > 30 || sHttp) {
		sLastNetTry = now;
		if (lingoNetUp(NULL, 0)) {
			enqueue(folder, name, png, none);
		}
	}
	return NULL;
}
