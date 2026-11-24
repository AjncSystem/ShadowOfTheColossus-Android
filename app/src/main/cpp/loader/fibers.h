// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace sotc::loader {

class StubArena;

// libSceFiber on FEX. Returns the guest address of the import's HLE stub, or 0 when `nid` is
// not a fiber function. See fibers.cpp.
uint64_t FiberImport(const std::string& nid, StubArena& stubs);

}  // namespace sotc::loader
