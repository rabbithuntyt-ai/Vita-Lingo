/* Lingo: Wi-Fi start-up and box art downloads (HTTPS, certificates checked). */
#include "psp2-lingo-covermatch.h"
#include "psp2-lingo-net.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
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

static bool httpUp(void) {
	if (sHttp) {
		return true;
	}
	if (!lingoNetUp(NULL, 0)) {
		return false;
	}
	sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP);
	sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
	sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
	sceSslInit(300 * 1024);
	sceHttpInit(1024 * 1024);
	sHttp = true;
	return true;
}

/* GET url into a malloc'd, NUL-terminated buffer. NULL on any failure,
 * including a certificate the Vita cannot verify. */
static char* httpGet(const char* url, size_t* len) {
	*len = 0;
	if (!httpUp()) {
		return NULL;
	}
	int tpl = sceHttpCreateTemplate("Lingo/1.0 (PS Vita)", SCE_HTTP_VERSION_1_1, 1);
	if (tpl < 0) {
		return NULL;
	}
	sceHttpSetResolveTimeOut(tpl, 10 * 1000000);
	sceHttpSetConnectTimeOut(tpl, 10 * 1000000);
	sceHttpSetRecvTimeOut(tpl, 20 * 1000000);
	char* data = NULL;
	int conn = sceHttpCreateConnectionWithURL(tpl, url, 0);
	int req = conn >= 0 ? sceHttpCreateRequestWithURL(conn, SCE_HTTP_METHOD_GET, url, 0) : -1;
	int status = 0;
	if (req >= 0 && sceHttpSendRequest(req, NULL, 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0 && status == 200) {
		size_t cap = 256 * 1024;
		data = malloc(cap + 1);
		while (data) {
			if (*len == cap) {
				cap *= 2;
				char* d = realloc(data, cap + 1);
				if (!d) {
					free(data);
					data = NULL;
					break;
				}
				data = d;
			}
			int r = sceHttpReadData(req, data + *len, (unsigned) (cap - *len));
			if (r < 0) {
				free(data);
				data = NULL;
				break;
			}
			if (r == 0) {
				data[*len] = 0;
				break;
			}
			*len += (size_t) r;
		}
	}
	if (req >= 0) {
		sceHttpDeleteRequest(req);
	}
	if (conn >= 0) {
		sceHttpDeleteConnection(conn);
	}
	sceHttpDeleteTemplate(tpl);
	return data;
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
