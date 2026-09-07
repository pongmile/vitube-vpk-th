/* Host stub: the clock src/media/hls.c ages its playlist against. The test
 * drives it by hand (tests/test_hls.c) so a reload can be made due without
 * the test having to sit through the real interval. */
#ifndef VITUBE_TEST_STUB_PSP2_PROCESSMGR_H
#define VITUBE_TEST_STUB_PSP2_PROCESSMGR_H

#include <stdint.h>

uint64_t sceKernelGetProcessTimeWide(void);

#endif
