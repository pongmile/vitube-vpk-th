#pragma once
#include <stdint.h>
typedef uint64_t SceUInt64;
typedef struct SceRtcTick { SceUInt64 tick; } SceRtcTick;
int sceRtcGetCurrentTick(SceRtcTick *tick);
typedef struct SceDateTime { unsigned short year, month, day, hour, minute, second; unsigned int microsecond; } SceDateTime;
int sceRtcGetCurrentClockLocalTime(SceDateTime *time);
