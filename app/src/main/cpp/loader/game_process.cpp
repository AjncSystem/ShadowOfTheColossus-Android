// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_process.h"

#include <FEXCore/Core/X86Enums.h>

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../guest/guest_cpu.h"
#include "../guest/sotc_guest.h"
#include "../log.h"
#include "sotc_modes.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace sotc::loader {

namespace {

constexpr uint64_t kPage = 4096;
// SotC packs pointers to its static data into 31 bits: the image must sit below 2 GiB. Claimed top
// down, as on SotC-PC's Linux build.
constexpr uint64_t kEbootMin = 0x10000000;
constexpr uint64_t kEbootLimit = 0x80000000;
constexpr uint64_t kEbootReservation = 256ull << 20;
// SotC-PC keeps 16 MiB after the image for its CPU patch trampolines (not used on FEX yet).
constexpr uint64_t kTrampolineSize = 16ull << 20;
constexpr size_t kMainStack = 8u << 20;

uint64_t RoundPage(uint64_t v) {
    return (v + kPage - 1) & ~(kPage - 1);
}

void Appendf(std::string& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Appendf(std::string& s, const char* fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
    LOGI("%s", buf);
}

const struct {
    const char* nid;
    const char* symbol;
} kImportNames[] = {
#include "import_names.inc"
};

struct StopInfo {
    std::atomic<bool> stopped{false};
    uint32_t import_index = 0;
    uint64_t args[3]{};
    uint64_t return_address = 0;
    bool exited = false;
};
StopInfo g_stop;
const GameProcess* g_process = nullptr;

uint64_t HleStop(guest::HleCall& c) {
    const uint64_t ret = *reinterpret_cast<const uint64_t*>(c.Rsp());
    GameProcess::NoteStop(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.User())), c.Arg(0), c.Arg(1), c.Arg(2), ret);
    return 0;
}

uint64_t HleProcessExit(guest::HleCall&) {
    g_stop.exited = true;
    g_stop.stopped = true;
    LOGI("guest reached the process finalizer callback");
    return 0;
}

}  // namespace

const char* ImportSymbol(const std::string& scoped) {
    const size_t length = scoped.find('#') == std::string::npos ? scoped.size() : scoped.find('#');
    for (const auto& n : kImportNames)
        if (strlen(n.nid) == length && !memcmp(n.nid, scoped.data(), length)) return n.symbol;
    return "?";
}

// ---- StubArena --------------------------------------------------------------------------------

bool StubArena::Init(size_t capacity) {
    const size_t bytes = RoundPage(capacity * guest::GuestCpu::kStubSize);
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<uint8_t*>(p);
    capacity_ = capacity;
    used_ = 0;
    return true;
}

uint64_t StubArena::Make(uint32_t hle_index, bool halt) {
    if (used_ >= capacity_) return 0;
    uint8_t* at = base_ + used_ * guest::GuestCpu::kStubSize;
    guest::GuestCpu::WriteHleStub(at, hle_index, halt);
    ++used_;
    return reinterpret_cast<uint64_t>(at);
}

// ---- GameProcess ------------------------------------------------------------------------------

GameProcess::GameProcess() = default;
GameProcess::~GameProcess() = default;

void GameProcess::NoteStop(uint32_t import_index, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t ret_addr) {
    g_stop.import_index = import_index;
    g_stop.args[0] = a0;
    g_stop.args[1] = a1;
    g_stop.args[2] = a2;
    g_stop.return_address = ret_addr;
    g_stop.stopped = true;
}

bool GameProcess::ReserveLowRange(std::string* log) {
    for (uint64_t at = kEbootLimit - kEbootReservation; at >= kEbootMin; at -= 16ull << 20) {
        void* p = mmap(reinterpret_cast<void*>(at), kEbootReservation, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (reinterpret_cast<uint64_t>(p) != at) {  // kernel without MAP_FIXED_NOREPLACE
            munmap(p, kEbootReservation);
            continue;
        }
        reservation_ = static_cast<uint8_t*>(p);
        return true;
    }
    Appendf(*log, "no free 256 MiB range below 2 GiB for the eboot\n");
    return false;
}

bool GameProcess::Load(const std::string& boot_path, ImportResolver* resolver, std::string* log) {
    resolver_ = resolver;
    std::string error;
    if (!boot_.Parse(boot_path, &error)) {
        Appendf(*log, "boot image: %s\n", error.c_str());
        return false;
    }
    Appendf(*log, "boot image: %" PRIu64 " MiB, %zu segments, %zu relocations, %zu imports (%zu native exports), %zu modules\n",
            boot_.size >> 20, boot_.segments.size(), boot_.relocs.size(), boot_.names.size(),
            static_cast<size_t>(std::count_if(boot_.binding_address.begin(), boot_.binding_address.end(),
                                              [](uint64_t a) { return a != 0; })),
            boot_.modules.size());

    if (resolver_) resolver_->Prepare(boot_);
    if (!ReserveLowRange(log)) return false;
    const uint64_t mapped = RoundPage(boot_.size) + kTrampolineSize;
    if (mapped > kEbootReservation || mprotect(reservation_, mapped, PROT_READ | PROT_WRITE)) {
        Appendf(*log, "cannot commit the eboot range\n");
        return false;
    }
    image_ = reservation_;
    {
        FILE* f = fopen(boot_path.c_str(), "rb");
        const bool ok = f && fseek(f, static_cast<long>(boot_.image_file_offset), SEEK_SET) == 0 &&
                        fread(image_, 1, boot_.size, f) == boot_.size;
        if (f) fclose(f);
        if (!ok) {
            Appendf(*log, "cannot read the memory image\n");
            return false;
        }
    }
    Appendf(*log, "image at %p (below 2 GiB, as the game's 31-bit pointers need)\n", image_);
    g_process = this;

    if (resolver && !resolver->ImageLoaded(*this, log)) return false;
    if (!BindImports(resolver, log)) return false;

    for (const auto& r : boot_.relocs) {
        uint64_t value;
        if (r.kind == 0) value = reinterpret_cast<uint64_t>(image_) + r.value;
        else if (r.kind == 1) value = function_targets_[r.value];
        else value = data_targets_[r.value] + r.addend;
        std::memcpy(image_ + r.target, &value, 8);
    }
    Appendf(*log, "applied %zu relocations\n", boot_.relocs.size());
    // SotC-PC's game modes and engine command line (the launcher's settings), as probe.c applies
    // patches.bin and cmdline.bin after relocation.
    ApplySotcModes(image_, boot_.size, image_ + RoundPage(boot_.size), kTrampolineSize, log);
    Protect();
    return true;
}

bool GameProcess::BindImports(ImportResolver* resolver, std::string* log) {
    const size_t n = boot_.names.size();
    // Which kinds each import is used as.
    std::vector<uint8_t> used(n, 0);
    for (const auto& r : boot_.relocs)
        if (r.kind) used[r.value] |= static_cast<uint8_t>(r.kind);

    if (!stubs_.Init(n + 64)) {
        Appendf(*log, "cannot allocate import stubs\n");
        return false;
    }
    auto& cpu = guest::GuestCpu::Get();
    function_targets_.assign(n, 0);
    data_targets_.assign(n, 0);
    size_t host = 0, native = 0, stop = 0, data_host = 0, data_native = 0, data_trap = 0;
    for (size_t i = 0; i < n; ++i) {
        const std::string& nid = boot_.names[i];
        if (used[i] & 1) {
            uint64_t target = resolver ? resolver->Function(static_cast<uint32_t>(i), nid, stubs_) : 0;
            if (target) {
                ++host;
            } else if (boot_.binding_address[i] && boot_.binding_kind[i] == 1) {
                target = reinterpret_cast<uint64_t>(image_) + boot_.binding_address[i];
                ++native;
            } else {
                const uint32_t hle = cpu.RegisterHle(boot_.names[i].c_str(), HleStop, reinterpret_cast<void*>(i));
                target = stubs_.Make(hle, /*halt=*/true);
                ++stop;
            }
            function_targets_[i] = target;
        }
        if (used[i] & 2) {
            uint64_t target = resolver ? resolver->Data(static_cast<uint32_t>(i), nid) : 0;
            if (target) {
                ++data_host;
            } else if (boot_.binding_address[i] && boot_.binding_kind[i] == 2) {
                target = reinterpret_cast<uint64_t>(image_) + boot_.binding_address[i];
                ++data_native;
            } else {
                // Unbacked data import: an inaccessible page, so the first access faults.
                void* trap = mmap(nullptr, kPage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                data_traps_.push_back(trap);
                target = reinterpret_cast<uint64_t>(trap);
                ++data_trap;
                Appendf(*log, "data import without backing: %s (%s)\n", nid.c_str(), ImportSymbol(nid));
            }
            data_targets_[i] = target;
        }
    }
    Appendf(*log, "functions: %zu host, %zu native PRX exports, %zu stop stubs; data: %zu host, %zu native, %zu trap pages\n",
            host, native, stop, data_host, data_native, data_trap);
    return true;
}

void GameProcess::Protect() {
    // FEX reads guest code as data: executable segments only need to stay readable.
    mprotect(image_, RoundPage(boot_.size), PROT_NONE);
    for (const auto& s : boot_.segments) {
        const int prot = ((s.flags & 5) ? PROT_READ : 0) | ((s.flags & 2) ? PROT_WRITE : 0);
        mprotect(image_ + s.address, RoundPage(s.size), prot);
    }
}

uint64_t GameProcess::MakeTcb(uint64_t thread_handle) {
    // FreeBSD-style TCB, TLS variant II (static TLS right below the TCB), as SotC-PC
    // runtime_thread.c attach(): tcb[0] = self, tcb[1] = dtv, tcb[2] = thread handle.
    const MainTls& t = boot_.main_tls;
    const uint64_t align = t.align ? t.align : 16;
    const uint64_t offset = (t.memsz + align - 1) & ~(align - 1);
    // mmap, not malloc: Android's heap tags the top byte of its pointers, and the game packs
    // pointers into 40 bits.
    const size_t total = RoundPage(offset + 256);
    auto* block = static_cast<uint8_t*>(mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (block == MAP_FAILED) return 0;
    if (t.filesz) std::memcpy(block, image_ + t.vaddr, t.filesz);
    auto* tcb = reinterpret_cast<uint64_t*>(block + offset);
    static uint64_t dtv[3];
    tcb[0] = reinterpret_cast<uint64_t>(tcb);
    tcb[1] = reinterpret_cast<uint64_t>(dtv);
    tcb[2] = thread_handle;
    return reinterpret_cast<uint64_t>(tcb);
}

void GameProcess::RunMain(std::string* log) {
    using namespace FEXCore::X86State;
    auto& cpu = guest::GuestCpu::Get();

    if (resolver_ && resolver_->AttachMainThread(*this)) {
        Appendf(*log, "main thread: TCB and TLS from the runtime\n");
    } else {
        static uint8_t main_thread_record[4096];  // the guest's ScePthread handle for the main thread
        const uint64_t tcb = MakeTcb(reinterpret_cast<uint64_t>(main_thread_record));
        // fs and gs both point at the TCB (link_modules.py rewrote the eboot's fs:[0] to gs:[0]).
        if (!tcb || sotc_guest_attach(tcb, kMainStack) != 0) {
            Appendf(*log, "cannot create the guest main thread\n");
            return;
        }
        Appendf(*log, "main thread: minimal TCB 0x%" PRIx64 "\n", tcb);
    }
    guest::GuestThread* main_thread = cpu.CurrentThread();

    auto report_stop = [&](const char* phase) {
        if (g_stop.exited) {
            Appendf(*log, "%s: the guest called the process finalizer\n", phase);
            return;
        }
        const uint32_t i = g_stop.import_index;
        const std::string& nid = boot_.names[i];
        const uint64_t ret = g_stop.return_address - reinterpret_cast<uint64_t>(image_);
        Appendf(*log, "STOP in %s: first import without implementation: %s = %s\n", phase, nid.c_str(), ImportSymbol(nid));
        Appendf(*log, "  args 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 "; return address image+0x%" PRIx64, g_stop.args[0],
                g_stop.args[1], g_stop.args[2], ret);
        for (size_t m = 0; m < boot_.modules.size(); ++m)
            if (ret >= boot_.modules[m].base && ret - boot_.modules[m].base < boot_.modules[m].size)
                Appendf(*log, " (module %zu +0x%" PRIx64 ")", m, ret - boot_.modules[m].base);
        Appendf(*log, "\n");
    };

    // Module initializers in link order (libc first), as the PS4 dynamic linker does.
    for (size_t m = 0; m < boot_.modules.size(); ++m) {
        Appendf(*log, "starting linked module %zu (init image+0x%" PRIx64 ")\n", m, boot_.modules[m].init);
        const uint64_t result = cpu.Call(reinterpret_cast<uint64_t>(image_ + boot_.modules[m].init), {0, 0, 0}, main_thread);
        if (g_stop.stopped) {
            report_stop("module initializer");
            return;
        }
        Appendf(*log, "module %zu initializer returned %d\n", m, static_cast<int>(result));
        if (static_cast<int>(result) != 0) return;
    }

    // The eboot entry point: (params {argc, argv}, exit callback).
    static const char kArgv0[] = "/app0/eboot.bin";
    static struct {
        uint64_t argc;
        const char* argv[2];
    } params = {1, {kArgv0, nullptr}};
    const uint32_t exit_hle = cpu.RegisterHle("process_exit_callback", HleProcessExit);
    const uint64_t exit_stub = stubs_.Make(exit_hle, /*halt=*/true);
    Appendf(*log, "entering the eboot at image+0x%" PRIx64 "\n", boot_.entry);
    cpu.Call(reinterpret_cast<uint64_t>(image_ + boot_.entry), {reinterpret_cast<uint64_t>(&params), exit_stub},
             main_thread);
    if (g_stop.stopped) report_stop("eboot");
    else Appendf(*log, "the eboot entry point returned\n");
}

}  // namespace sotc::loader
