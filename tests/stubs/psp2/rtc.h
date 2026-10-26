#pragma once
typedef struct SceDateTime { unsigned short year, month, day, hour, minute, second; unsigned int microsecond; } SceDateTime;
int sceRtcGetCurrentClockLocalTime(SceDateTime* t);
