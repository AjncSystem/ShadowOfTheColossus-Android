/* SPDX-License-Identifier: GPL-2.0-or-later
 * C interface between the PS4 runtime (runtime_*.c, from SotC-PC) and the guest CPU (FEXCore).
 * On SotC-PC the runtime calls guest function pointers directly because the game's code runs
 * natively; here those calls go through FEX. */
#ifndef SOTC_GUEST_H
#define SOTC_GUEST_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gives the calling host thread a guest CPU context (FEX thread) whose TLS base (fs and gs) is
 * `tcb` and whose own guest stack has `stack_size` bytes. Idempotent: a second call only moves
 * the TLS base. Returns 0 on success. */
int sotc_guest_attach(uint64_t tcb, uint64_t stack_size);
/* Releases the calling thread's guest context (thread exit). */
void sotc_guest_detach(void);
int sotc_guest_attached(void);

/* Calls guest function `fn` with up to 12 integer arguments on the calling thread's guest
 * context: nested when called from inside an HLE function, top level otherwise. Returns RAX. */
uint64_t sotc_guest_call(const void *fn, unsigned count, const uint64_t *args);

static inline uint64_t sotc_guest_call0(const void *fn) { return sotc_guest_call(fn, 0, 0); }
static inline uint64_t sotc_guest_call1(const void *fn, uint64_t a) { return sotc_guest_call(fn, 1, &a); }
static inline uint64_t sotc_guest_call2(const void *fn, uint64_t a, uint64_t b) {
    const uint64_t v[2] = {a, b};
    return sotc_guest_call(fn, 2, v);
}
static inline uint64_t sotc_guest_call3(const void *fn, uint64_t a, uint64_t b, uint64_t c) {
    const uint64_t v[3] = {a, b, c};
    return sotc_guest_call(fn, 3, v);
}

/* Host side of the generic guest -> native bridge (host_bridge.S). */
uint64_t sotc_call_host(const void *fn, const uint64_t ints[8], const uint64_t fps[8], uint64_t out[3]);

#ifdef __cplusplus
}
#endif
#endif
