// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace sotc::guest {

// Initializes the guest CPU (FEXCore) and runs the M1 checks and benchmarks.
// `home` is a writable directory for FEX; the human-readable result is appended to `report`.
bool RunM1SelfTest(const std::string& home, std::string* report);

}  // namespace sotc::guest
