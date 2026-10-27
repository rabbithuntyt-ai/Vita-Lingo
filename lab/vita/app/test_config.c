/* Host test for Vita Lab's taiHEN config editing. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void check(const char* name, const char* in, bool add, const char* want) {
	int n;
	char* out = labRewriteConfig(in, add, &n);
	if (strcmp(out, want) || (int) strlen(out) != n) {
		printf("FAIL %s\n--- got ---\n%s--- want ---\n%s", name, out, want);
		++fails;
	}
	free(out);
}

int main(void) {
	const char* stock = "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"
	                    "*KERNEL\nur0:tai/nonpdrm.skprx\n# hello\n*main\n*NPXS10015\nur0:tai/henkaku.suprx\n";
	check("add to stock", stock, true,
	      "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"
	      "*KERNEL\nur0:tai/vitacompanion_kernel.skprx\nur0:tai/nonpdrm.skprx\n# hello\n*main\nur0:tai/vitacompanion.suprx\n"
	      "*NPXS10015\nur0:tai/henkaku.suprx\n");
	char* added;
	int n;
	added = labRewriteConfig(stock, true, &n);
	check("add twice is the same", added, true,
	      "# This file is used as an alternative if ux0:tai/config.txt is not found.\n"
	      "*KERNEL\nur0:tai/vitacompanion_kernel.skprx\nur0:tai/nonpdrm.skprx\n# hello\n*main\nur0:tai/vitacompanion.suprx\n"
	      "*NPXS10015\nur0:tai/henkaku.suprx\n");
	check("remove gives the original back", added, false, stock);
	free(added);
	check("no sections", "*NPXS10015\nur0:tai/henkaku.suprx", true,
	      "*KERNEL\nur0:tai/vitacompanion_kernel.skprx\n*NPXS10015\nur0:tai/henkaku.suprx\n*main\nur0:tai/vitacompanion.suprx\n");
	check("CRLF and an old vitacompanion", "*KERNEL\r\nux0:tai/vitacompanion_kernel.skprx\r\n*main\r\nux0:tai/vitacompanion.suprx\r\n", true,
	      "*KERNEL\r\nur0:tai/vitacompanion_kernel.skprx\n*main\r\nur0:tai/vitacompanion.suprx\n");
	check("empty file", "", true, "*KERNEL\nur0:tai/vitacompanion_kernel.skprx\n*main\nur0:tai/vitacompanion.suprx\n");
	if (fails) {
		printf("%d FAILED\n", fails);
		return 1;
	}
	printf("config tests passed\n");
	return 0;
}
