#pragma once
#include <stdint.h>
enum { SCE_CTRL_SELECT = 1, SCE_CTRL_START = 8, SCE_CTRL_UP = 0x10, SCE_CTRL_RIGHT = 0x20, SCE_CTRL_DOWN = 0x40, SCE_CTRL_LEFT = 0x80,
       SCE_CTRL_TRIANGLE = 0x1000, SCE_CTRL_CIRCLE = 0x2000, SCE_CTRL_CROSS = 0x4000, SCE_CTRL_SQUARE = 0x8000, SCE_CTRL_L1 = 0x400, SCE_CTRL_R1 = 0x800 };
typedef struct SceCtrlData { uint64_t timeStamp; unsigned int buttons; unsigned char lx, ly, rx, ry; } SceCtrlData;
int sceCtrlPeekBufferPositive(int port, SceCtrlData* pad, int count);
