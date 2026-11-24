// SPDX-License-Identifier: GPL-2.0-or-later
#include "boot_image.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace sotc::loader {

namespace {

constexpr uint64_t kPage = 4096;

struct File {
    FILE* f = nullptr;
    bool ok = true;
    ~File() {
        if (f) fclose(f);
    }
    uint64_t Read64() {
        unsigned char b[8];
        if (fread(b, 1, 8, f) != 8) {
            ok = false;
            return 0;
        }
        uint64_t n = 0;
        for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
        return n;
    }
};

bool Fail(std::string* error, const char* message) {
    *error = message;
    return false;
}

}  // namespace

bool BootImage::Mapped(uint64_t address, uint64_t bytes) const {
    for (const auto& s : segments)
        if (address >= s.address && bytes <= s.size && address - s.address <= s.size - bytes) return true;
    return false;
}

bool BootImage::Executable(uint64_t address) const {
    for (const auto& s : segments)
        if ((s.flags & 1) && address >= s.address && address - s.address < s.size) return true;
    return false;
}

bool BootImage::Parse(const std::string& file, std::string* error) {
    path = file;
    File f;
    f.f = fopen(file.c_str(), "rb");
    if (!f.f) return Fail(error, "cannot open the boot image");
    char magic[8];
    if (fread(magic, 1, 8, f.f) != 8 || memcmp(magic, "BBPROBE5", 8))
        return Fail(error, "not a BBPROBE5 boot image (run SotC-PC's scripts/link_modules.py)");

    size = f.Read64();
    entry = f.Read64();
    const uint64_t ns = f.Read64(), nr = f.Read64(), import_count = f.Read64();
    capabilities = f.Read64();
    if (capabilities & ~uint64_t{1}) return Fail(error, "unknown runtime capabilities");
    if (!size || size > 512ull * 1024 * 1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        return Fail(error, "boot file limits exceeded");

    procparam = f.Read64();
    main_tls = {f.Read64(), f.Read64(), f.Read64(), f.Read64()};
    const uint64_t module_count = f.Read64();
    if (!module_count || module_count > 16) return Fail(error, "invalid module count");
    modules.resize(module_count);
    for (auto& m : modules) m = {f.Read64(), f.Read64(), f.Read64(), f.Read64(), f.Read64(), f.Read64(), f.Read64()};
    const uint64_t nb = f.Read64();
    if (!f.ok) return Fail(error, "truncated boot image header");

    if (main_tls.filesz > main_tls.memsz || main_tls.memsz > 1024 * 1024 || main_tls.vaddr > size ||
        main_tls.filesz > size - main_tls.vaddr || (main_tls.align & (main_tls.align - 1)) || main_tls.align > 4096)
        return Fail(error, "invalid eboot TLS metadata");
    for (const auto& x : modules) {
        if (x.base >= size || !x.size || x.size > size - x.base || x.init < x.base || x.init - x.base >= x.size ||
            (x.tls_module && (x.tls_address >= size || x.tls_memsz > size - x.tls_address || x.tls_memsz > 1024 * 1024 ||
                              x.tls_filesz > x.tls_memsz || x.tls_module < 2 || x.tls_module > 7)))
            return Fail(error, "invalid linked module metadata");
    }
    if (nb > import_count) return Fail(error, "invalid binding count");
    binding_address.assign(import_count, 0);
    binding_kind.assign(import_count, 0);
    for (uint64_t i = 0; i < nb; ++i) {
        const uint64_t index = f.Read64(), address = f.Read64(), kind = f.Read64();
        bool inside = false;
        for (const auto& m : modules)
            if (address >= m.base && address - m.base < m.size) inside = true;
        if (index >= import_count || !inside || (kind != 1 && kind != 2) || binding_address[index])
            return Fail(error, "invalid native binding");
        binding_address[index] = address;
        binding_kind[index] = static_cast<uint8_t>(kind);
    }

    segments.resize(ns);
    for (auto& s : segments) {
        s = {f.Read64(), f.Read64(), f.Read64()};
        if (s.address > size || s.size > size - s.address || s.address % kPage || s.flags > 7)
            return Fail(error, "bad segment");
    }

    names.resize(import_count);
    for (auto& n : names) {
        char buf[128];
        if (fread(buf, 1, sizeof(buf), f.f) != sizeof(buf)) return Fail(error, "truncated import names");
        if (!memchr(buf, 0, sizeof(buf))) return Fail(error, "unterminated import name");
        n = buf;
    }

    relocs.resize(nr);
    for (auto& r : relocs) {
        r = {f.Read64(), f.Read64(), f.Read64(), f.Read64()};
        if (!Mapped(r.target, 8) || r.kind > 2 || (r.kind && r.value >= import_count) || (r.kind != 2 && r.addend) ||
            r.addend >= kPage)
            return Fail(error, "bad relocation");
    }
    if (!f.ok) return Fail(error, "truncated boot image tables");

    for (const auto& m : modules) {
        if (!Mapped(m.init, 1) || (m.tls_module && !Mapped(m.tls_address, m.tls_memsz)))
            return Fail(error, "unmapped module metadata");
        if (!Executable(m.init)) return Fail(error, "module init is not executable");
    }
    if (!Mapped(procparam, 64)) return Fail(error, "unmapped procparam");
    if (!Executable(entry)) return Fail(error, "entry is not executable");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!binding_address[i]) continue;
        if (!Mapped(binding_address[i], 1)) return Fail(error, "unmapped native export");
        if (binding_kind[i] == 1 && !Executable(binding_address[i])) return Fail(error, "native function is not executable");
    }

    image_file_offset = static_cast<uint64_t>(ftell(f.f));
    if (fseek(f.f, 0, SEEK_END) != 0) return Fail(error, "cannot size the boot image");
    if (static_cast<uint64_t>(ftell(f.f)) != image_file_offset + size) return Fail(error, "incorrect memory image size");
    return true;
}

}  // namespace sotc::loader
