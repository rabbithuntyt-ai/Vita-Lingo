#pragma once

#include <stdbool.h>

#define USER_PLUGIN "ur0:tai/vitacompanion.suprx"
#define KERNEL_PLUGIN "ur0:tai/vitacompanion_kernel.skprx"

/* The config text without any vitacompanion lines; with add, our two plugins
 * go back in under *KERNEL and *main (sections added if missing). malloc'd. */
char* labRewriteConfig(const char* in, bool add, int* outLen);
