// SPDX-License-Identifier: GPL-2.0-or-later
// sotc-android: force-included into the GPU library build. The vendored code times itself with
// x86 builtins; on ARM64 they map to the generic timer (CNTVCT_EL0, the same clock FEX gives the
// game's rdtsc and the runtime's sceKernelReadTsc) and to YIELD.
#pragma once
#if defined(__aarch64__)
#include <stdint.h>
#ifdef __cplusplus
#include <arm_neon.h>
#endif
static inline unsigned long long sotc_cntvct(void) {
    unsigned long long v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#define __builtin_ia32_rdtsc() sotc_cntvct()
#define __rdtsc() sotc_cntvct()
#define __builtin_ia32_pause() __asm__ volatile("yield")
#define _mm_pause() __asm__ volatile("yield")
#endif
