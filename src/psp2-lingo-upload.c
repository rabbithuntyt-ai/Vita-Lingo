/* Lingo: phone-to-Vita game upload over the local network. */
#include "psp2-lingo-upload.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

struct LingoUpload {
	int fd;
	int port;
	char root[256];
	char token[32];
	const struct LingoUploadSystem* systems;
	int nSystems;
	pthread_t thread;
	pthread_mutex_t lock;
	volatile int running;
	struct LingoUploadStatus st;
};

/* ---- helpers ---- */

int lingoUrlDecode(const char* in, char* out, size_t n) {
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 1 < n; ++i) {
		char c = in[i];
		if (c == '%' && in[i + 1] && in[i + 2]) {
			char hex[3] = { in[i + 1], in[i + 2], 0 };
			char* end;
			long v = strtol(hex, &end, 16);
			if (*end == 0) {
				out[o++] = (char) v;
				i += 2;
				continue;
			}
		}
		out[o++] = c == '+' ? ' ' : c;
	}
	out[o] = 0;
	return (int) o;
}

/* Keeps the name readable (Japanese included) but never a path. */
bool lingoUploadSafeName(const char* in, char* out, size_t n) {
	const char* base = in;
	for (const char* p = in; *p; ++p) {
		if (*p == '/' || *p == '\\' || *p == ':') {
			base = p + 1;
		}
	}
	size_t o = 0;
	for (const char* p = base; *p && o + 1 < n; ++p) {
		unsigned char c = (unsigned char) *p;
		if (c < 0x20 || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
			out[o++] = '_';
		} else {
			out[o++] = (char) c;
		}
	}
	out[o] = 0;
	/* no hidden files, no "." or "..", nothing empty */
	while (out[0] == '.') {
		memmove(out, out + 1, strlen(out));
	}
	return out[0] != 0;
}

static bool queryParam(const char* query, const char* key, char* out, size_t n) {
	size_t kl = strlen(key);
	const char* p = query;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char* v = p + kl + 1;
			size_t len = strcspn(v, "&");
			char raw[512];
			if (len >= sizeof(raw)) {
				len = sizeof(raw) - 1;
			}
			memcpy(raw, v, len);
			raw[len] = 0;
			lingoUrlDecode(raw, out, n);
			return true;
		}
		p = strchr(p, '&');
		if (p) {
			++p;
		}
	}
	return false;
}

static void sendAll(int fd, const char* data, size_t len) {
	while (len) {
		ssize_t w = send(fd, data, len, 0);
		if (w <= 0) {
			return;
		}
		data += w;
		len -= (size_t) w;
	}
}

static void respond(int fd, int code, const char* type, const char* body) {
	char head[256];
	const char* text = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 400 ? "Bad Request" : "Error";
	/* %u, not %zu: the Vita's C library prints "%zu" literally, which made
	 * "Content-Length: zu" and phones refused the page (ERR_INVALID_RESPONSE) */
	int n = snprintf(head, sizeof(head),
	                 "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n"
	                 "Cache-Control: no-store\r\n\r\n",
	                 code, text, type, (unsigned) strlen(body));
	sendAll(fd, head, n);
	sendAll(fd, body, strlen(body));
}

/* ---- the page the phone sees ---- */

static const char PAGE_HEAD[] =
	"<!doctype html><html lang=en><head><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'><title>Send to Lingo</title><style>"
	"body{margin:0;background:#1a2e25;color:#eef1e6;font:16px/1.5 system-ui,sans-serif;padding:24px 18px}"
	"h1{font-size:28px;margin:0 0 4px;color:#f1d37c}p{color:#9db2a4;margin:4px 0 18px}"
	".card{background:#20382d;border:2px solid #325246;border-radius:16px;padding:16px;margin-bottom:14px}"
	"label{display:block;color:#9db2a4;font-size:14px;margin-bottom:6px}"
	"select,input[type=file]{width:100%;font-size:16px;padding:10px;border-radius:10px;border:2px solid #4f7a67;"
	"background:#1a2e25;color:#eef1e6;box-sizing:border-box}"
	"button{width:100%;margin-top:14px;padding:14px;font-size:18px;border:0;border-radius:12px;background:#f1d37c;"
	"color:#1a2e25;font-weight:700}button:disabled{opacity:.5}"
	".bar{height:12px;background:#1a2e25;border-radius:6px;overflow:hidden;margin-top:12px}"
	".bar i{display:block;height:100%;width:0;background:#a6e3bd}"
	"#log div{padding:6px 0;border-bottom:1px solid #325246;font-size:14px}.ok{color:#a6e3bd}.bad{color:#ff8e7c}"
	"small{color:#9db2a4}</style></head><body>"
	"<h1>ことば Lingo</h1><p>Send game backups to your Vita. Use dumps of games you own.</p>"
	"<div class=card><label>Game files</label><input id=f type=file multiple>"
	"<label style='margin-top:12px'>Console</label><select id=s></select>"
	"<small id=hint></small><button id=go disabled>Send to Vita</button>"
	"<div class=bar><i id=bar></i></div><div id=now></div></div><div class=card id=log></div><script>";

static const char PAGE_TAIL[] =
	"const f=document.getElementById('f'),s=document.getElementById('s'),go=document.getElementById('go'),"
	"bar=document.getElementById('bar'),now=document.getElementById('now'),log=document.getElementById('log'),"
	"hint=document.getElementById('hint');"
	"SYS.forEach(x=>{const o=document.createElement('option');o.value=x[0];o.textContent=x[1];s.appendChild(o)});"
	"function ext(n){const i=n.lastIndexOf('.');return i<0?'':n.slice(i).toLowerCase()}"
	"function guess(n){const e=ext(n),m=SYS.filter(x=>x[2].split('.').slice(1).includes(e.slice(1)));"
	"return m}"
	"f.onchange=()=>{go.disabled=!f.files.length;if(!f.files.length)return;const m=guess(f.files[0].name);"
	"if(m.length){s.value=m[0][0];hint.textContent=m.length>1?'This file type fits more than one console. Check the one above.':'';}"
	"else hint.textContent='Unusual file type. Pick the console it belongs to.'};"
	"function send(file){return new Promise(ok=>{const x=new XMLHttpRequest();"
	"x.open('PUT','/upload?k='+TOKEN+'&sys='+encodeURIComponent(s.value)+'&name='+encodeURIComponent(file.name));"
	"x.upload.onprogress=e=>{if(e.lengthComputable)bar.style.width=(100*e.loaded/e.total)+'%'};"
	"x.onload=()=>{const d=document.createElement('div');const good=x.status==200;"
	"d.className=good?'ok':'bad';d.textContent=(good?'✓ ':'✗ ')+file.name+(good?'':' — '+x.responseText);"
	"log.prepend(d);ok()};x.onerror=()=>{const d=document.createElement('div');d.className='bad';"
	"d.textContent='✗ '+file.name+' — lost connection to the Vita';log.prepend(d);ok()};x.send(file)})}"
	"go.onclick=async()=>{go.disabled=true;for(const file of f.files){now.textContent='Sending '+file.name+'…';"
	"bar.style.width='0';await send(file)}now.textContent='Done. Pick more, or play on the Vita.';go.disabled=false};"
	"</script></body></html>";

static void servePage(struct LingoUpload* u, int fd) {
	size_t cap = sizeof(PAGE_HEAD) + sizeof(PAGE_TAIL) + 4096;
	char* page = malloc(cap);
	if (!page) {
		return;
	}
	size_t n = (size_t) snprintf(page, cap, "%sconst TOKEN='%s',SYS=[", PAGE_HEAD, u->token);
	for (int i = 0; i < u->nSystems && n < cap - 256; ++i) {
		n += (size_t) snprintf(page + n, cap - n, "%s['%s','%s','%s']", i ? "," : "", u->systems[i].id,
		                       u->systems[i].label, u->systems[i].exts);
	}
	snprintf(page + n, cap - n, "];%s", PAGE_TAIL);
	respond(fd, 200, "text/html; charset=utf-8", page);
	free(page);
}

static const struct LingoUploadSystem* findSystem(struct LingoUpload* u, const char* id) {
	for (int i = 0; i < u->nSystems; ++i) {
		if (!strcmp(u->systems[i].id, id)) {
			return &u->systems[i];
		}
	}
	return NULL;
}

static void receive(struct LingoUpload* u, int fd, const char* query, uint64_t length, const char* already, size_t alreadyLen) {
	char sys[16] = "", rawName[256] = "", name[200];
	queryParam(query, "sys", sys, sizeof(sys));
	queryParam(query, "name", rawName, sizeof(rawName));
	const struct LingoUploadSystem* target = findSystem(u, sys);
	if (!target || !lingoUploadSafeName(rawName, name, sizeof(name))) {
		respond(fd, 400, "text/plain", "unknown console or file name");
		return;
	}
	if (length == 0) {
		respond(fd, 400, "text/plain", "empty file");
		return;
	}
	char dir[400], path[640], part[660];
	if (target->dir) {
		snprintf(dir, sizeof(dir), "%s", target->dir);
	} else {
		snprintf(dir, sizeof(dir), "%s/%s", u->root, sys);
		mkdir(u->root, 0777);
	}
	mkdir(dir, 0777);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(part, sizeof(part), "%s.part", path);
	FILE* f = fopen(part, "wb");
	if (!f) {
		respond(fd, 400, "text/plain", "could not create the file on the Vita");
		return;
	}
	pthread_mutex_lock(&u->lock);
	u->st.active = true;
	snprintf(u->st.name, sizeof(u->st.name), "%s", name);
	snprintf(u->st.system, sizeof(u->st.system), "%s", sys);
	u->st.received = 0;
	u->st.total = length;
	pthread_mutex_unlock(&u->lock);

	uint64_t got = 0;
	bool ok = true;
	if (alreadyLen) {
		size_t take = alreadyLen > length ? (size_t) length : alreadyLen;
		ok = fwrite(already, 1, take, f) == take;
		got = take;
	}
	static char buf[64 * 1024];
	while (ok && got < length && u->running) {
		size_t want = length - got > sizeof(buf) ? sizeof(buf) : (size_t) (length - got);
		ssize_t r = recv(fd, buf, want, 0);
		if (r <= 0) {
			ok = false;
			break;
		}
		if (fwrite(buf, 1, (size_t) r, f) != (size_t) r) {
			ok = false;
			break;
		}
		got += (uint64_t) r;
		pthread_mutex_lock(&u->lock);
		u->st.received = got;
		pthread_mutex_unlock(&u->lock);
	}
	ok = fclose(f) == 0 && ok && got == length;
	if (ok) {
		remove(path);
		ok = rename(part, path) == 0;
	} else {
		remove(part);
	}
	pthread_mutex_lock(&u->lock);
	u->st.active = false;
	if (ok) {
		u->st.done++;
		if (u->st.nRecent == LINGO_UPLOAD_RECENT) {
			memmove(u->st.recent, u->st.recent + 1, sizeof(u->st.recent[0]) * (LINGO_UPLOAD_RECENT - 1));
			u->st.nRecent--;
		}
		snprintf(u->st.recent[u->st.nRecent++], sizeof(u->st.recent[0]), "%s/%s", sys, name);
		u->st.error[0] = 0;
	} else {
		snprintf(u->st.error, sizeof(u->st.error), "%s did not arrive in one piece", name);
	}
	pthread_mutex_unlock(&u->lock);
	respond(fd, ok ? 200 : 400, "text/plain", ok ? "ok" : "upload interrupted");
}

static void handle(struct LingoUpload* u, int fd) {
	char req[8192];
	size_t have = 0;
	char* end = NULL;
	while (have < sizeof(req) - 1) {
		ssize_t r = recv(fd, req + have, sizeof(req) - 1 - have, 0);
		if (r <= 0) {
			return;
		}
		have += (size_t) r;
		req[have] = 0;
		if ((end = strstr(req, "\r\n\r\n"))) {
			break;
		}
	}
	if (!end) {
		respond(fd, 400, "text/plain", "bad request");
		return;
	}
	*end = 0;
	char* body = end + 4;
	size_t bodyHave = have - (size_t) (body - req);
	char method[8] = "", target[1024] = "";
	if (sscanf(req, "%7s %1023s", method, target) != 2) {
		respond(fd, 400, "text/plain", "bad request");
		return;
	}
	uint64_t length = 0;
	for (char* line = strstr(req, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
		if (!strncasecmp(line + 2, "Content-Length:", 15)) {
			length = strtoull(line + 17, NULL, 10);
		}
	}
	char* query = strchr(target, '?');
	if (query) {
		*query++ = 0;
	} else {
		query = "";
	}
	char token[40] = "";
	queryParam(query, "k", token, sizeof(token));
	pthread_mutex_lock(&u->lock);
	u->st.clients++;
	pthread_mutex_unlock(&u->lock);
	if (strcmp(token, u->token)) {
		respond(fd, 403, "text/html; charset=utf-8",
		        "<!doctype html><meta charset=utf-8><body style='background:#1a2e25;color:#eef1e6;font:18px system-ui;"
		        "padding:24px'>Scan the QR code on the Vita again: this link has expired.");
		return;
	}
	if (!strcmp(method, "GET") && (!strcmp(target, "/") || !strcmp(target, "/index.html"))) {
		servePage(u, fd);
	} else if ((!strcmp(method, "PUT") || !strcmp(method, "POST")) && !strcmp(target, "/upload")) {
		receive(u, fd, query, length, body, bodyHave);
	} else {
		respond(fd, 400, "text/plain", "not found");
	}
}

static void* serverThread(void* arg) {
	struct LingoUpload* u = arg;
	while (u->running) {
		struct sockaddr_in addr;
		socklen_t len = sizeof(addr);
		int c = accept(u->fd, (struct sockaddr*) &addr, &len);
		if (c < 0) {
			continue; /* timeout: check running again */
		}
		struct timeval tv = { 20, 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		handle(u, c);
		/* let the reply drain to the phone before the socket goes away */
#ifdef SHUT_WR
		shutdown(c, SHUT_WR);
#endif
		char drain[256];
		struct timeval quick = { 1, 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &quick, sizeof(quick));
		while (recv(c, drain, sizeof(drain), 0) > 0) {
		}
		close(c);
	}
	return NULL;
}

struct LingoUpload* lingoUploadStart(int port, const char* root, const char* token,
                                     const struct LingoUploadSystem* systems, int nSystems) {
	struct LingoUpload* u = calloc(1, sizeof(*u));
	if (!u) {
		return NULL;
	}
	snprintf(u->root, sizeof(u->root), "%s", root);
	snprintf(u->token, sizeof(u->token), "%s", token);
	u->systems = systems;
	u->nSystems = nSystems;
	pthread_mutex_init(&u->lock, NULL);
	u->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (u->fd < 0) {
		free(u);
		return NULL;
	}
	int one = 1;
	setsockopt(u->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t) port);
	if (bind(u->fd, (struct sockaddr*) &addr, sizeof(addr)) < 0 || listen(u->fd, 4) < 0) {
		close(u->fd);
		free(u);
		return NULL;
	}
	socklen_t al = sizeof(addr);
	getsockname(u->fd, (struct sockaddr*) &addr, &al);
	u->port = ntohs(addr.sin_port);
	struct timeval tv = { 0, 250000 };
	setsockopt(u->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	u->running = 1;
	if (pthread_create(&u->thread, NULL, serverThread, u) != 0) {
		close(u->fd);
		free(u);
		return NULL;
	}
	return u;
}

int lingoUploadPort(const struct LingoUpload* u) {
	return u ? u->port : 0;
}

void lingoUploadGetStatus(struct LingoUpload* u, struct LingoUploadStatus* out) {
	pthread_mutex_lock(&u->lock);
	*out = u->st;
	pthread_mutex_unlock(&u->lock);
}

void lingoUploadStop(struct LingoUpload* u) {
	if (!u) {
		return;
	}
	u->running = 0;
	shutdown(u->fd, SHUT_RDWR);
	close(u->fd);
	pthread_join(u->thread, NULL);
	pthread_mutex_destroy(&u->lock);
	free(u);
}
