/* SPDX-License-Identifier: GPL-2.0-or-later
 * sotc-android: what the runtime expects from parts that are not ported yet.
 *  - Fibers (runtime_fiber.c switches x86 stacks with host assembly; on FEX the switch has to
 *    happen in guest code): not provided yet, so their imports stop the guest when called.
 *  - probe.c's process hooks. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

void runtime_restart(void) {
    fputs("STOP: the game asked for a restart (not supported on Android yet)\n", stderr);
    exit(24);
}

/* ---- fibers ---- */
uintptr_t runtime_fiber_resolve(const char *name) { (void)name; return 0; }
void runtime_fiber_thread_exit(void) {}
void runtime_fiber_report(void) { puts("Runtime: fibers not available on Android yet"); }
