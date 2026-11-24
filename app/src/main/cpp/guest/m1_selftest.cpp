// SPDX-License-Identifier: GPL-2.0-or-later
// M1 self-test: proves guest x86-64 code runs through FEXCore and talks to host code.
//  1. HLE call with 8 integer arguments (6 in registers, 2 on the stack);
//  2. host -> guest callback from inside an HLE call (nested JIT entry), callee-saved registers
//     and RSP preserved;
//  3. floating-point argument and return through XMM0/XMM1;
//  4. two guest threads on two host threads, each reading its own TLS through fs:[0];
//  5. code invalidation after a guest code patch;
//  6. benchmarks: integer loop vs native, HLE call cost, guest memory copy bandwidth.
#include "m1_selftest.h"

#include <FEXCore/Core/X86Enums.h>

#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../log.h"
#include "guest_cpu.h"
#include "x86_asm.h"

namespace sotc::guest {

namespace {

using namespace x86;

void Appendf(std::string& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Appendf(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

double NowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct Region {
    void* p = MAP_FAILED;
    size_t n = 0;
    explicit Region(size_t size) : n{size} {
        p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    ~Region() {
        if (p != MAP_FAILED) munmap(p, n);
    }
    bool ok() const { return p != MAP_FAILED; }
    uint64_t addr() const { return reinterpret_cast<uint64_t>(p); }
    uint64_t top() const { return addr() + n - 64; }
};

// ---- host (HLE) functions called from guest code ----------------------------------------------

uint64_t HleSum8(HleCall& c) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r += c.Arg(i) * (i + 1);
    return r;
}

uint64_t HleCallGuestTwice(HleCall& c) {
    // Calls the guest function in arg0 with arg1, twice, from host code.
    const uint64_t fn = c.Arg(0);
    const uint64_t x = c.Arg(1);
    const uint64_t a = GuestCpu::Get().Call(fn, {x});
    const uint64_t b = GuestCpu::Get().Call(fn, {x + 1});
    return a + b;
}

uint64_t HleMulF64(HleCall& c) {
    c.ReturnF64(c.ArgF64(0) * c.ArgF64(1));
    return 0;
}

uint64_t HleNop(HleCall&) {
    return 0;
}

constexpr uint64_t kSentinelA = 0x1357'9bdf'2468'ace0ull;
constexpr uint64_t kSentinelB = 0x0246'8ace'1357'9bdfull;
constexpr uint64_t kCalleeSaved = 0x00c0'ffee'1234'5678ull;
constexpr uint64_t kLoopN = 200'000'000;
constexpr uint64_t kHleN = 2'000'000;
constexpr size_t kCopyBytes = 64u << 20;

struct Program {
    size_t main_sum8, main_callback, main_float, tls_read, patch_site, patch_imm, bench_loop, bench_hle, bench_copy;
    size_t stub_sum8, stub_cb, stub_mul, stub_nop, guest_square, f64_a, f64_b;
};

Program Build(Asm& a, uint32_t hle_sum8, uint32_t hle_cb, uint32_t hle_mul, uint32_t hle_nop) {
    Program p{};
    // HLE stubs first, so calls below can target them.
    auto stub = [&](uint32_t index) {
        a.Align(16);
        const size_t at = a.Pos();
        a.Reserve(GuestCpu::kStubSize);
        GuestCpu::WriteHleStub(a.At(at), index);
        return at;
    };
    p.stub_sum8 = stub(hle_sum8);
    p.stub_cb = stub(hle_cb);
    p.stub_mul = stub(hle_mul);
    p.stub_nop = stub(hle_nop);

    a.Align(8);
    p.f64_a = a.Pos();
    double fa = 1.5, fb = 2.25;
    uint64_t bits;
    std::memcpy(&bits, &fa, 8);
    a.Data64(bits);
    p.f64_b = a.Pos();
    std::memcpy(&bits, &fb, 8);
    a.Data64(bits);

    // guest_square(x) = x * x
    a.Align(16);
    p.guest_square = a.Pos();
    a.Mov(RAX, RDI);
    a.Imul(RAX, RDI);
    a.Ret();

    // 1. sum8(1..8) -> RBX
    a.Align(16);
    p.main_sum8 = a.Pos();
    a.MovImm(RDI, 1);
    a.MovImm(RSI, 2);
    a.MovImm(RDX, 3);
    a.MovImm(RCX, 4);
    a.MovImm(R8, 5);
    a.MovImm(R9, 6);
    a.PushImm8(8);
    a.PushImm8(7);
    a.Call(p.stub_sum8);
    a.AddImm8(RSP, 16);
    a.Mov(RBX, RAX);
    a.Hlt();

    // 2. callback: RAX = square(7) + square(8); R13 must survive
    a.Align(16);
    p.main_callback = a.Pos();
    a.MovImm(R13, kCalleeSaved);
    a.MovImm(RDI, 0);  // patched below with guest_square's absolute address
    const size_t square_imm = a.Pos() - 8;
    a.MovImm(RSI, 7);
    a.Call(p.stub_cb);
    a.Mov(R12, RAX);
    a.Hlt();
    // (absolute address is filled in by the caller once the code is placed)
    p.patch_imm = square_imm;

    // 3. float: R14 = bits(1.5 * 2.25)
    a.Align(16);
    p.main_float = a.Pos();
    a.MovsdRip(0, p.f64_a);
    a.MovsdRip(1, p.f64_b);
    a.Call(p.stub_mul);
    a.MovqFromXmm(R14, 0);
    a.Hlt();

    // 4. TLS: RAX = fs:[0]
    a.Align(16);
    p.tls_read = a.Pos();
    a.MovRaxFs0();
    a.Hlt();

    // 5. patch site: RAX = imm64, rewritten between runs
    a.Align(16);
    p.patch_site = a.Pos();
    a.MovImm(RAX, 0x1111);
    a.Hlt();

    // 6a. integer loop: rax = (rax + rcx) * 3, rcx = N..1
    a.Align(16);
    p.bench_loop = a.Pos();
    a.MovImm(RCX, kLoopN);
    a.XorEaxEax();
    const size_t loop = a.Pos();
    a.Add(RAX, RCX);
    a.ImulImm32(RAX, RAX, 3);
    a.Dec(RCX);
    a.Jnz(loop);
    a.Hlt();

    // 6b. HLE call cost: call nop stub N times
    a.Align(16);
    p.bench_hle = a.Pos();
    a.MovImm(RBX, kHleN);
    const size_t hle_loop = a.Pos();
    a.Call(p.stub_nop);
    a.Dec(RBX);
    a.Jnz(hle_loop);
    a.Hlt();

    // 6c. copy RCX qwords from [RSI] to [RDI]
    a.Align(16);
    p.bench_copy = a.Pos();
    const size_t copy_loop = a.Pos();
    a.LoadMem(RAX, RSI);
    a.StoreMem(RDI, RAX);
    a.AddImm8(RSI, 8);
    a.AddImm8(RDI, 8);
    a.Dec(RCX);
    a.Jnz(copy_loop);
    a.Hlt();
    return p;
}

uint64_t NativeLoop(uint64_t n) {
    uint64_t rax = 0;
    for (uint64_t rcx = n; rcx != 0; --rcx) {
        rax = (rax + rcx) * 3;
        __asm__ volatile("" : "+r"(rax));  // keep the loop scalar, like the guest's
    }
    return rax;
}

}  // namespace

bool RunM1SelfTest(const std::string& home, std::string* report) {
    using namespace FEXCore::X86State;
    std::string& r = *report;
    auto& cpu = GuestCpu::Get();
    std::string error;
    const double t_init = NowSeconds();
    if (!cpu.Init(home, &error)) {
        Appendf(r, "FEX init FAILED: %s\n", error.c_str());
        return false;
    }
    Appendf(r, "FEX %s initialized in %.1f ms\n", cpu.Info().fex_version.c_str(), (NowSeconds() - t_init) * 1e3);
    Appendf(r, "host: %s\n", cpu.Info().host_features.c_str());
    Appendf(r, "TSO emulation: %s\n", cpu.Info().tso ? "on" : "off (SOTC_FEX_TSO=0)");

    const uint32_t i_sum8 = cpu.RegisterHle("test.sum8", HleSum8);
    const uint32_t i_cb = cpu.RegisterHle("test.call_guest_twice", HleCallGuestTwice);
    const uint32_t i_mul = cpu.RegisterHle("test.mul_f64", HleMulF64);
    const uint32_t i_nop = cpu.RegisterHle("test.nop", HleNop);

    Asm a;
    Program p = Build(a, i_sum8, i_cb, i_mul, i_nop);
    Region code(64 * 1024), stack(1 << 20), tls_a(4096), tls_b(4096), copy_src(kCopyBytes), copy_dst(kCopyBytes);
    if (!code.ok() || !stack.ok() || !tls_a.ok() || !tls_b.ok() || !copy_src.ok() || !copy_dst.ok()) {
        r += "mmap FAILED\n";
        return false;
    }
    std::memcpy(code.p, a.Bytes().data(), a.Bytes().size());
    const uint64_t base = code.addr();
    const uint64_t square_abs = base + p.guest_square;
    std::memcpy(static_cast<uint8_t*>(code.p) + p.patch_imm, &square_abs, 8);
    std::memcpy(tls_a.p, &kSentinelA, 8);
    std::memcpy(tls_b.p, &kSentinelB, 8);

    auto thread = cpu.CreateThread(base + p.main_sum8, stack.top() & ~uint64_t{15}, tls_a.addr());
    if (!thread) {
        r += "CreateThread FAILED\n";
        return false;
    }
    const uint64_t rsp0 = thread->Reg(REG_RSP);
    bool all = true;
    auto check = [&](const char* name, bool ok, const std::string& detail) {
        Appendf(r, "[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
        all &= ok;
    };
    auto run_at = [&](size_t off) {
        thread->SetRip(base + off);
        thread->SetReg(REG_RSP, rsp0);
        cpu.Run(*thread);
    };
    char buf[160];

    // 1
    run_at(p.main_sum8);
    snprintf(buf, sizeof(buf), "rbx=%" PRIu64 " (want 204), rsp %s", thread->Reg(REG_RBX),
             thread->Reg(REG_RSP) == rsp0 ? "ok" : "MOVED");
    check("hle 8 args", thread->Reg(REG_RBX) == 204 && thread->Reg(REG_RSP) == rsp0, buf);

    // 2
    run_at(p.main_callback);
    snprintf(buf, sizeof(buf), "r12=%" PRIu64 " (want 113), r13 %s, rsp %s", thread->Reg(REG_R12),
             thread->Reg(REG_R13) == kCalleeSaved ? "kept" : "CLOBBERED", thread->Reg(REG_RSP) == rsp0 ? "ok" : "MOVED");
    check("host->guest callback", thread->Reg(REG_R12) == 49 + 64 && thread->Reg(REG_R13) == kCalleeSaved &&
                                      thread->Reg(REG_RSP) == rsp0, buf);

    // 3
    run_at(p.main_float);
    double got;
    const uint64_t bits = thread->Reg(REG_R14);
    std::memcpy(&got, &bits, 8);
    snprintf(buf, sizeof(buf), "xmm0=%g (want 3.375)", got);
    check("float arg/return", got == 3.375, buf);

    // 4
    std::atomic<int> tls_ok{0};
    auto tls_worker = [&](uint64_t fs, uint64_t want) {
        Region st(64 * 1024);
        auto t = cpu.CreateThread(base + p.tls_read, st.top() & ~uint64_t{15}, fs);
        if (!t) return;
        cpu.Run(*t);
        if (t->Reg(REG_RAX) == want) tls_ok.fetch_add(1);
    };
    std::thread ta(tls_worker, tls_a.addr(), kSentinelA);
    std::thread tb(tls_worker, tls_b.addr(), kSentinelB);
    ta.join();
    tb.join();
    snprintf(buf, sizeof(buf), "%d/2 threads read their own fs:[0]", tls_ok.load());
    check("threads + TLS", tls_ok.load() == 2, buf);

    // 5
    run_at(p.patch_site);
    const uint64_t before = thread->Reg(REG_RAX);
    const uint64_t patched = 0x2222;
    std::memcpy(static_cast<uint8_t*>(code.p) + p.patch_site + 2, &patched, 8);
    cpu.InvalidateCode(base + p.patch_site, 10);
    run_at(p.patch_site);
    snprintf(buf, sizeof(buf), "before 0x%" PRIx64 ", after 0x%" PRIx64, before, thread->Reg(REG_RAX));
    check("code invalidation", before == 0x1111 && thread->Reg(REG_RAX) == patched, buf);

    // 6a
    double t0 = NowSeconds();
    run_at(p.bench_loop);
    const double guest_s = NowSeconds() - t0;
    const uint64_t guest_result = thread->Reg(REG_RAX);
    t0 = NowSeconds();
    const uint64_t native_result = NativeLoop(kLoopN);
    const double native_s = NowSeconds() - t0;
    snprintf(buf, sizeof(buf), "guest %.0f ms, native %.0f ms -> guest at %.0f%% of native, results %s",
             guest_s * 1e3, native_s * 1e3, 100.0 * native_s / guest_s, guest_result == native_result ? "match" : "DIFFER");
    check("bench int loop 200M", guest_result == native_result, buf);

    // 6b
    const uint64_t calls_before = cpu.HleCallCount();
    t0 = NowSeconds();
    run_at(p.bench_hle);
    const double hle_s = NowSeconds() - t0;
    const uint64_t calls = cpu.HleCallCount() - calls_before;
    snprintf(buf, sizeof(buf), "%" PRIu64 " calls, %.0f ns per guest->host->guest round trip", calls, hle_s * 1e9 / kHleN);
    check("bench HLE call", calls == kHleN, buf);

    // 6c
    std::memset(copy_src.p, 0x5A, kCopyBytes);
    thread->SetReg(REG_RSI, copy_src.addr());
    thread->SetReg(REG_RDI, copy_dst.addr());
    thread->SetReg(REG_RCX, kCopyBytes / 8);
    t0 = NowSeconds();
    run_at(p.bench_copy);
    const double copy_s = NowSeconds() - t0;
    t0 = NowSeconds();
    std::memcpy(copy_dst.p, copy_src.p, kCopyBytes);
    const double memcpy_s = NowSeconds() - t0;
    const bool copied = std::memcmp(copy_src.p, copy_dst.p, kCopyBytes) == 0;
    snprintf(buf, sizeof(buf), "guest 8-byte loop %.2f GB/s, host memcpy %.2f GB/s", kCopyBytes / copy_s / 1e9,
             kCopyBytes / memcpy_s / 1e9);
    check("bench memory copy 64 MiB", copied, buf);

    Appendf(r, "M1 %s\n", all ? "OK" : "FAILED");
    return all;
}

}  // namespace sotc::guest
