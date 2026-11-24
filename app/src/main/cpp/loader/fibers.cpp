// SPDX-License-Identifier: GPL-2.0-or-later
// libSceFiber for Shadow of the Colossus on FEX (the game's loader and job system run on fibers).
// Same contract and checks as SotC-PC's runtime_fiber.c (a port of shadPS4's HLE), but the switch
// happens inside the HLE handler instead of host assembly: every fiber call reaches us through a
// `syscall; ret` stub, FEX reloads all guest registers from the thread state after the handler,
// and the stub's `ret` then pops a return address from whatever RSP we left. Suspending a fiber
// = saving its RSP (pointing at its return address) and callee-saved registers; resuming =
// loading them back. A new fiber gets a stack whose top holds [entry, exit stub], so the `ret`
// enters `entry` with the System V arguments already in RDI/RSI.
// Fibers can be resumed on another host thread (job workers): the saved state is only guest
// registers, and the run point (sceFiberRun) is per host thread, as on the PS4.
#include "fibers.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../gpu/bbgpu.h"
#include "../guest/guest_cpu.h"
#include "../log.h"
#include "game_process.h"

namespace sotc::loader {

namespace {

using namespace FEXCore::X86State;
using guest::HleCall;

constexpr int32_t kErrNull = static_cast<int32_t>(0x80590001u);
constexpr int32_t kErrAlignment = static_cast<int32_t>(0x80590002u);
constexpr int32_t kErrRange = static_cast<int32_t>(0x80590003u);
constexpr int32_t kErrInvalid = static_cast<int32_t>(0x80590004u);
constexpr int32_t kErrPermission = static_cast<int32_t>(0x80590005u);
constexpr int32_t kErrState = static_cast<int32_t>(0x80590006u);
constexpr uint64_t kContextMinimumSize = 512;
constexpr int kMaxName = 31;

enum : uint32_t { StateRun = 1, StateIdle = 2, StateTerminated = 3 };
enum : uint32_t { FlagContextSizeCheck = 0x10, FlagSetFpuRegs = 0x100 };
constexpr uint32_t kSignature0 = 0xdef1649c, kSignature1 = 0xb37592a0, kOptSignature = 0xbb40e64d;
constexpr uint64_t kStackSignature = 0x7149f2ca7149f2caull, kStackSizeCheck = 0xdeadbeefdeadbeefull;

// Suspended guest state of a fiber (host memory, owned through Fiber::context).
struct Saved {
    uint64_t rsp, rbx, rbp, r12, r13, r14, r15;
    uint32_t mxcsr;
    uint16_t fcw;
    uint64_t* out_arg;  // where the fiber wants the argument it is resumed with
};

// Guest-allocated SceFiber (256 bytes); same layout as SotC-PC / shadPS4.
struct Fiber {
    uint32_t magic_start;
    std::atomic<uint32_t> state;
    uint64_t entry;
    uint64_t arg_on_initialize;
    uint8_t* addr_context;
    uint64_t size_context;
    char name[kMaxName + 1];
    Saved* context;
    uint32_t flags;
    void *context_start, *context_end;
    uint32_t magic_end;
};
static_assert(sizeof(Fiber) <= 256, "SceFiber is 256 bytes");

struct FiberInfo {
    uint64_t size;
    uint64_t entry;
    uint64_t arg_on_initialize;
    void* addr_context;
    uint64_t size_context;
    char name[kMaxName + 1];
    uint64_t size_context_margin;
    uint8_t pad[48];
};
static_assert(sizeof(FiberInfo) == 128, "SceFiberInfo is 128 bytes");

// The sceFiberRun point of this host thread.
struct RunPoint {
    bool active = false;
    Saved thread{};            // the thread's own state at sceFiberRun
    Fiber* current = nullptr;
    uint64_t* arg_on_return = nullptr;
};
thread_local RunPoint tls_run;

std::atomic<unsigned> g_created{0}, g_runs{0}, g_switches{0}, g_refused{0};
uint64_t g_exit_stub = 0;  // return address of a fresh fiber's entry: "entry returned"

FEXCore::Core::CPUState& State(HleCall& c) {
    return c.Frame()->State;
}

void Save(HleCall& c, Saved& s, uint64_t* out_arg) {
    const auto& st = State(c);
    s.rsp = st.gregs[REG_RSP];
    s.rbx = st.gregs[REG_RBX];
    s.rbp = st.gregs[REG_RBP];
    s.r12 = st.gregs[REG_R12];
    s.r13 = st.gregs[REG_R13];
    s.r14 = st.gregs[REG_R14];
    s.r15 = st.gregs[REG_R15];
    s.mxcsr = st.mxcsr;
    s.fcw = st.FCW;
    s.out_arg = out_arg;
}

void Load(HleCall& c, const Saved& s) {
    auto& st = State(c);
    st.gregs[REG_RSP] = s.rsp;
    st.gregs[REG_RBX] = s.rbx;
    st.gregs[REG_RBP] = s.rbp;
    st.gregs[REG_R12] = s.r12;
    st.gregs[REG_R13] = s.r13;
    st.gregs[REG_R14] = s.r14;
    st.gregs[REG_R15] = s.r15;
    st.mxcsr = (st.mxcsr & 0x3f) | (s.mxcsr & ~0x3fu);  // keep the status flags, as SotC-PC
    st.FCW = s.fcw;
}

bool Valid(const Fiber* f) {
    return f->magic_start == kSignature0 && f->magic_end == kSignature1;
}

int32_t Refused(const char* call, const Fiber* f, int32_t error) {
    if (g_refused.fetch_add(1) < 32)
        fprintf(stderr, "Runtime: %s refused fiber '%s' (state %u): 0x%08x\n", call, f ? f->name : "?",
                f ? f->state.load() : 0, static_cast<unsigned>(error));
    return error;
}

void StackPrepare(Fiber* f) {
    bbgpu_register_guest_stack(reinterpret_cast<uintptr_t>(f->addr_context), f->size_context);
    f->context_start = f->addr_context;
    f->context_end = f->addr_context + f->size_context;
    std::memcpy(f->addr_context, &kStackSignature, 8);
    if (f->flags & FlagContextSizeCheck)
        for (auto* p = reinterpret_cast<uint64_t*>(f->context_start) + 1; p < reinterpret_cast<uint64_t*>(f->context_end); ++p)
            *p = kStackSizeCheck;
}

void CheckOverflow(const Fiber* f) {
    uint64_t sig;
    if (f && f->addr_context && (std::memcpy(&sig, f->addr_context, 8), sig != kStackSignature)) {
        fprintf(stderr, "STOP: stack overflow in fiber '%s' (context 0x%llx bytes)\n", f->name,
                static_cast<unsigned long long>(f->size_context));
        abort();
    }
}

int32_t AttachContext(Fiber* f, uint64_t addr, uint64_t size) {
    if (size && size < kContextMinimumSize) return kErrRange;
    if ((size & 15) || !addr || !size || f->addr_context) return kErrInvalid;
    f->addr_context = reinterpret_cast<uint8_t*>(addr);
    f->size_context = size;
    StackPrepare(f);
    return 0;
}

// Makes the guest continue in fiber f (state Run), passing `arg` as its arg_on_run.
// `fallback_rsp`: where a fiber without its own context starts (below the run point).
void Enter(HleCall& c, Fiber* f, uint64_t arg, uint64_t fallback_rsp) {
    if (Saved* s = f->context) {
        Load(c, *s);
        if (s->out_arg) *s->out_arg = arg;
        f->context = nullptr;
        delete s;
        return;
    }
    const uint64_t top = f->addr_context ? reinterpret_cast<uint64_t>(f->addr_context + f->size_context) & ~uint64_t{15}
                                         : fallback_rsp & ~uint64_t{15};
    // [top-8] = the entry's return address, [top-16] = entry: the stub's ret jumps into entry
    // with RSP = top-8 (16n+8, as after a call).
    uint64_t* stack = reinterpret_cast<uint64_t*>(top);
    stack[-1] = g_exit_stub;
    stack[-2] = f->entry;
    auto& st = State(c);
    for (int r : {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RBP, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15})
        st.gregs[r] = 0;
    st.gregs[REG_RSP] = top - 16;
    st.gregs[REG_RDI] = f->arg_on_initialize;
    st.gregs[REG_RSI] = arg;
    if (f->flags & FlagSetFpuRegs) {
        st.mxcsr = 0x9fc0;
        st.FCW = 0x037f;
    }
}

// ---- functions ---------------------------------------------------------------------------------

int32_t Initialize(Fiber* f, const char* name, uint64_t entry, uint64_t arg, uint64_t addr, uint64_t size,
                   const uint32_t* opt, uint32_t flags, uint32_t build_version) {
    if (!f || !name || !entry) return kErrNull;
    if ((reinterpret_cast<uintptr_t>(f) & 7) || (addr & 15) || (opt && (reinterpret_cast<uintptr_t>(opt) & 7))) return kErrAlignment;
    if (size && size < kContextMinimumSize) return kErrRange;
    if ((size & 15) || (!addr) != (!size) || (opt && *opt != kOptSignature)) return kErrInvalid;
    if (build_version >= 0x03500000) flags |= FlagSetFpuRegs;
    std::memset(f->name, 0, sizeof(f->name));
    std::strncpy(f->name, name, kMaxName);
    f->entry = entry;
    f->arg_on_initialize = arg;
    f->addr_context = reinterpret_cast<uint8_t*>(addr);
    f->size_context = size;
    f->context = nullptr;
    f->flags = flags;
    f->magic_start = kSignature0;
    f->magic_end = kSignature1;
    if (addr) StackPrepare(f);
    f->state.store(StateIdle);
    g_created.fetch_add(1);
    return 0;
}

uint64_t HleInitialize(HleCall& c) {  // (f, name, entry, arg, addr, size, opt, build_version)
    return static_cast<uint32_t>(Initialize(reinterpret_cast<Fiber*>(c.Arg(0)), reinterpret_cast<const char*>(c.Arg(1)),
                                            c.Arg(2), c.Arg(3), c.Arg(4), c.Arg(5),
                                            reinterpret_cast<const uint32_t*>(c.Arg(6)), 0, static_cast<uint32_t>(c.Arg(7))));
}

uint64_t HleInitializeInternal(HleCall& c) {  // (f, name, entry, arg, addr, size, opt, flags, build_version)
    return static_cast<uint32_t>(Initialize(reinterpret_cast<Fiber*>(c.Arg(0)), reinterpret_cast<const char*>(c.Arg(1)),
                                            c.Arg(2), c.Arg(3), c.Arg(4), c.Arg(5),
                                            reinterpret_cast<const uint32_t*>(c.Arg(6)), static_cast<uint32_t>(c.Arg(7)),
                                            static_cast<uint32_t>(c.Arg(8))));
}

uint64_t HleOptParamInitialize(HleCall& c) {
    auto* opt = reinterpret_cast<uint32_t*>(c.Arg(0));
    if (!opt) return static_cast<uint32_t>(kErrNull);
    if (reinterpret_cast<uintptr_t>(opt) & 7) return static_cast<uint32_t>(kErrAlignment);
    *opt = kOptSignature;
    return 0;
}

uint64_t HleFinalize(HleCall& c) {
    auto* f = reinterpret_cast<Fiber*>(c.Arg(0));
    if (!f) return static_cast<uint32_t>(kErrNull);
    if (reinterpret_cast<uintptr_t>(f) & 7) return static_cast<uint32_t>(kErrAlignment);
    if (!Valid(f)) return static_cast<uint32_t>(kErrInvalid);
    uint32_t expected = StateIdle;
    if (!f->state.compare_exchange_strong(expected, StateTerminated))
        return static_cast<uint32_t>(Refused("sceFiberFinalize", f, kErrState));
    if (f->addr_context) bbgpu_register_guest_stack_end(reinterpret_cast<uintptr_t>(f->addr_context), f->size_context);
    return 0;
}

int32_t Run(HleCall& c, Fiber* f, uint64_t addr, uint64_t size, uint64_t arg, uint64_t* arg_on_return) {
    if (!f) return kErrNull;
    if ((reinterpret_cast<uintptr_t>(f) & 7) || (addr & 15)) return kErrAlignment;
    if (!Valid(f)) return kErrInvalid;
    RunPoint& run = tls_run;
    if (run.active) return Refused("sceFiberRun", f, kErrPermission);
    if (addr || size) {
        if (int32_t r = AttachContext(f, addr, size); r < 0) return r;
    }
    uint32_t expected = StateIdle;
    if (!f->state.compare_exchange_strong(expected, StateRun)) return Refused("sceFiberRun", f, kErrState);
    g_runs.fetch_add(1);
    run.active = true;
    run.current = f;
    run.arg_on_return = arg_on_return;
    Save(c, run.thread, nullptr);
    // A fiber without its own context runs on the thread's stack, below the run point.
    Enter(c, f, arg, run.thread.rsp - 256);
    return 0;
}

uint64_t HleRun(HleCall& c) {  // (f, arg, *arg_on_return)
    return static_cast<uint32_t>(Run(c, reinterpret_cast<Fiber*>(c.Arg(0)), 0, 0, c.Arg(1), reinterpret_cast<uint64_t*>(c.Arg(2))));
}
uint64_t HleAttachAndRun(HleCall& c) {  // (f, addr, size, arg, *arg_on_return)
    return static_cast<uint32_t>(Run(c, reinterpret_cast<Fiber*>(c.Arg(0)), c.Arg(1), c.Arg(2), c.Arg(3),
                                     reinterpret_cast<uint64_t*>(c.Arg(4))));
}

int32_t Switch(HleCall& c, Fiber* f, uint64_t addr, uint64_t size, uint64_t arg, uint64_t* arg_on_run) {
    if (!f) return kErrNull;
    if ((reinterpret_cast<uintptr_t>(f) & 7) || (addr & 15)) return kErrAlignment;
    if (!Valid(f)) return kErrInvalid;
    RunPoint& run = tls_run;
    if (!run.active) return kErrPermission;
    if (addr || size) {
        if (int32_t r = AttachContext(f, addr, size); r < 0) return r;
    }
    uint32_t expected = StateIdle;
    if (!f->state.compare_exchange_strong(expected, StateRun)) return Refused("sceFiberSwitch", f, kErrState);
    g_switches.fetch_add(1);
    Fiber* current = run.current;
    uint64_t fallback = run.thread.rsp - 256;
    if (current->addr_context) {
        // Suspend the current fiber; it continues after this call when switched back to.
        auto* s = new Saved;
        Save(c, *s, arg_on_run);
        current->context = s;
        CheckOverflow(current);
    }
    // (a fiber without its own context lives below the run point and is abandoned, as on SotC-PC)
    current->state.store(StateIdle);
    run.current = f;
    Enter(c, f, arg, fallback);
    return 0;
}

uint64_t HleSwitch(HleCall& c) {  // (f, arg, *arg_on_run)
    return static_cast<uint32_t>(Switch(c, reinterpret_cast<Fiber*>(c.Arg(0)), 0, 0, c.Arg(1), reinterpret_cast<uint64_t*>(c.Arg(2))));
}
uint64_t HleAttachAndSwitch(HleCall& c) {  // (f, addr, size, arg, *arg_on_run)
    return static_cast<uint32_t>(Switch(c, reinterpret_cast<Fiber*>(c.Arg(0)), c.Arg(1), c.Arg(2), c.Arg(3),
                                        reinterpret_cast<uint64_t*>(c.Arg(4))));
}

uint64_t HleReturnToThread(HleCall& c) {  // (arg_on_return, *arg_on_run)
    RunPoint& run = tls_run;
    if (!run.active) return static_cast<uint32_t>(kErrPermission);
    Fiber* current = run.current;
    if (current->addr_context) {
        auto* s = new Saved;
        Save(c, *s, reinterpret_cast<uint64_t*>(c.Arg(1)));
        current->context = s;
        CheckOverflow(current);
    }
    current->state.store(StateIdle);
    if (run.arg_on_return) *run.arg_on_return = c.Arg(0);
    run.active = false;
    run.current = nullptr;
    Load(c, run.thread);
    return 0;  // sceFiberRun returns 0
}

uint64_t HleGetSelf(HleCall& c) {
    auto** out = reinterpret_cast<Fiber**>(c.Arg(0));
    if (!out) return static_cast<uint32_t>(kErrNull);
    if (!tls_run.active) return static_cast<uint32_t>(kErrPermission);
    *out = tls_run.current;
    return 0;
}

uint64_t HleGetInfo(HleCall& c) {
    auto* f = reinterpret_cast<Fiber*>(c.Arg(0));
    auto* info = reinterpret_cast<FiberInfo*>(c.Arg(1));
    if (!f || !info) return static_cast<uint32_t>(kErrNull);
    if ((reinterpret_cast<uintptr_t>(f) & 7) || (reinterpret_cast<uintptr_t>(info) & 7)) return static_cast<uint32_t>(kErrAlignment);
    if (info->size != sizeof(FiberInfo) || !Valid(f)) return static_cast<uint32_t>(kErrInvalid);
    info->entry = f->entry;
    info->arg_on_initialize = f->arg_on_initialize;
    info->addr_context = f->addr_context;
    info->size_context = f->size_context;
    std::memset(info->name, 0, sizeof(info->name));
    std::memcpy(info->name, f->name, kMaxName);
    info->size_context_margin = UINT64_MAX;
    uint64_t sig = 0;
    if ((f->flags & FlagContextSizeCheck) && f->addr_context && (std::memcpy(&sig, f->context_start, 8), sig == kStackSignature)) {
        auto* p = reinterpret_cast<uint64_t*>(f->context_start) + 1;
        while (p < reinterpret_cast<uint64_t*>(f->context_end) && *p == kStackSizeCheck) ++p;
        info->size_context_margin = static_cast<uint64_t>(reinterpret_cast<uint8_t*>(p) - reinterpret_cast<uint8_t*>(reinterpret_cast<uint64_t*>(f->context_start) + 1));
    }
    return 0;
}

uint64_t HleRename(HleCall& c) {
    auto* f = reinterpret_cast<Fiber*>(c.Arg(0));
    const auto* name = reinterpret_cast<const char*>(c.Arg(1));
    if (!f || !name) return static_cast<uint32_t>(kErrNull);
    if (reinterpret_cast<uintptr_t>(f) & 7) return static_cast<uint32_t>(kErrAlignment);
    if (!Valid(f)) return static_cast<uint32_t>(kErrInvalid);
    std::memset(f->name, 0, sizeof(f->name));
    std::strncpy(f->name, name, kMaxName);
    return 0;
}

uint64_t HleFramePointerAddress(HleCall& c) {
    auto* out = reinterpret_cast<uint64_t*>(c.Arg(0));
    if (!out) return static_cast<uint32_t>(kErrNull);
    if (!tls_run.active) return static_cast<uint32_t>(kErrPermission);
    *out = tls_run.thread.rbp;
    return 0;
}

uint64_t HleEntryReturned(HleCall&) {
    fprintf(stderr, "STOP: fiber entry function returned\n");
    LOGE("fiber entry function returned (fiber '%s')", tls_run.current ? tls_run.current->name : "?");
    return 0;
}

struct Export {
    const char* nid;
    guest::HleFunction fn;
};
const Export kExports[] = {
    {"hVYD7Ou2pCQ", HleInitialize},          // _sceFiberInitializeImpl
    {"7+OJIpko9RY", HleInitializeInternal},  // _sceFiberInitializeWithInternalOptionImpl
    {"asjUJJ+aa8s", HleOptParamInitialize}, {"JeNX5F-NzQU", HleFinalize},
    {"a0LLrZWac0M", HleRun}, {"PFT2S-tJ7Uk", HleSwitch},
    {"p+zLIOg27zU", HleGetSelf}, {"B0ZX2hx9DMw", HleReturnToThread},
    {"avfGJ94g36Q", HleAttachAndRun},     // _sceFiberAttachContextAndRun
    {"ZqhZFuzKT6U", HleAttachAndSwitch},  // _sceFiberAttachContextAndSwitch
    {"uq2Y5BFz0PE", HleGetInfo}, {"JzyT91ucGDc", HleRename},
    {"0dy4JtMUcMQ", HleFramePointerAddress},
};

}  // namespace

uint64_t FiberImport(const std::string& nid, StubArena& stubs) {
    const size_t length = nid.find('#') == std::string::npos ? nid.size() : nid.find('#');
    for (const auto& e : kExports) {
        if (std::strlen(e.nid) != length || nid.compare(0, length, e.nid) != 0) continue;
        auto& cpu = guest::GuestCpu::Get();
        if (!g_exit_stub) g_exit_stub = stubs.Make(cpu.RegisterHle("fiber_entry_returned", HleEntryReturned), /*halt=*/true);
        return stubs.Make(cpu.RegisterHle(e.nid, e.fn));
    }
    return 0;
}

}  // namespace sotc::loader
