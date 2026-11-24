// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif
#include <cstring>

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(ARCH_ARM64) && defined(__linux__)
    // sotc-android: on FEX the faulting code is the JIT's ARM64 code.
    return (void*)((ucontext_t*)ctx)->uc_mcontext.pc;
#else
#error "Unsupported architecture"
#endif
}

#if defined(ARCH_ARM64) && defined(__linux__) && !defined(__APPLE__)
// sotc-android: the ESR record of the Linux arm64 signal frame (asm/sigcontext.h esr_context).
static u64 Arm64Esr(void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    const u8* p = reinterpret_cast<const u8*>(uc->uc_mcontext.__reserved);
    const u8* end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + 8 <= end) {
        u32 magic, size;
        std::memcpy(&magic, p, 4);
        std::memcpy(&size, p + 4, 4);
        if (magic == 0 || size < 8) break;
        if (magic == 0x45535201 && size >= 16) {  // ESR_MAGIC
            u64 esr;
            std::memcpy(&esr, p + 8, 8);
            return esr;
        }
        p += size;
    }
    return 0;
}
#endif

void SkipLoadAsZero(void* ctx, u8 gpr_index, u64 length) {
#if defined(_WIN32)
    // CONTEXT keeps Rax..R15 in x86 encoding order.
    auto* context = ((EXCEPTION_POINTERS*)ctx)->ContextRecord;
    (&context->Rax)[gpr_index] = 0;
    context->Rip += length;
#elif defined(ARCH_X86_64) && !defined(__APPLE__) && !defined(__FreeBSD__)
    static constexpr int regs[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                     REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                     REG_R12, REG_R13, REG_R14, REG_R15};
    auto& gregs = ((ucontext_t*)ctx)->uc_mcontext.gregs;
    gregs[regs[gpr_index & 15]] = 0;
    gregs[REG_RIP] += length;
#elif defined(ARCH_ARM64) && defined(__linux__)
    // sotc-android: the faulting instruction is an ARM64 load from the FEX JIT: zero its
    // destination register (Rt, bits 0-4) and step over it. gpr_index/length describe x86.
    (void)gpr_index;
    (void)length;
    auto& mc = ((ucontext_t*)ctx)->uc_mcontext;
    u32 insn;
    std::memcpy(&insn, reinterpret_cast<const void*>(mc.pc), 4);
    const u32 rt = insn & 31;
    if (rt != 31) mc.regs[rt] = 0;
    mc.pc += 4;
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#elif defined(ARCH_ARM64) && defined(__linux__)
    return Arm64Esr(ctx) & 0x40;  // sotc-android: ESR_ELx.WnR (data abort written by a store)
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
