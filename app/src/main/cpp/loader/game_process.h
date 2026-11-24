// SPDX-License-Identifier: GPL-2.0-or-later
// The game process: maps the prepared boot image like SotC-PC's probe.c does, binds every import
// (host function through an HLE stub, native export of a linked PRX, or a stop stub), builds the
// guest main thread's TCB/TLS and runs the module initializers and the eboot entry point on FEX.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "boot_image.h"

namespace sotc::guest {
class GuestThread;
}

namespace sotc::loader {

// Guest-callable HLE stubs, 16 bytes each, in one executable-by-FEX arena.
class StubArena {
public:
    bool Init(size_t capacity);
    uint64_t Make(uint32_t hle_index, bool halt = false);  // returns the stub's guest address

private:
    uint8_t* base_ = nullptr;
    size_t capacity_ = 0, used_ = 0;
};

// Supplies host implementations for imports. Return 0 to fall back to the linked PRX's own
// export (if any), and then to a stop stub.
class ImportResolver {
public:
    virtual ~ImportResolver() = default;
    // Called once the boot image header is read, before any import is resolved.
    virtual void Prepare(const BootImage&) {}
    // Called when the image bytes are in place, before imports are bound (SotC-PC's probe.c
    // initializes the GPU library here, so its symbols are known when imports resolve).
    virtual bool ImageLoaded(const class GameProcess&, std::string*) { return true; }
    virtual uint64_t Function(uint32_t index, const std::string& nid, StubArena& stubs) = 0;
    virtual uint64_t Data(uint32_t index, const std::string& nid) = 0;
    // Gives the calling host thread its guest context (TCB/TLS) for the main thread. Return
    // false to let the loader build a minimal one.
    virtual bool AttachMainThread(const class GameProcess&) { return false; }
};

// Symbol name for a scoped NID ("Ou3iL1abvng#0#F" -> "__stack_chk_fail"), or "?".
const char* ImportSymbol(const std::string& scoped);

class GameProcess {
public:
    GameProcess();
    ~GameProcess();

    bool Load(const std::string& boot_path, ImportResolver* resolver, std::string* log);
    ImportResolver* Resolver() const { return resolver_; }

    // Starts the guest main thread on the calling host thread: module initializers in link order,
    // then the entry point. Returns when the guest halts (stop stub, exit, or a failure).
    void RunMain(std::string* log);

    uint8_t* Image() const { return image_; }
    const BootImage& Boot() const { return boot_; }

    // Set by a stop stub: which import stopped the guest.
    static void NoteStop(uint32_t import_index, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t ret_addr);

private:
    bool ReserveLowRange(std::string* log);
    bool BindImports(ImportResolver* resolver, std::string* log);
    void Protect();
    uint64_t MakeTcb(uint64_t thread_handle);

    BootImage boot_;
    uint8_t* reservation_ = nullptr;
    uint8_t* image_ = nullptr;
    StubArena stubs_;
    std::vector<uint64_t> function_targets_, data_targets_;
    std::vector<void*> data_traps_;
    ImportResolver* resolver_ = nullptr;
};

}  // namespace sotc::loader
