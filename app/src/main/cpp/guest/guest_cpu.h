// SPDX-License-Identifier: GPL-2.0-or-later
// Guest CPU: runs the PS4 game's x86-64 code on the ARM64 host through FEXCore (JIT).
//
// Native (host) functions are reached from guest code through HLE stubs:
//     mov r10, rcx ; mov rax, kHleMagic | index ; syscall ; ret      (16 bytes)
// The guest calls a stub with the System V AMD64 ABI, FEX hands the syscall to our handler, the
// handler runs the registered host function and its return value lands in RAX. The loader (M2)
// will write one stub per imported NID, so `call [import]` in the game ends in native code.
// Host code can call back into guest code (callbacks, thread entry points) with Call().
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace FEXCore {
namespace Core {
struct CpuStateFrame;
struct InternalThreadState;
} // namespace Core
} // namespace FEXCore

namespace sotc::guest {

// RAX value for HLE calls: magic in the upper half, function index in the lower half.
constexpr uint64_t kHleMagic = 0x5C0C'D00D'0000'0000ull;
constexpr uint64_t kHleMagicMask = 0xFFFF'FFFF'0000'0000ull;

// View of a guest -> host call in progress (valid only inside the HLE function).
class HleCall {
public:
    HleCall(FEXCore::Core::CpuStateFrame* frame, uint32_t index, void* user)
        : frame_{frame}, index_{index}, user_{user} {}

    // Which registered function is running, and the pointer given to RegisterHle.
    uint32_t Index() const { return index_; }
    void* User() const { return user_; }

    // Integer/pointer argument i (System V): RDI, RSI, RDX, RCX, R8, R9, then the stack.
    uint64_t Arg(int i) const;
    // Floating-point argument i (XMM0..XMM7, low lane).
    double ArgF64(int i) const;
    float ArgF32(int i) const;
    // Sets XMM0 for functions returning float/double (RAX is still the integer return).
    void ReturnF64(double v);
    void ReturnF32(float v);

    // Raw low 64 bits of XMMi, read/written in place (no full register reconstruction).
    uint64_t XmmLow(int i) const;
    void SetXmmLow(int i, uint64_t v);
    void SetRdx(uint64_t v);

    uint64_t Rsp() const;
    FEXCore::Core::CpuStateFrame* Frame() const { return frame_; }

private:
    FEXCore::Core::CpuStateFrame* frame_;
    uint32_t index_;
    void* user_;
};

using HleFunction = uint64_t (*)(HleCall&);

// One guest thread: FEX thread state plus the stacks FEX needs. Runs on the host thread that
// calls GuestCpu::Run(); a guest thread must always run on the same host thread.
class GuestThread {
public:
    ~GuestThread();
    FEXCore::Core::InternalThreadState* Native() const { return native_; }
    uint64_t Reg(int index) const;        // FEXCore::X86State::REG_*
    void SetReg(int index, uint64_t v);
    uint64_t Rip() const;
    void SetRip(uint64_t rip);
    void SetFsBase(uint64_t base);        // PS4 TLS lives at fs:0
    void SetGsBase(uint64_t base);        // SotC-PC's linker rewrote the game's fs:[0] loads to gs:[0]

private:
    friend class GuestCpu;
    GuestThread() = default;
    FEXCore::Core::InternalThreadState* native_ = nullptr;
    void* callret_reservation_ = nullptr;
    struct Gdt;
    std::unique_ptr<Gdt> gdt_;
};

struct GuestCpuInfo {
    std::string fex_version;
    std::string host_features;  // short summary of what FEX detected on the host CPU
    bool tso = true;            // x86 memory ordering emulation enabled
};

class GuestCpu {
public:
    static GuestCpu& Get();

    // Creates the FEX context. `home` is a writable directory for FEX config/cache files.
    bool Init(const std::string& home, std::string* error);
    const GuestCpuInfo& Info() const { return info_; }

    // Registers a host function and returns its HLE index (for WriteHleStub). `user` is handed
    // back through HleCall::User(), so one host function can serve many stubs.
    uint32_t RegisterHle(const char* name, HleFunction fn, void* user = nullptr);
    const char* HleName(uint32_t index) const;
    static constexpr size_t kStubSize = 16;
    // `halt` ends the stub with HLT instead of RET: the guest thread leaves Run() right after the
    // host function returns (used for imports that must stop the guest).
    static void WriteHleStub(uint8_t* dst, uint32_t index, bool halt = false);

    std::unique_ptr<GuestThread> CreateThread(uint64_t rip, uint64_t rsp, uint64_t fs_base);

    // Runs guest code on `t` from its current RIP until it executes HLT.
    void Run(GuestThread& t);

    // Calls a guest function from host code. Inside an HLE function it reuses the calling guest
    // thread (nested JIT entry, guest registers are preserved); `t` selects the thread otherwise.
    uint64_t Call(uint64_t fn, std::initializer_list<uint64_t> args, GuestThread* t = nullptr);
    uint64_t Call(uint64_t fn, const uint64_t* args, size_t count, GuestThread* t = nullptr);

    // Registers a native C function for the generic bridge (host_bridge.S): System V integer
    // arguments (6 registers + 2 stack slots) to x0-x7, XMM0-7 to d0-d7; x0/x1 back to RAX/RDX,
    // d0/d1 to XMM0/XMM1. Covers non-variadic functions without struct-by-value arguments.
    uint32_t RegisterHostFunction(const char* name, const void* fn);

    // The guest context owned by the calling host thread (sotc_guest_attach), or null.
    GuestThread* CurrentThread() const;

    // SIGSEGV handler hook (the GPU library's page tracking): returns nonzero when it handled
    // the fault; otherwise the previous handler (debuggerd) gets it.
    static void SetFaultHook(int (*hook)(void* ucontext, void* address));

    // Must be called after guest code bytes change (patches, code loaded over old code).
    void InvalidateCode(uint64_t start, uint64_t length);

    uint64_t HleCallCount() const;

private:
    friend class HleCall;
    friend class GuestThread;
    GuestCpu() = default;
    struct Impl;
    Impl* impl_ = nullptr;
    GuestCpuInfo info_;
};

} // namespace sotc::guest
