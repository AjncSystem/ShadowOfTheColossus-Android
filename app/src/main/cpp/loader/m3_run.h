// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace sotc::loader {

struct M3Paths {
    std::string boot_image;       // SotC-PC out/boot-linked.bin
    std::string content_profile;  // SotC-PC out/content.bin
    std::string app0;             // the game dump (CUSA08809 or CUSA08034)
    std::string user_dir;         // saves, caches
};

// M3: runs the game's initialization on the SotC-PC runtime (no video yet) until the first
// import nothing implements. Blocks the calling thread; the result is appended to `report`.
void RunM3(const M3Paths& paths, std::string* report);

}  // namespace sotc::loader
