// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include <vulkan/vulkan.h>

struct GpuDriverConfig {
    std::string native_lib_dir;  // ApplicationInfo.nativeLibraryDir (adrenotools hooks live here)
    std::string driver_dir;      // app-private folder with the extracted driver, trailing '/'
    std::string driver_lib;      // e.g. libvulkan_freedreno.so; empty = system driver
    std::string driver_name;     // human-readable name from meta.json
};

// Opens libvulkan, through adrenotools when a custom driver is configured, and returns its
// vkGetInstanceProcAddr. Falls back to the system driver if the custom one cannot be loaded.
// `used` describes what was actually loaded.
PFN_vkGetInstanceProcAddr LoadVulkanDriver(const GpuDriverConfig& cfg, std::string* used);
