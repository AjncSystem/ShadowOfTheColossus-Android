// SPDX-License-Identifier: GPL-2.0-or-later
#include "m2_run.h"

#include <cstdint>
#include <cstring>
#include <random>

#include "game_process.h"

namespace sotc::loader {

namespace {

// M2 has no runtime yet: only the two data imports SotC-PC's runtime.c also serves itself.
class M2Imports final : public ImportResolver {
public:
    M2Imports() {
        std::random_device rd;
        stack_canary_ = (static_cast<uint64_t>(rd()) << 32 | rd()) & ~uint64_t{255};
    }

    uint64_t Function(uint32_t, const std::string&, StubArena&) override { return 0; }

    uint64_t Data(uint32_t, const std::string& nid) override {
        if (Is(nid, "f7uOxY9mM1U")) return reinterpret_cast<uint64_t>(&stack_canary_);    // __stack_chk_guard
        if (Is(nid, "ZT4ODD2Ts9o")) return reinterpret_cast<uint64_t>(&need_libc_internal_);  // SDK marker (Fios2)
        return 0;
    }

private:
    static bool Is(const std::string& scoped, const char* nid) {
        const size_t n = strlen(nid);
        return scoped.compare(0, n, nid) == 0 && (scoped.size() == n || scoped[n] == '#');
    }
    uint64_t stack_canary_ = 0;
    int32_t need_libc_internal_ = 1;
};

}  // namespace

void RunM2(const std::string& boot_path, std::string* report) {
    static M2Imports imports;
    static GameProcess process;  // the guest image stays mapped for the life of the app
    if (!process.Load(boot_path, &imports, report)) {
        *report += "M2 FAILED (load)\n";
        return;
    }
    process.RunMain(report);
    *report += "M2 done\n";
}

}  // namespace sotc::loader
