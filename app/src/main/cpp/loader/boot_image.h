// SPDX-License-Identifier: GPL-2.0-or-later
// Reader for the prepared boot image (BBPROBE5) written by SotC-PC's scripts/link_modules.py:
// the eboot linked with the game's own sce_module PRXs into one memory image, with one shared
// import table. Same layout and checks as SotC-PC src/probe.c.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sotc::loader {

struct Segment {
    uint64_t address, size, flags;  // flags: 1 = execute, 2 = write, 4 = read
};

// kind 0: image-relative pointer (image + value); kind 1: function import `value`;
// kind 2: data import `value` (+ addend).
struct Reloc {
    uint64_t target, kind, value, addend;
};

struct LinkedModule {
    uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module;
};

struct MainTls {
    uint64_t vaddr, filesz, memsz, align;
};

struct BootImage {
    uint64_t size = 0, entry = 0, capabilities = 0, procparam = 0;
    MainTls main_tls{};
    std::vector<LinkedModule> modules;
    std::vector<uint64_t> binding_address;  // per import: native export in the image, 0 = none
    std::vector<uint8_t> binding_kind;      // 1 = function, 2 = data
    std::vector<Segment> segments;
    std::vector<std::string> names;         // scoped NIDs, e.g. "Ou3iL1abvng#0#F"
    std::vector<Reloc> relocs;
    std::string path;
    uint64_t image_file_offset = 0;

    // Reads and validates everything but the image bytes (read later straight into place).
    bool Parse(const std::string& file, std::string* error);
    bool Mapped(uint64_t address, uint64_t bytes) const;
    bool Executable(uint64_t address) const;
};

}  // namespace sotc::loader
