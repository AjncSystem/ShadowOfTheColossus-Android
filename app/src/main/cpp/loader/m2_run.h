// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace sotc::loader {

// M2: loads the prepared boot image (SotC-PC out/boot-linked.bin) and runs the module
// initializers and the eboot until the first import that has no implementation yet.
// Blocks the calling thread; the result is appended to `report`.
void RunM2(const std::string& boot_path, std::string* report);

}  // namespace sotc::loader
