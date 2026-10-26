#pragma once
#include <stdint.h>
enum { SCE_TOUCH_PORT_FRONT = 0, SCE_TOUCH_PORT_BACK = 1, SCE_TOUCH_SAMPLING_STATE_START = 1 };
typedef struct SceTouchReport { uint8_t id, force; int16_t x, y; } SceTouchReport;
typedef struct SceTouchData { uint64_t timeStamp; uint32_t status, reportNum; SceTouchReport report[8]; } SceTouchData;
int sceTouchPeek(int port, SceTouchData* data, int n); int sceTouchSetSamplingState(int port, int state);
