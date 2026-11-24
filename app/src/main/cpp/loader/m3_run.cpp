// SPDX-License-Identifier: GPL-2.0-or-later
// M3: the game on the SotC-PC runtime (runtime/), without video. The Android counterpart of
// SotC-PC src/probe.c main(): configure content, files, saves and TLS from the boot image,
// bind every import the runtime implements through the generic HLE bridge, then run the module
// initializers and the eboot until the first import nothing implements yet.
#include "m3_run.h"

#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "../guest/guest_cpu.h"
#include "../log.h"
#include "fibers.h"
#include "game_process.h"

extern "C" {
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char* name, int is_data);
void runtime_content_configure(const uint32_t values[5]);
void runtime_content_title(const char* id);
void runtime_file_configure(const char* app0, const char* user);
void runtime_savedata_configure(const char* title);
void runtime_set_module_tls(uint64_t module, const void* data, uint64_t filesz, uint64_t memsz);
void runtime_set_main_tls(const void* data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
void runtime_set_procparam(void* param);
void runtime_report(void);
}
#include "../gpu/bbgpu.h"

namespace sotc::loader {

namespace {

// Same reader as SotC-PC probe.c sfo_value().
bool SfoValue(const std::string& path, const char* key, char* text, size_t text_size) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    static unsigned char data[65536];
    const size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    if (n < 20 || memcmp(data, "\0PSF", 4)) return false;
    uint32_t keys, values, count;
    memcpy(&keys, data + 8, 4);
    memcpy(&values, data + 12, 4);
    memcpy(&count, data + 16, 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= n; ++i) {
        const unsigned char* e = data + 20 + i * 16;
        uint16_t key_offset;
        uint32_t length, offset;
        memcpy(&key_offset, e, 2);
        memcpy(&length, e + 4, 4);
        memcpy(&offset, e + 12, 4);
        if (keys + key_offset >= n || values + offset + length > n ||
            strcmp(reinterpret_cast<const char*>(data) + keys + key_offset, key))
            continue;
        const size_t copy = length < text_size - 1 ? length : text_size - 1;
        memcpy(text, data + values + offset, copy);
        text[copy] = 0;
        return true;
    }
    return false;
}

bool NidIs(const std::string& scoped, const char* nid) {
    const size_t n = strlen(nid);
    return scoped.compare(0, n, nid) == 0 && (scoped.size() == n || scoped[n] == '#');
}

class RuntimeImports final : public ImportResolver {
public:
    // runtime_resolve only answers once runtime_start knows the image links the native libc
    // (capability bit 0), as probe.c does right after reading the header.
    void Prepare(const BootImage& b) override { runtime_start(b.capabilities); }

    uint64_t Function(uint32_t, const std::string& nid, StubArena& stubs) override {
        // Fibers switch guest stacks inside their HLE handlers (fibers.cpp).
        if (const uint64_t fiber = FiberImport(nid, stubs)) return fiber;
        const uintptr_t fn = runtime_resolve(nid.c_str(), 0);
        if (!fn) return 0;
        // pthread_exit / scePthreadExit return to their stub, which halts the guest thread.
        const bool halt = NidIs(nid, "3kg7rT0NQIs") || NidIs(nid, "FJrT5LuUBAU");
        auto& cpu = guest::GuestCpu::Get();
        const char* name = names_.insert(nid).first->c_str();  // stable storage for the HLE table
        return stubs.Make(cpu.RegisterHostFunction(name, reinterpret_cast<const void*>(fn)), halt);
    }

    uint64_t Data(uint32_t, const std::string& nid) override {
        return runtime_resolve(nid.c_str(), 1);
    }

    // M4: window, Vulkan device (Turnip), presenter and GPU command processor, between reading
    // the image and binding the imports, as probe.c does.
    bool ImageLoaded(const GameProcess& p, std::string* log) override {
        char title[128] = "Shadow of the Colossus";
        SfoValue(sfo_path, "TITLE", title, sizeof(title));
        const uint32_t attributes = 0;  // param.sfo ATTRIBUTE (an integer): not read yet
        uint64_t sdk = 0;
        std::memcpy(&sdk, p.Image() + p.Boot().procparam + 16, 8);  // procparam: size, magic, count, sdk
        BbGpuConfig gpu{title, serial.c_str(), user_dir.c_str(), static_cast<uint32_t>(sdk), attributes, 1920, 1080};
        if (bbgpu_init(&gpu) != 0) {
            *log += "GPU initialization FAILED\n";
            return false;
        }
        guest::GuestCpu::SetFaultHook(bbgpu_handle_fault);
        char line[160];
        snprintf(line, sizeof(line), "GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",
                 static_cast<unsigned>(sdk), bbgpu_symbol_count());
        *log += line;
        LOGI("%s", line);
        return true;
    }

    std::string sfo_path, serial = "CUSA08809", user_dir;

    bool AttachMainThread(const GameProcess& p) override {
        const BootImage& b = p.Boot();
        uint8_t* image = p.Image();
        for (const auto& m : b.modules)
            if (m.tls_module) runtime_set_module_tls(m.tls_module, image + m.tls_address, m.tls_filesz, m.tls_memsz);
        runtime_set_main_tls(image + b.main_tls.vaddr, b.main_tls.filesz, b.main_tls.memsz, b.main_tls.align);
        runtime_thread_attach_main();  // TCB/TLS + guest context for this host thread
        runtime_set_procparam(image + b.procparam);
        return true;
    }

private:
    std::set<std::string> names_;
};

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

}  // namespace

void RunM3(const M3Paths& paths, std::string* report) {
    static RuntimeImports imports;
    static GameProcess process;

    // Kernel event queues live in the GPU library; registered first, as probe.c does.
    bbgpu_register_kernel();

    // Content profile (BBCONT01 + 5 values) from SotC-PC's content_profile.py.
    if (FILE* f = fopen(paths.content_profile.c_str(), "rb")) {
        unsigned char data[28];
        const bool ok = fread(data, 1, sizeof(data), f) == sizeof(data) && !memcmp(data, "BBCONT01", 8);
        fclose(f);
        if (ok) {
            uint32_t values[5];
            for (int i = 0; i < 5; ++i) memcpy(&values[i], data + 8 + i * 4, 4);
            runtime_content_configure(values);
            Appendf(*report, "content profile loaded\n");
        }
    }
    mkdir(paths.user_dir.c_str(), 0700);
    runtime_file_configure(paths.app0.c_str(), paths.user_dir.c_str());
    const std::string sfo = paths.app0 + "/sce_sys/param.sfo";
    char id[16] = "", title_id[16] = "";
    if (SfoValue(sfo, "INSTALL_DIR_SAVEDATA", id, sizeof(id)) || SfoValue(sfo, "TITLE_ID", id, sizeof(id)))
        runtime_savedata_configure(id);
    if (SfoValue(sfo, "TITLE_ID", title_id, sizeof(title_id))) runtime_content_title(title_id);
    Appendf(*report, "app0 %s (title %s), user %s\n", paths.app0.c_str(), title_id[0] ? title_id : "?", paths.user_dir.c_str());
    imports.sfo_path = sfo;
    if (title_id[0]) imports.serial = title_id;
    imports.user_dir = paths.user_dir;

    if (!process.Load(paths.boot_image, &imports, report)) {
        *report += "M3 FAILED (load)\n";
        return;
    }

    process.RunMain(report);
    runtime_report();
    *report += "M3 done\n";
}

}  // namespace sotc::loader
