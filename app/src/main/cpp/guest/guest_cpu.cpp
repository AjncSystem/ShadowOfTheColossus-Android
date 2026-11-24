// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_cpu.h"

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

#include "../log.h"
#include "sotc_guest.h"

namespace sotc::guest {

namespace {

using FEXCore::X86State::REG_R10;
using FEXCore::X86State::REG_R8;
using FEXCore::X86State::REG_R9;
using FEXCore::X86State::REG_RAX;
using FEXCore::X86State::REG_RCX;
using FEXCore::X86State::REG_RDI;
using FEXCore::X86State::REG_RDX;
using FEXCore::X86State::REG_RSI;
using FEXCore::X86State::REG_RSP;

constexpr size_t kPage = 4096;
constexpr size_t kMaxHle = 1 << 16;
constexpr size_t kRedZone = 128;
constexpr size_t kHaltOffset = 16;  // HLT byte in the callback page

// Guest thread currently executing on this host thread (inside Run/Call), for nested calls.
thread_local FEXCore::Core::InternalThreadState* tls_current = nullptr;

// Architectural guest registers, saved around a nested host -> guest call. (CPUState itself is
// not copyable: it also holds FEX bookkeeping such as an atomic signal counter.)
struct SavedRegs {
    using S = FEXCore::Core::CPUState;
    decltype(S::rip) rip;
    decltype(S::gregs) gregs;
    decltype(S::pf_raw) pf_raw;
    decltype(S::af_raw) af_raw;
    decltype(S::flags) flags;
    decltype(S::xmm) xmm;
    decltype(S::avx_high) avx_high;
    decltype(S::mm) mm;
    decltype(S::mxcsr) mxcsr;
    decltype(S::FCW) fcw;
    decltype(S::AbridgedFTW) ftw;
    decltype(S::callret_sp) callret_sp;

    void Save(const S& s) {
        rip = s.rip;
        std::memcpy(gregs, s.gregs, sizeof(gregs));
        pf_raw = s.pf_raw;
        af_raw = s.af_raw;
        std::memcpy(flags, s.flags, sizeof(flags));
        std::memcpy(&xmm, &s.xmm, sizeof(xmm));
        std::memcpy(avx_high, s.avx_high, sizeof(avx_high));
        std::memcpy(mm, s.mm, sizeof(mm));
        mxcsr = s.mxcsr;
        fcw = s.FCW;
        ftw = s.AbridgedFTW;
        callret_sp = s.callret_sp;
    }
    void Restore(S& s) const {
        s.rip = rip;
        std::memcpy(s.gregs, gregs, sizeof(gregs));
        s.pf_raw = pf_raw;
        s.af_raw = af_raw;
        std::memcpy(s.flags, flags, sizeof(flags));
        std::memcpy(&s.xmm, &xmm, sizeof(xmm));
        std::memcpy(s.avx_high, avx_high, sizeof(avx_high));
        std::memcpy(s.mm, mm, sizeof(mm));
        s.mxcsr = mxcsr;
        s.FCW = fcw;
        s.AbridgedFTW = ftw;
        s.callret_sp = callret_sp;
    }
};

void FexLog(LogMan::DebugLevels level, const char* message) {
    const int prio = level <= LogMan::ERROR ? ANDROID_LOG_ERROR
                   : level == LogMan::INFO  ? ANDROID_LOG_INFO
                                            : ANDROID_LOG_DEBUG;
    __android_log_print(prio, "SotC-FEX", "%s", message);
}

void FexThrow(const char* message) {
    __android_log_print(ANDROID_LOG_FATAL, "SotC-FEX", "assert: %s", message);
}

// x86 allows unaligned atomics, and with TSO emulation even plain guest loads/stores become
// ARM64 acquire/release instructions, which fault when unaligned. FEX emulates the access and
// backpatches the JIT code (half-barrier form) from the SIGBUS handler, as FEX's own Linux
// frontend and the Bachata-S4 core do. Other signals go to the previous handler (debuggerd).
struct sigaction g_previous_sigbus;
struct sigaction g_previous_sigsegv;
FEXCore::Context::Context* g_signal_ctx = nullptr;
std::atomic<int (*)(void*, void*)> g_fault_hook{nullptr};

void ChainSignal(const struct sigaction& previous, int sig, siginfo_t* info, void* raw) {
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) previous.sa_sigaction(sig, info, raw);
    } else if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else {
        previous.sa_handler(sig);
    }
}

void SigbusHandler(int sig, siginfo_t* info, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    auto* th = tls_current;
    if (th && g_signal_ctx && info && info->si_code == BUS_ADRALN) {
        const uintptr_t pc = uc->uc_mcontext.pc;
        if (g_signal_ctx->IsAddressInCodeBuffer(th, pc)) {
            const auto adjust = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
                th, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc,
                reinterpret_cast<uint64_t*>(uc->uc_mcontext.regs));
            if (adjust) {
                uc->uc_mcontext.pc = pc + *adjust;
                return;
            }
        }
    }
    ChainSignal(g_previous_sigbus, sig, info, raw);
}

// SIGSEGV: the GPU library write/read-protects guest pages it tracks (page_manager.cpp); a fault
// there is handled by unprotecting and re-running the instruction, as SotC-PC's probe.c does.
void SigsegvHandler(int sig, siginfo_t* info, void* raw) {
    if (auto* hook = g_fault_hook.load(std::memory_order_acquire); hook && info) {
        if (hook(raw, info->si_addr)) return;
    }
    ChainSignal(g_previous_sigsegv, sig, info, raw);
}

class CallbackSignalDelegator final : public FEXCore::SignalDelegator {
public:
    explicit CallbackSignalDelegator(uintptr_t callback_ret) : callback_ret_{callback_ret} {}
    uintptr_t GetThunkCallbackRET() const override { return callback_ret_; }

private:
    uintptr_t callback_ret_;
};

} // namespace

struct GuestThread::Gdt {
    std::array<FEXCore::Core::CPUState::gdt_segment, 32> entries{};
};

struct GuestCpu::Impl {
    class Syscalls final : public FEXCore::HLE::SyscallHandler {
    public:
        explicit Syscalls(Impl* impl) : impl_{impl} { OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC; }

        // With OS_GENERIC, FEX spills every guest register to frame->State before this call and
        // reloads them afterwards; it passes no arguments and does not store the result, so the
        // handler reads RAX from the state and writes the return value back itself.
        uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments*) override {
            auto& gregs = frame->State.gregs;
            const uint64_t rax = gregs[REG_RAX];
            uint64_t result;
            if ((rax & kHleMagicMask) != kHleMagic) {
                LOGW("guest made a raw syscall %llu at rip 0x%llx; returning -ENOSYS",
                     static_cast<unsigned long long>(rax), static_cast<unsigned long long>(frame->State.rip));
                result = static_cast<uint64_t>(-ENOSYS);
            } else if (const uint32_t index = static_cast<uint32_t>(rax);
                       index >= impl_->hle_count.load(std::memory_order_acquire)) {
                LOGE("HLE index %u out of range", index);
                result = static_cast<uint64_t>(-ENOSYS);
            } else {
                impl_->calls.fetch_add(1, std::memory_order_relaxed);
                const auto& entry = impl_->hle[index];
                HleCall call{frame, index, entry.user};
                result = entry.fn(call);
            }
            gregs[REG_RAX] = result;
            return result;
        }

        FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
            // M1: everything is executable. The loader (M2) will answer from the guest image map.
            return {0, std::numeric_limits<uint64_t>::max(), true};
        }

        std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*,
                                                                                       uint64_t) override {
            return std::nullopt;
        }

    private:
        Impl* impl_;
    };

    struct Entry {
        const char* name = nullptr;
        HleFunction fn = nullptr;
        void* user = nullptr;
    };

    fextl::unique_ptr<FEXCore::Context::Context> ctx;
    std::unique_ptr<CallbackSignalDelegator> delegator;
    std::unique_ptr<Syscalls> syscalls;
    FEXCore::HostFeatures host{};
    void* callback_page = nullptr;

    std::array<Entry, kMaxHle> hle{};
    std::atomic<uint32_t> hle_count{0};
    std::mutex hle_mutex;
    std::atomic<uint64_t> calls{0};

    std::mutex threads_mutex;
    std::vector<FEXCore::Core::InternalThreadState*> threads;
};

// ---- HleCall ----------------------------------------------------------------------------------

uint64_t HleCall::Arg(int i) const {
    static constexpr int kRegs[6] = {REG_RDI, REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9};
    const auto& s = frame_->State;
    if (i < 6) return s.gregs[kRegs[i]];
    // [rsp] is the return address pushed by `call stub`; stack arguments follow it.
    const uint64_t addr = s.gregs[REG_RSP] + 8 + 8 * static_cast<uint64_t>(i - 6);
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    return v;
}

double HleCall::ArgF64(int i) const {
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm{};
    GuestCpu::Get().impl_->ctx->ReconstructXMMRegisters(tls_current, xmm.data(), nullptr);
    double v;
    std::memcpy(&v, &xmm[i], sizeof(v));
    return v;
}

float HleCall::ArgF32(int i) const {
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm{};
    GuestCpu::Get().impl_->ctx->ReconstructXMMRegisters(tls_current, xmm.data(), nullptr);
    float v;
    std::memcpy(&v, &xmm[i], sizeof(v));
    return v;
}

void HleCall::ReturnF64(double v) {
    auto* ctx = GuestCpu::Get().impl_->ctx.get();
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm{};
    ctx->ReconstructXMMRegisters(tls_current, xmm.data(), nullptr);
    xmm[0] = 0;
    std::memcpy(&xmm[0], &v, sizeof(v));
    ctx->SetXMMRegistersFromState(tls_current, xmm.data(), nullptr);
}

void HleCall::ReturnF32(float v) {
    auto* ctx = GuestCpu::Get().impl_->ctx.get();
    std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> xmm{};
    ctx->ReconstructXMMRegisters(tls_current, xmm.data(), nullptr);
    xmm[0] = 0;
    std::memcpy(&xmm[0], &v, sizeof(v));
    ctx->SetXMMRegistersFromState(tls_current, xmm.data(), nullptr);
}

uint64_t HleCall::Rsp() const {
    return frame_->State.gregs[REG_RSP];
}

// Where FEX keeps XMMi in CPUState depends on the host (SSE layout: 16-byte registers with the
// AVX upper halves elsewhere; AVX layout: 32-byte registers). Found once by probing a thread.
namespace {
enum class XmmLayout { Unknown, Sse, Avx };
std::atomic<XmmLayout> g_xmm_layout{XmmLayout::Unknown};

uint64_t* XmmSlot(FEXCore::Core::CPUState& s, int i) {
    return g_xmm_layout.load(std::memory_order_relaxed) == XmmLayout::Avx ? &s.xmm.avx.data[i][0] : &s.xmm.sse.data[i][0];
}
}  // namespace

uint64_t HleCall::XmmLow(int i) const {
    return *XmmSlot(frame_->State, i);
}

void HleCall::SetXmmLow(int i, uint64_t v) {
    uint64_t* slot = XmmSlot(frame_->State, i);
    slot[0] = v;
    slot[1] = 0;
}

void HleCall::SetRdx(uint64_t v) {
    frame_->State.gregs[REG_RDX] = v;
}

// ---- GuestThread ------------------------------------------------------------------------------

namespace {
constexpr size_t kCallRetReservation = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * kPage;
}

GuestThread::~GuestThread() {
    if (native_) {
        auto* impl = GuestCpu::Get().impl_;
        {
            std::scoped_lock lock{impl->threads_mutex};
            std::erase(impl->threads, native_);
        }
        impl->ctx->DestroyThread(native_);
    }
    if (callret_reservation_) munmap(callret_reservation_, kCallRetReservation);
}

uint64_t GuestThread::Reg(int index) const {
    return native_->CurrentFrame->State.gregs[index];
}

void GuestThread::SetReg(int index, uint64_t v) {
    native_->CurrentFrame->State.gregs[index] = v;
}

uint64_t GuestThread::Rip() const {
    return native_->CurrentFrame->State.rip;
}

void GuestThread::SetRip(uint64_t rip) {
    native_->CurrentFrame->State.rip = rip;
}

void GuestThread::SetFsBase(uint64_t base) {
    native_->CurrentFrame->State.fs_cached = base;
}

void GuestThread::SetGsBase(uint64_t base) {
    native_->CurrentFrame->State.gs_cached = base;
}

// ---- GuestCpu ---------------------------------------------------------------------------------

GuestCpu& GuestCpu::Get() {
    static GuestCpu instance;
    return instance;
}

bool GuestCpu::Init(const std::string& home, std::string* error) {
    if (impl_) return true;
    auto* impl = new Impl();

    // FEX looks for its config under $HOME/.fex-emu; point it at app storage.
    setenv("HOME", home.c_str(), 1);
    FEX::Config::InitializeConfigs(FEX::Config::PortableInformation{});
    FEXCore::Config::Initialize();
    FEXCore::Config::Load();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
    const char* tso = getenv("SOTC_FEX_TSO");
    info_.tso = !(tso && tso[0] == '0');
    FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, info_.tso ? "1" : "0");
    LogMan::Msg::InstallHandler(FexLog);
    LogMan::Throw::InstallHandler(FexThrow);

    impl->host = FEX::FetchHostFeatures();
    const auto& h = impl->host;
    info_.host_features = std::string("atomics=") + (h.SupportsAtomics ? "1" : "0") +
                          " rcpc=" + (h.SupportsRCPC ? "1" : "0") + " tsoimm9=" + (h.SupportsTSOImm9 ? "1" : "0") +
                          " afp=" + (h.SupportsAFP ? "1" : "0") + " flagm=" + (h.SupportsFlagM ? "1" : "0") +
                          " flagm2=" + (h.SupportsFlagM2 ? "1" : "0") + " sve128=" + (h.SupportsSVE128 ? "1" : "0") +
                          " sve256=" + (h.SupportsSVE256 ? "1" : "0") + " avx=" + (h.SupportsAVX ? "1" : "0") +
                          " preserve_all=" + (h.SupportsPreserveAllABI ? "1" : "0");
    info_.fex_version = SOTC_FEX_REVISION;

    impl->ctx = FEXCore::Context::Context::CreateNewContext(impl->host);
    if (!impl->ctx) {
        *error = "FEX CreateNewContext failed";
        delete impl;
        return false;
    }

    // Guest callbacks return to a CALLBACKRET (0F 3E) instruction, which leaves the JIT.
    impl->callback_page = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (impl->callback_page == MAP_FAILED) {
        *error = "mmap callback page failed";
        delete impl;
        return false;
    }
    static constexpr uint8_t kCallbackRet[] = {0x0f, 0x3e};
    std::memcpy(impl->callback_page, kCallbackRet, sizeof(kCallbackRet));
    // Return address for top-level calls (Call() outside a running guest): HLT leaves the JIT.
    static_cast<uint8_t*>(impl->callback_page)[kHaltOffset] = 0xf4;
    mprotect(impl->callback_page, kPage, PROT_READ | PROT_EXEC);

    impl->delegator = std::make_unique<CallbackSignalDelegator>(reinterpret_cast<uintptr_t>(impl->callback_page));
    impl->syscalls = std::make_unique<Impl::Syscalls>(impl);
    impl->ctx->SetSignalDelegator(impl->delegator.get());
    impl->ctx->SetSyscallHandler(impl->syscalls.get());
    impl->ctx->EnableExitOnHLT();
    if (!impl->ctx->InitCore()) {
        *error = "FEX InitCore failed";
        delete impl;
        return false;
    }
    impl_ = impl;

    g_signal_ctx = impl->ctx.get();
    struct sigaction sa {};
    sa.sa_sigaction = SigbusHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_previous_sigbus);
    struct sigaction segv {};
    segv.sa_sigaction = SigsegvHandler;
    segv.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&segv.sa_mask);
    sigaction(SIGSEGV, &segv, &g_previous_sigsegv);
    return true;
}

uint32_t GuestCpu::RegisterHle(const char* name, HleFunction fn, void* user) {
    std::scoped_lock lock{impl_->hle_mutex};
    const uint32_t index = impl_->hle_count.load(std::memory_order_relaxed);
    if (index >= kMaxHle) {
        LOGE("HLE table full (%s)", name);
        return 0;
    }
    impl_->hle[index] = {name, fn, user};
    impl_->hle_count.store(index + 1, std::memory_order_release);
    return index;
}

const char* GuestCpu::HleName(uint32_t index) const {
    return index < impl_->hle_count.load(std::memory_order_acquire) ? impl_->hle[index].name : "?";
}

void GuestCpu::WriteHleStub(uint8_t* dst, uint32_t index, bool halt) {
    const uint64_t rax = kHleMagic | index;
    uint8_t stub[kStubSize] = {
        0x4c, 0x8b, 0xd1,  // mov r10, rcx   (rcx is clobbered by syscall)
        0x48, 0xb8,        // mov rax, imm64
        0, 0, 0, 0, 0, 0, 0, 0,
        0x0f, 0x05,        // syscall
        static_cast<uint8_t>(halt ? 0xf4 : 0xc3),  // hlt / ret
    };
    std::memcpy(stub + 5, &rax, sizeof(rax));
    std::memcpy(dst, stub, sizeof(stub));
}

std::unique_ptr<GuestThread> GuestCpu::CreateThread(uint64_t rip, uint64_t rsp, uint64_t fs_base) {
    std::unique_ptr<GuestThread> t{new GuestThread()};
    t->callret_reservation_ = mmap(nullptr, kCallRetReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t->callret_reservation_ == MAP_FAILED) {
        t->callret_reservation_ = nullptr;
        return nullptr;
    }
    // Guard page on each side of FEX's call/return prediction stack.
    void* base = static_cast<uint8_t*>(t->callret_reservation_) + kPage;
    if (mprotect(base, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE) != 0) return nullptr;

    t->native_ = impl_->ctx->CreateThread(rip, rsp);
    if (!t->native_) return nullptr;
    t->native_->CallRetStackBase = base;
    auto& s = t->native_->CurrentFrame->State;
    s.callret_sp = reinterpret_cast<uint64_t>(base) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;

    // Flat 64-bit code segment.
    t->gdt_ = std::make_unique<GuestThread::Gdt>();
    s.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = t->gdt_->entries.data();
    s.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = t->gdt_->entries.data();
    s.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto* cs = FEXCore::Core::CPUState::GetSegmentFromIndex(s, s.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(cs, 0);
    FEXCore::Core::CPUState::SetGDTLimit(cs, 0xF'FFFFU);
    s.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*cs);
    cs->L = 1;
    cs->D = 0;
    s.fs_cached = fs_base;

    if (g_xmm_layout.load(std::memory_order_relaxed) == XmmLayout::Unknown) {
        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> probe{};
        const uint64_t mark = 0x5C0C'0000'0000'0001ull;
        probe[1] = mark;
        impl_->ctx->SetXMMRegistersFromState(t->native_, probe.data(), nullptr);
        XmmLayout found = XmmLayout::Unknown;
        if (s.xmm.sse.data[1][0] == mark) found = XmmLayout::Sse;
        else if (s.xmm.avx.data[1][0] == mark) found = XmmLayout::Avx;
        probe[1] = 0;
        impl_->ctx->SetXMMRegistersFromState(t->native_, probe.data(), nullptr);
        if (found == XmmLayout::Unknown) LOGE("cannot locate XMM registers in FEX CPUState");
        else LOGI("FEX keeps XMM registers in the %s layout", found == XmmLayout::Sse ? "SSE" : "AVX");
        g_xmm_layout.store(found, std::memory_order_relaxed);
    }

    std::scoped_lock lock{impl_->threads_mutex};
    impl_->threads.push_back(t->native_);
    return t;
}

void GuestCpu::Run(GuestThread& t) {
    auto* prev = tls_current;
    tls_current = t.native_;
    impl_->ctx->ExecuteThread(t.native_);
    tls_current = prev;
}

uint64_t GuestCpu::Call(uint64_t fn, std::initializer_list<uint64_t> args, GuestThread* t) {
    return Call(fn, args.begin(), args.size(), t);
}

uint64_t GuestCpu::Call(uint64_t fn, const uint64_t* args, size_t n, GuestThread* t) {
    FEXCore::Core::InternalThreadState* th = t ? t->native_ : tls_current;
    if (!th) {
        LOGE("GuestCpu::Call without a guest thread");
        return 0;
    }
    auto& s = th->CurrentFrame->State;
    SavedRegs saved;  // caller's guest state (inside an HLE call)
    saved.Save(s);

    static constexpr int kRegs[6] = {REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9};
    const size_t stack_args = n > 6 ? n - 6 : 0;
    uint64_t rsp = s.gregs[REG_RSP] - kRedZone - 8 * stack_args;
    rsp &= ~uint64_t{15};  // 16-aligned at the call; HandleCallback pushes the return address
    for (size_t i = 0; i < n; ++i) {
        if (i < 6) {
            s.gregs[kRegs[i]] = args[i];
        } else {
            std::memcpy(reinterpret_cast<void*>(rsp + 8 * (i - 6)), &args[i], sizeof(args[i]));
        }
    }
    auto* prev = tls_current;
    if (prev == th) {
        // Nested: we are inside an HLE call of this very thread, so the JIT is running and
        // HandleCallback can re-enter it (it pushes its own CALLBACKRET return address).
        s.gregs[REG_RSP] = rsp;
        impl_->ctx->HandleCallback(th, fn);
    } else {
        // Top level: no JIT frame to return to. Push a return address that points at HLT and
        // run the thread from `fn` until it halts.
        rsp -= 8;
        const uint64_t halt = reinterpret_cast<uint64_t>(impl_->callback_page) + kHaltOffset;
        std::memcpy(reinterpret_cast<void*>(rsp), &halt, sizeof(halt));
        s.gregs[REG_RSP] = rsp;
        s.rip = fn;
        tls_current = th;
        impl_->ctx->ExecuteThread(th);
    }
    tls_current = prev;

    const uint64_t ret = s.gregs[REG_RAX];
    saved.Restore(s);
    return ret;
}

void GuestCpu::InvalidateCode(uint64_t start, uint64_t length) {
    std::scoped_lock code_lock{impl_->ctx->GetCodeInvalidationMutex()};
    impl_->ctx->InvalidateCodeBuffersCodeRange(start, length);
    std::scoped_lock lock{impl_->threads_mutex};
    for (auto* th : impl_->threads) impl_->ctx->InvalidateThreadCachedCodeRange(th, start, length);
}

uint64_t GuestCpu::HleCallCount() const {
    return impl_ ? impl_->calls.load(std::memory_order_relaxed) : 0;
}

// ---- generic bridge to native functions -------------------------------------------------------

namespace {
uint64_t HleHostCall(HleCall& c) {
    uint64_t ints[8], fps[8], out[3];
    for (int i = 0; i < 8; ++i) ints[i] = c.Arg(i);
    for (int i = 0; i < 8; ++i) fps[i] = c.XmmLow(i);
    const uint64_t rax = sotc_call_host(c.User(), ints, fps, out);
    c.SetRdx(out[0]);
    // XMM0/XMM1 are caller-saved in System V: always handing back d0/d1 is harmless for
    // functions that return an integer, and right for float/double/16-byte FP returns.
    c.SetXmmLow(0, out[1]);
    c.SetXmmLow(1, out[2]);
    return rax;
}

// Host thread -> its guest context, for threads attached through sotc_guest_attach.
struct OwnedThread {
    std::unique_ptr<GuestThread> thread;
    void* stack = nullptr;
    size_t stack_size = 0;
    ~OwnedThread() {
        thread.reset();
        if (stack) munmap(stack, stack_size);
    }
};
thread_local OwnedThread tls_owned;
}  // namespace

uint32_t GuestCpu::RegisterHostFunction(const char* name, const void* fn) {
    return RegisterHle(name, HleHostCall, const_cast<void*>(fn));
}

GuestThread* GuestCpu::CurrentThread() const {
    return tls_owned.thread.get();
}

void GuestCpu::SetFaultHook(int (*hook)(void* ucontext, void* address)) {
    g_fault_hook.store(hook, std::memory_order_release);
}

} // namespace sotc::guest

// ---- C interface for the runtime (sotc_guest.h) ------------------------------------------------

using sotc::guest::GuestCpu;

extern "C" int sotc_guest_attach(uint64_t tcb, uint64_t stack_size) {
    auto& owned = sotc::guest::tls_owned;
    if (owned.thread) {
        owned.thread->SetFsBase(tcb);
        owned.thread->SetGsBase(tcb);
        return 0;
    }
    if (stack_size < (64u << 10)) stack_size = 64u << 10;
    stack_size = (stack_size + 0xFFFF) & ~uint64_t{0xFFFF};
    // Guest stacks are separate from the host thread's stack (the JIT runs on that one).
    void* stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stack == MAP_FAILED) return -1;
    const uint64_t top = (reinterpret_cast<uint64_t>(stack) + stack_size - 64) & ~uint64_t{15};
    auto t = GuestCpu::Get().CreateThread(0, top, tcb);
    if (!t) {
        munmap(stack, stack_size);
        return -1;
    }
    t->SetGsBase(tcb);
    owned.thread = std::move(t);
    owned.stack = stack;
    owned.stack_size = stack_size;
    return 0;
}

extern "C" void sotc_guest_detach(void) {
    auto& owned = sotc::guest::tls_owned;
    owned.thread.reset();
    if (owned.stack) munmap(owned.stack, owned.stack_size);
    owned.stack = nullptr;
}

extern "C" int sotc_guest_attached(void) {
    return sotc::guest::tls_owned.thread != nullptr;
}

extern "C" uint64_t sotc_guest_call(const void* fn, unsigned count, const uint64_t* args) {
    auto& cpu = GuestCpu::Get();
    return cpu.Call(reinterpret_cast<uint64_t>(fn), args, count, cpu.CurrentThread());
}
