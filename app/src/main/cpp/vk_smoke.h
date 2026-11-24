// SPDX-License-Identifier: GPL-2.0-or-later
// M0 Vulkan smoke test: instance/device on the loaded driver, a report of what the shadPS4
// video core will need, and a swapchain that clears to a color every frame.
#pragma once

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct ANativeWindow;

class VkSmoke {
public:
    bool Init(PFN_vkGetInstanceProcAddr gipa);
    std::string Report() const { return report_; }

    bool CreateSurface(ANativeWindow* window);
    void DestroySurface();
    bool HasSurface() const { return surface_ != VK_NULL_HANDLE; }

    // Returns false when the swapchain must be recreated or the surface is gone.
    bool DrawFrame(float r, float g, float b);
    bool RecreateSwapchain();

    void Shutdown();

private:
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    void BuildReport();

    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image

    static constexpr int kFrames = 2;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_[kFrames]{};
    VkFence fence_[kFrames]{};
    VkSemaphore acquired_[kFrames]{};
    int frame_ = 0;

    std::string report_;

#define SOTC_VK_FN(name) PFN_##name name = nullptr;
#include "vk_functions.inc"
#undef SOTC_VK_FN
};
