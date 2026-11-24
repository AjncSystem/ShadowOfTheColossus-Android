// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu_driver.h"

#include <dlfcn.h>

#include <adrenotools/driver.h>

#include "log.h"

// Read by the GPU library's vk_platform.cpp (sotc-android) to create the game's Vulkan instance
// on the same driver.
extern "C" PFN_vkGetInstanceProcAddr sotc_android_vk_gipa = nullptr;

PFN_vkGetInstanceProcAddr LoadVulkanDriver(const GpuDriverConfig& cfg, std::string* used) {
    void* handle = nullptr;

    if (!cfg.driver_lib.empty() && !cfg.driver_dir.empty()) {
        std::string hook_dir = cfg.native_lib_dir;
        if (hook_dir.empty() || hook_dir.back() != '/') hook_dir += '/';
        handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                            nullptr, hook_dir.c_str(), cfg.driver_dir.c_str(),
                                            cfg.driver_lib.c_str(), nullptr, nullptr);
        if (handle) {
            *used = "custom: " + cfg.driver_name + " [" + cfg.driver_dir + cfg.driver_lib + "]";
        } else {
            const char* err = dlerror();
            LOGE("adrenotools could not load %s%s: %s", cfg.driver_dir.c_str(),
                 cfg.driver_lib.c_str(), err ? err : "(no dlerror)");
        }
    }

    if (!handle) {
        handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            LOGE("dlopen(libvulkan.so) failed: %s", dlerror());
            return nullptr;
        }
        *used = "system libvulkan.so";
    }

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
    if (!gipa) LOGE("vkGetInstanceProcAddr not exported by the loaded driver");
    sotc_android_vk_gipa = gipa;
    return gipa;
}
