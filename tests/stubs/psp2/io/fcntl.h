#pragma once
#include <stddef.h>
typedef int SceUID; typedef long long SceOff;
enum { SCE_O_RDONLY = 1, SCE_O_WRONLY = 2, SCE_O_CREAT = 0x200, SCE_SEEK_SET = 0, SCE_SEEK_END = 2 };
SceUID sceIoOpen(const char* f, int flags, int mode); int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void* buf, size_t n); SceOff sceIoLseek(SceUID fd, SceOff off, int whence);
