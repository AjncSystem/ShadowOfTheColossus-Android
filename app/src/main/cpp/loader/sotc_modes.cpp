// SPDX-License-Identifier: GPL-2.0-or-later
#include "sotc_modes.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void bbgpu_set_fps_target_slot(void* slot);
extern "C" void runtime_memory_set_release_site(uintptr_t site);

namespace sotc::loader {
namespace {

// Same tables as scripts/sotc_modes.py (EU 1.01 CUSA08809 / US 1.00 CUSA08034).
constexpr uint64_t kBase = 0x400000;
constexpr uint64_t kCmdlineLea = 0x009367b9;
constexpr const char* kCmdlineOriginal = "+VSyncWindow=10%";

struct Patch {
    uint64_t address;
    const char* original;  // hex
    const char* bytes;     // hex
};

struct Mode {
    const char* name;
    std::vector<Patch> patches;
    std::vector<const char*> cvars;
};

const std::vector<Mode>& Modes() {
    static const std::vector<Mode> modes = {
        // the frame limiter's import stub (sceVideoOutSetFlipRate) returns 0
        {"fps60", {{0x01390090, "ff25a20567", "31c0c3"}}, {}},
        // room for 4x the grass (virtual geometry buffers, patch chunks, weed blades)
        {"grassbuf",
         {{0x004d92b9, "b900800100", "b900000600"},
          {0x004d92e5, "b900800100", "b900000600"},
          {0x004d928d, "b900800000", "b900000200"},
          {0x004d9311, "b900400200", "b900000900"}},
         {}},
        // the engine's own 60 FPS target (PS4 Pro Performance mode), with its timing
        {"fps60game",
         {{0x00b0a7a6, "c745b802000000", "c745b801000000"},
          {0x0089a47f, "c7458402000000", "c7458401000000"},
          {0x0089a499, "c7458002000000", "c7458001000000"},
          {0x00b0a886, "c745c802000000", "c745c801000000"}},
         {"TargetFramesPerSeconds=1"}},
        {"af16",
         {},
         {"progSamplerAnisoLevel=16", "progSamplerAnisoLevel_BasePS4=16", "progSamplerAnisoLevel_Perf=16",
          "progSamplerAnisoLevel_Cinematic=16", "linearSamplerAnisoLevel=16"}},
        {"hdr", {}, {"r_WantsHDR=0", "r_WantsHDR_Game=2"}},
    };
    return modes;
}

// The page allocator fix (sotc_modes.py POOL_FIX): on a failed bitmap scan, zero the free-page
// tally and carve a new chunk instead of reading bitmap[-1].
const Patch kPoolFix = {0x41a321, "eb34666666662e0f1f840000000000", "41c7467400000000" "31c0" "e9c1feffff"};
const Patch kVersionChecks[] = {{kCmdlineLea, "488d352debad00", nullptr}, {0x01390090, "ff25a20567", nullptr}};

void Appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

std::vector<uint8_t> Hex(const char* hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        char b[3] = {hex[i], hex[i + 1], 0};
        out.push_back(static_cast<uint8_t>(strtoul(b, nullptr, 16)));
    }
    return out;
}

bool Holds(const uint8_t* image, uint64_t size, uint64_t address, const char* hex) {
    const std::vector<uint8_t> want = Hex(hex);
    const uint64_t at = address - kBase;
    return at + want.size() <= size && std::memcmp(image + at, want.data(), want.size()) == 0;
}

void Write(uint8_t* image, const Patch& p) {
    const std::vector<uint8_t> data = Hex(p.bytes);
    std::memcpy(image + (p.address - kBase), data.data(), data.size());
}

std::string Env(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

std::vector<std::string> Split(const std::string& s, const char* separators) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::strchr(separators, c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// sotc_modes.py CVAR: NAME, NAME=VALUE or NAME(ARGS).
bool ValidCvar(const std::string& c) {
    if (c.empty() || !(std::isalpha(static_cast<unsigned char>(c[0])) || c[0] == '_')) return false;
    size_t i = 0;
    while (i < c.size() && (std::isalnum(static_cast<unsigned char>(c[i])) || c[i] == '_')) ++i;
    if (i == c.size()) return true;
    auto value_char = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || std::strchr("-+._%", ch);
    };
    if (c[i] == '=') {
        if (i + 1 == c.size()) return false;
        for (size_t j = i + 1; j < c.size(); ++j)
            if (!value_char(c[j])) return false;
        return true;
    }
    if (c[i] == '(' && c.back() == ')') {
        for (size_t j = i + 1; j + 1 < c.size(); ++j)
            if (!value_char(c[j]) && c[j] != ',' && c[j] != ' ') return false;
        return true;
    }
    return false;
}

}  // namespace

void ApplySotcModes(uint8_t* image, uint64_t size, uint8_t* cmdline_area, uint64_t cmdline_area_size,
                    std::string* log) {
    std::string& out = *log;
    bool supported = true;
    for (const Patch& v : kVersionChecks) supported = supported && Holds(image, size, v.address, v.original);
    if (!supported) {
        Appendf(out, "Modes: this eboot is not EU 1.01 / US 1.00, no patches applied\n");
        return;
    }

    size_t writes = 0;
    std::vector<std::string> cvars;
    const bool pool_fix = Holds(image, size, kPoolFix.address, kPoolFix.original);
    if (pool_fix) {
        Write(image, kPoolFix);
        ++writes;
    }
    for (const std::string& raw : Split(Env("SOTC_MODES"), ", ")) {
        std::string name;
        for (char c : raw) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const Mode* mode = nullptr;
        for (const Mode& m : Modes())
            if (name == m.name) mode = &m;
        if (!mode) {
            Appendf(out, "Mode %s: unknown, ignored\n", name.c_str());
            continue;
        }
        bool ok = true;
        for (const Patch& p : mode->patches) ok = ok && Holds(image, size, p.address, p.original);
        if (!ok) {
            Appendf(out, "Mode %s: unexpected bytes, not applied\n", name.c_str());
            continue;
        }
        for (const Patch& p : mode->patches) Write(image, p);
        writes += mode->patches.size();
        for (const char* c : mode->cvars) cvars.emplace_back(c);
        Appendf(out, "Mode %s: on\n", name.c_str());
    }

    // PS4 Pro mode: its larger render targets push the free system memory figure under the
    // texture pool reserve (textures dropping to mip 3): reserve 0, which needs the pool fix.
    const std::string reserve = Env("SOTC_POOL_RESERVE");
    if (pool_fix && reserve != "1" && (reserve == "0" || Env("SOTC_NEO") == "1")) {
        cvars.emplace_back("texturePoolReserveSystemMemMiBs=0");
        Appendf(out, "Texture pool reserve: 0 (page allocator fix on)\n");
    }
    for (std::string c : Split(Env("SOTC_CVARS"), " \t\r\n")) {
        while (!c.empty() && c[0] == '+') c.erase(0, 1);
        if (!ValidCvar(c)) {
            Appendf(out, "cvar %s: not NAME=VALUE, ignored\n", c.c_str());
            continue;
        }
        cvars.push_back(c);
    }
    Appendf(out, "Patches: %zu writes\n", writes);

    // fps60game: the engine's TargetFramesPerSeconds cvar (object 0x2eed7a0, value at +0x60),
    // which the presenter's frame cap follows (probe.c).
    if (size > 0x2eed800 && Holds(image, size, 0x0089a47f + 0, "c7458401000000"))
        bbgpu_set_fps_target_slot(image + 0x2eed7a0 + 0x60);
    // T38: the page pool release's VirtualQuery call (runtime_memory.c, SOTC_POOL_OWNER).
    if (size > 0x1ac82 && image[0x1ac7d] == 0xE8)
        runtime_memory_set_release_site(reinterpret_cast<uintptr_t>(image + 0x1ac82));

    if (cvars.empty()) return;
    std::string text = kCmdlineOriginal;
    for (const std::string& c : cvars) text += " +" + c;
    constexpr uint64_t kTextMax = 16384;
    uint8_t* at = image + (kCmdlineLea - kBase);
    if (text.size() + 1 > kTextMax || cmdline_area_size < kTextMax || at[0] != 0x48 || at[1] != 0x8d || at[2] != 0x35) {
        Appendf(out, "Engine command line: cannot place it, skipped\n");
        return;
    }
    uint8_t* place = cmdline_area + cmdline_area_size - kTextMax;
    std::memcpy(place, text.c_str(), text.size() + 1);
    const int64_t disp = place - (at + 7);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        Appendf(out, "Engine command line: out of reach, skipped\n");
        return;
    }
    const int32_t disp32 = static_cast<int32_t>(disp);
    std::memcpy(at + 3, &disp32, 4);
    Appendf(out, "Engine command line: %s\n", text.c_str());
}

}  // namespace sotc::loader
