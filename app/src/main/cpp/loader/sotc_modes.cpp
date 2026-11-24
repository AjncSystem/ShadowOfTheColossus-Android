// SPDX-License-Identifier: GPL-2.0-or-later
#include "sotc_modes.h"

#include <algorithm>
#include <cctype>
#include <iterator>
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

// Render size (sotc_modes.py RENDER_SITES, PS4 mode; the PS4 Pro values stay): kind 'W'/'H' an
// integer immediate (the last 4 bytes of the instruction), 'w'/'h' a float, 'w2'/'h2' half of it,
// 'a' the aspect W/H, 'P' the render target pool, 'v' the width of a 1080 line canvas (1080*W/H).
struct RenderSite {
    uint64_t address;
    const char* kind;
    const char* original;
};
const RenderSite kRenderSites[] = {
    {0x00495bec, "H", "b938040000"}, {0x00495bf7, "W", "b880070000"},
    {0x00495c91, "W", "ba80070000"}, {0x00495c96, "H", "b938040000"},
    {0x00495d34, "W", "ba80070000"}, {0x00495d39, "H", "b938040000"},
    {0x00495d61, "W", "b880070000"}, {0x00495d7c, "H", "b938040000"},
    {0x0049b693, "W", "3d80070000"}, {0x0049b6c6, "H", "817c242038040000"},
    {0x0049b74f, "P", "be00002814"},
    {0x004a2ae9, "W", "41be80070000"}, {0x004a2af2, "H", "41bf38040000"},
    {0x0056f2f3, "W", "b880070000"}, {0x0056f313, "H", "b838040000"},
    {0x00662ac6, "W", "c705109ac80280070000"}, {0x00662ad0, "H", "c7050a9ac80238040000"},
    {0x00ab5888, "W", "c7054a2db80280070000"}, {0x00ab5899, "H", "c7053d2db80238040000"},
    {0x00ab5978, "W", "c70780070000"}, {0x00ab597e, "H", "c7470438040000"},
    {0x00ab6001, "W", "c70080070000"}, {0x00ab6007, "H", "c7400438040000"},
    {0x013c34a0, "w", "0000f044"}, {0x013c34a8, "h", "00008744"},
    {0x013d7518, "w", "0000f044"}, {0x013d7520, "h", "00008744"},
    {0x01667f14, "w", "0000f044"}, {0x01667f1c, "h", "00008744"},
    {0x01667f24, "w", "0000f044"}, {0x01667f2c, "h", "00008744"},
    {0x01667f34, "w", "0000f044"}, {0x01667f3c, "h", "00008744"},
    {0x01667f44, "w2", "00007044"}, {0x01667f4c, "w2", "00007044"},
    {0x01667f54, "w", "0000f044"}, {0x01667f5c, "h", "00008744"},
    {0x01667f64, "w2", "00007044"}, {0x01667f6c, "h2", "00000744"},
    {0x01667f74, "w2", "00007044"}, {0x01667f7c, "h2", "00000744"},
    {0x01667f84, "w", "0000f044"}, {0x01667f8c, "h", "00008744"},
    {0x01667f94, "w2", "00007044"}, {0x01667f9c, "h2", "00000744"},
    {0x0166633c, "a", "398ee33f"},
    {0x008e2cf1, "v", "c78550feffff0000f044"},
    {0x00920309, "v", "c78560feffff0000f044"},
};
constexpr uint64_t kRenderTargetPool = 0x14280000;

// SOTC_RENDER=WxH: the game renders at WxH instead of 1920x1080 (16:9 sizes; the presenter's FSR 1
// scales the picture to the screen). PS4 mode only: the PS4 Pro path keeps its own sizes.
size_t ApplyRenderSize(uint8_t* image, uint64_t size, std::string& out) {
    const std::string spec = Env("SOTC_RENDER");
    unsigned w = 0, h = 0;
    if (spec.empty() || sscanf(spec.c_str(), "%ux%u", &w, &h) != 2 || (w == 1920 && h == 1080)) return 0;
    if (w < 640 || h < 360 || w > 7680 || h > 4320 || w % 8 || h % 8) {
        Appendf(out, "Render size %s: 640x360 to 7680x4320, multiples of 8; ignored\n", spec.c_str());
        return 0;
    }
    if (Env("SOTC_NEO") == "1") {
        Appendf(out, "Render size %s: PS4 Pro mode keeps its own sizes; ignored\n", spec.c_str());
        return 0;
    }
    for (const RenderSite& s : kRenderSites) {
        if (!Holds(image, size, s.address, s.original)) {
            Appendf(out, "Render size: unexpected bytes at %#llx, not applied\n",
                    static_cast<unsigned long long>(s.address));
            return 0;
        }
    }
    auto pool = std::max<uint64_t>(kRenderTargetPool, kRenderTargetPool * w * h / (1920 * 1080));
    pool = (pool + 0xffff) & ~uint64_t{0xffff};
    for (const RenderSite& s : kRenderSites) {
        uint8_t data[4];
        const std::string kind = s.kind;
        auto put_u32 = [&](uint32_t v) { std::memcpy(data, &v, 4); };
        auto put_f32 = [&](float v) { std::memcpy(data, &v, 4); };
        if (kind == "W") put_u32(w);
        else if (kind == "H") put_u32(h);
        else if (kind == "P") put_u32(static_cast<uint32_t>(pool));
        else if (kind == "w") put_f32(static_cast<float>(w));
        else if (kind == "h") put_f32(static_cast<float>(h));
        else if (kind == "w2") put_f32(w / 2.0f);
        else if (kind == "h2") put_f32(h / 2.0f);
        else if (kind == "a") put_f32(static_cast<float>(w) / h);
        else put_f32(1080.0f * w / h); // 'v'
        const uint64_t original_len = Hex(s.original).size();
        std::memcpy(image + (s.address - kBase) + original_len - 4, data, 4);
    }
    Appendf(out, "Render size %ux%u: on\n", w, h);
    return std::size(kRenderSites);
}

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
    writes += ApplyRenderSize(image, size, out);

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
