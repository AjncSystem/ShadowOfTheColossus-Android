// SPDX-License-Identifier: GPL-2.0-or-later
// uint64_t sotc_call_host(void *fn, const uint64_t ints[8], const uint64_t fps[8], uint64_t out[3])
//
// Calls a native (AAPCS64) function with the guest's System V arguments already sorted into
// integer and floating-point classes: ints[] go to x0-x7, the low 64 bits of XMM0-7 go to d0-d7.
// AAPCS64 assigns the two classes independently, like System V does, so any non-variadic
// function with at most 8 integer and 8 FP arguments receives exactly what the guest passed.
// Returns x0; out[0] = x1, out[1] = d0, out[2] = d1 (for RDX and XMM0/XMM1 of 16-byte returns).
    .text
    .p2align 4
    .globl sotc_call_host
    .type sotc_call_host, %function
sotc_call_host:
    hint #34                    // bti c
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x3                 // out
    mov x16, x0                 // fn
    mov x17, x1                 // ints
    ldp d0, d1, [x2, #0]
    ldp d2, d3, [x2, #16]
    ldp d4, d5, [x2, #32]
    ldp d6, d7, [x2, #48]
    ldp x0, x1, [x17, #0]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    blr x16
    str x1, [x19, #0]
    str d0, [x19, #8]
    str d1, [x19, #16]
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
    .size sotc_call_host, . - sotc_call_host

    .section .note.GNU-stack, "", %progbits
