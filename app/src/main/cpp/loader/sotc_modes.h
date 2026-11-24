// SPDX-License-Identifier: GPL-2.0-or-later
// SotC-PC's game modes (scripts/sotc_modes.py + probe.c apply_patches / apply_sotc_cmdline),
// applied to the relocated image in memory instead of through patches.bin / cmdline.bin.
#pragma once

#include <cstdint>
#include <string>

namespace sotc::loader {

// Reads SOTC_MODES (af16, fps60, fps60game, grassbuf, hdr), SOTC_CVARS, SOTC_NEO and
// SOTC_POOL_RESERVE. `image`/`size` is the relocated eboot image; the engine command line goes at
// the end of [cmdline_area, cmdline_area + cmdline_area_size), which must be within 2 GiB of it.
void ApplySotcModes(uint8_t* image, uint64_t size, uint8_t* cmdline_area, uint64_t cmdline_area_size,
                    std::string* log);

}  // namespace sotc::loader
