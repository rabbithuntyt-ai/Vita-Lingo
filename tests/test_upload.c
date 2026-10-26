/* Host test for the upload server: real HTTP over localhost. */
#include "../src/psp2-lingo-upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures;
#define CHECK(c, ...) do { if (!(c)) { ++failures; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char** argv) {
	const char* root = argc > 1 ? argv[1] : "upload-test";
	char cmd[1024], name[64];
	CHECK(lingoUploadSafeName("../../etc/passwd", name, sizeof name) && !strcmp(name, "passwd"), "path stripped: %s", name);
	CHECK(lingoUploadSafeName("ポケモン エメラルド.gba", name, sizeof name) && !strcmp(name, "ポケモン エメラルド.gba"), "japanese kept: %s", name);
	CHECK(!lingoUploadSafeName("..", name, sizeof name), "dots rejected");
	char dec[64]; lingoUrlDecode("a%20b%E3%81%82+c", dec, sizeof dec);
	CHECK(!strcmp(dec, "a bあ c"), "url decode: %s", dec);
	static const struct LingoUploadSystem sys[] = { { "gba", "Game Boy Advance", ".gba", NULL }, { "ps1", "PlayStation", ".cue.bin", NULL } };
	struct LingoUpload* u = lingoUploadStart(0, root, "tok123", sys, 2);
	CHECK(u != NULL, "server did not start");
	if (!u) return 1;
	int port = lingoUploadPort(u);
	printf("listening on %d\n", port);
	/* the page, with and without the token */
	snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o %s/page.html -w '%%{http_code}' 'http://127.0.0.1:%d/?k=tok123'", root, port);
	FILE* p = popen(cmd, "r"); char code[8] = ""; fgets(code, sizeof code, p); pclose(p);
	CHECK(!strcmp(code, "200"), "page status %s", code);
	snprintf(cmd, sizeof cmd, "grep -c \"SYS=\\[\\['gba'\" %s/page.html", root);
	p = popen(cmd, "r"); fgets(code, sizeof code, p); pclose(p);
	CHECK(atoi(code) == 1, "page lacks the console list");
	snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o /dev/null -w '%%{http_code}' 'http://127.0.0.1:%d/?k=wrong'", port);
	p = popen(cmd, "r"); fgets(code, sizeof code, p); pclose(p);
	CHECK(!strcmp(code, "403"), "bad token status %s", code);
	/* a 3 MB file with a Japanese name */
	snprintf(cmd, sizeof cmd, "head -c 3000000 /dev/urandom > %s/src.bin", root); system(cmd);
	snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o /dev/null -w '%%{http_code}' -T %s/src.bin "
		"'http://127.0.0.1:%d/upload?k=tok123&sys=gba&name=%%E3%%83%%9D%%E3%%82%%B1%%E3%%83%%A2%%E3%%83%%B3.gba'", root, port);
	p = popen(cmd, "r"); fgets(code, sizeof code, p); pclose(p);
	CHECK(!strcmp(code, "200"), "upload status %s", code);
	snprintf(cmd, sizeof cmd, "cmp -s %s/src.bin '%s/gba/ポケモン.gba' && echo same", root, root);
	p = popen(cmd, "r"); char same[8] = ""; fgets(same, sizeof same, p); pclose(p);
	CHECK(!strncmp(same, "same", 4), "uploaded file differs or missing");
	/* path tricks and unknown consoles are refused */
	snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o /dev/null -w '%%{http_code}' -T %s/src.bin 'http://127.0.0.1:%d/upload?k=tok123&sys=xyz&name=a.gba'", root, port);
	p = popen(cmd, "r"); fgets(code, sizeof code, p); pclose(p);
	CHECK(!strcmp(code, "400"), "unknown console accepted (%s)", code);
	snprintf(cmd, sizeof cmd, "curl -s --noproxy '*' -o /dev/null -w '%%{http_code}' -T %s/src.bin 'http://127.0.0.1:%d/upload?k=tok123&sys=gba&name=..%%2F..%%2Fevil.gba'", root, port);
	p = popen(cmd, "r"); fgets(code, sizeof code, p); pclose(p);
	snprintf(cmd, sizeof cmd, "test -f %s/gba/evil.gba && test ! -f %s/../evil.gba && echo safe", root, root);
	p = popen(cmd, "r"); char safe[8] = ""; fgets(safe, sizeof safe, p); pclose(p);
	CHECK(!strcmp(code, "200") && !strncmp(safe, "safe", 4), "path traversal not contained (%s)", code);
	struct LingoUploadStatus st;
	lingoUploadGetStatus(u, &st);
	printf("done %d, recent: %s | %s\n", st.done, st.nRecent ? st.recent[0] : "", st.nRecent > 1 ? st.recent[1] : "");
	CHECK(st.done == 2 && !st.active, "status done=%d", st.done);
	lingoUploadStop(u);
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
