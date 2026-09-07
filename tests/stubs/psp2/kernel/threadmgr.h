/* Host stub: the two thread calls src/media/hls.c makes. Bodies live in
 * tests/test_hls.c — the priority change is a no-op there and the delay is a
 * real sleep, which is all the polling loops need. */
#ifndef VITUBE_TEST_STUB_PSP2_THREADMGR_H
#define VITUBE_TEST_STUB_PSP2_THREADMGR_H

#include <stdint.h>

typedef int SceUID;

SceUID sceKernelGetThreadId(void);
int sceKernelChangeThreadPriority(SceUID thid, int priority);
int sceKernelDelayThread(uint32_t usec);

#endif
