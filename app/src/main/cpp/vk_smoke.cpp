// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_smoke.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <android/native_window.h>

#include "log.h"

namespace {

// Extensions the vendored shadPS4 video core (SotC-PC gpu/shadps4) asks for. Reported so we know
// up front which fallbacks the Android build needs.
const char* const kShadExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    "VK_KHR_maintenance5",
    "VK_KHR_maintenance8",
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    "VK_EXT_attachment_feedback_loop_layout",
    "VK_EXT_attachment_feedback_loop_dynamic_state",
    "VK_EXT_depth_range_unrestricted",
    "VK_EXT_extended_dynamic_state3",
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    "VK_EXT_depth_clip_control",
    VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    "VK_EXT_vertex_input_dynamic_state",
    "VK_EXT_primitive_topology_list_restart",
    "VK_AMD_shader_explicit_vertex_parameter",
    "VK_KHR_fragment_shader_barycentric",
    VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
    VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
    "VK_AMD_shader_image_load_store_lod",
    "VK_AMD_gcn_shader",
    "VK_AMD_shader_trinary_minmax",
    "VK_NV_framebuffer_mixed_samples",
    "VK_AMD_mixed_attachment_samples",
    "VK_EXT_shader_atomic_float",
    "VK_EXT_shader_atomic_float2",
    "VK_KHR_workgroup_memory_explicit_layout",
    "VK_EXT_image_2d_view_of_3d",
    "VK_EXT_image_view_min_lod",
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    "VK_KHR_shader_clock",
    "VK_EXT_calibrated_timestamps",
    "VK_EXT_device_fault",
};

void Appendf(std::string& s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void Appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

std::string VersionString(uint32_t v) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
             VK_API_VERSION_PATCH(v));
    return buf;
}

}  // namespace

bool VkSmoke::Init(PFN_vkGetInstanceProcAddr gipa) {
    gipa_ = gipa;
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa_(nullptr, "vkCreateInstance"));
    vkEnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa_(nullptr, "vkEnumerateInstanceVersion"));
    vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa_(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!vkCreateInstance) {
        LOGE("driver has no vkCreateInstance");
        return false;
    }

    uint32_t instance_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&instance_version);

    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SotC-Android";
    app.applicationVersion = 1;
    app.pEngineName = "bbport";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: %d", r);
        return false;
    }

#define SOTC_VK_FN(name) \
    if (!name) name = reinterpret_cast<PFN_##name>(gipa_(instance_, #name));
#include "vk_functions.inc"
#undef SOTC_VK_FN

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        LOGE("no Vulkan physical devices (adrenotools hook not active?)");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance_, &count, gpus.data());
    gpu_ = gpus[0];

    report_.clear();
    Appendf(report_, "instance version: %s\n", VersionString(instance_version).c_str());
    BuildReport();
    return CreateDevice();
}

void VkSmoke::BuildReport() {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    p11.pNext = &driver;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &p11;
    vkGetPhysicalDeviceProperties2(gpu_, &props);
    const auto& p = props.properties;

    Appendf(report_, "device: %s (vendor 0x%04x, device 0x%08x)\n", p.deviceName, p.vendorID, p.deviceID);
    Appendf(report_, "api: %s, driverVersion: 0x%08x\n", VersionString(p.apiVersion).c_str(), p.driverVersion);
    Appendf(report_, "driver: %s | %s (id %d, conformance %u.%u.%u.%u)\n", driver.driverName,
            driver.driverInfo, driver.driverID, driver.conformanceVersion.major,
            driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
            driver.conformanceVersion.patch);
    Appendf(report_, "subgroup: size %u, stages 0x%x, ops 0x%x\n", p11.subgroupSize,
            p11.subgroupSupportedStages, p11.subgroupSupportedOperations);
    Appendf(report_,
            "limits: pushConstants %u, maxStorageBufferRange %u, minSSBOAlign %" PRIu64
            ", sharedMem %u, maxImageDim2D %u, maxBoundDescriptorSets %u\n",
            p.limits.maxPushConstantsSize, p.limits.maxStorageBufferRange,
            static_cast<uint64_t>(p.limits.minStorageBufferOffsetAlignment),
            p.limits.maxComputeSharedMemorySize, p.limits.maxImageDimension2D,
            p.limits.maxBoundDescriptorSets);

    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
        Appendf(report_, "heap %u: %" PRIu64 " MiB flags 0x%x\n", i,
                static_cast<uint64_t>(mem.memoryHeaps[i].size >> 20), mem.memoryHeaps[i].flags);
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f12.pNext = &f13;
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(gpu_, &f);
    const auto& f10 = f.features;

    struct Item {
        const char* name;
        VkBool32 value;
    };
    const Item features[] = {
        {"robustBufferAccess", f10.robustBufferAccess},
        {"imageCubeArray", f10.imageCubeArray},
        {"independentBlend", f10.independentBlend},
        {"geometryShader", f10.geometryShader},
        {"tessellationShader", f10.tessellationShader},
        {"dualSrcBlend", f10.dualSrcBlend},
        {"logicOp", f10.logicOp},
        {"depthClamp", f10.depthClamp},
        {"depthBiasClamp", f10.depthBiasClamp},
        {"fillModeNonSolid", f10.fillModeNonSolid},
        {"multiViewport", f10.multiViewport},
        {"samplerAnisotropy", f10.samplerAnisotropy},
        {"textureCompressionBC", f10.textureCompressionBC},
        {"shaderStorageImageExtendedFormats", f10.shaderStorageImageExtendedFormats},
        {"shaderStorageImageReadWithoutFormat", f10.shaderStorageImageReadWithoutFormat},
        {"shaderStorageImageWriteWithoutFormat", f10.shaderStorageImageWriteWithoutFormat},
        {"shaderClipDistance", f10.shaderClipDistance},
        {"shaderCullDistance", f10.shaderCullDistance},
        {"shaderFloat64", f10.shaderFloat64},
        {"shaderInt64", f10.shaderInt64},
        {"shaderInt16", f10.shaderInt16},
        {"storageBuffer16BitAccess", f11.storageBuffer16BitAccess},
        {"shaderDrawParameters", f11.shaderDrawParameters},
        {"shaderFloat16", f12.shaderFloat16},
        {"shaderInt8", f12.shaderInt8},
        {"storageBuffer8BitAccess", f12.storageBuffer8BitAccess},
        {"descriptorIndexing", f12.descriptorIndexing},
        {"timelineSemaphore", f12.timelineSemaphore},
        {"bufferDeviceAddress", f12.bufferDeviceAddress},
        {"vulkanMemoryModel", f12.vulkanMemoryModel},
        {"scalarBlockLayout", f12.scalarBlockLayout},
        {"separateDepthStencilLayouts", f12.separateDepthStencilLayouts},
        {"hostQueryReset", f12.hostQueryReset},
        {"samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge},
        {"dynamicRendering", f13.dynamicRendering},
        {"synchronization2", f13.synchronization2},
        {"maintenance4", f13.maintenance4},
        {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
        {"subgroupSizeControl", f13.subgroupSizeControl},
        {"computeFullSubgroups", f13.computeFullSubgroups},
        {"robustImageAccess", f13.robustImageAccess},
        {"pipelineCreationCacheControl", f13.pipelineCreationCacheControl},
    };
    report_ += "features (- = missing):\n";
    for (const auto& it : features) Appendf(report_, "  %c %s\n", it.value ? '+' : '-', it.name);

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &n, exts.data());
    Appendf(report_, "device extensions: %u total; shadPS4 wishlist (- = missing):\n", n);
    for (const char* want : kShadExtensions) {
        bool have = std::any_of(exts.begin(), exts.end(),
                                [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, want) == 0; });
        Appendf(report_, "  %c %s\n", have ? '+' : '-', want);
    }
}

bool VkSmoke::CreateDevice() {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &n, fams.data());
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOGE("no graphics queue family");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = exts;
    VkResult r = vkCreateDevice(gpu_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateDevice failed: %d", r);
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = kFrames;
    vkAllocateCommandBuffers(device_, &cai, cmd_);
    for (int i = 0; i < kFrames; ++i) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fci, nullptr, &fence_[i]);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sci, nullptr, &acquired_[i]);
    }
    return true;
}

bool VkSmoke::CreateSurface(ANativeWindow* window) {
    if (!window) return false;
    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window;
    VkResult r = vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed: %d", r);
        surface_ = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu_, queue_family_, surface_, &present);
    if (!present) {
        LOGE("graphics queue cannot present to this surface");
        return false;
    }
    return CreateSwapchain();
}

bool VkSmoke::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    extent_ = caps.currentExtent;
    if (extent_.width == 0 || extent_.height == 0) return false;

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = f;
            break;
        }
    }
    format_ = chosen.format;

    uint32_t image_count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) image_count = std::min(image_count, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkResult r = vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %d", r);
        swapchain_ = VK_NULL_HANDLE;
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
    render_done_.resize(n);
    for (auto& s : render_done_) {
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sem, nullptr, &s);
    }
    LOGI("swapchain %ux%u, %u images, format %d, transform 0x%x", extent_.width, extent_.height, n,
         format_, caps.currentTransform);
    return true;
}

void VkSmoke::DestroySwapchain() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (auto s : render_done_) vkDestroySemaphore(device_, s, nullptr);
    render_done_.clear();
    images_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkSmoke::RecreateSwapchain() {
    DestroySwapchain();
    return surface_ && CreateSwapchain();
}

void VkSmoke::DestroySurface() {
    DestroySwapchain();
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

bool VkSmoke::DrawFrame(float r, float g, float b) {
    if (!swapchain_) return false;
    const int f = frame_;
    vkWaitForFences(device_, 1, &fence_[f], VK_TRUE, UINT64_MAX);

    uint32_t index = 0;
    VkResult res = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_[f], VK_NULL_HANDLE, &index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        LOGE("vkAcquireNextImageKHR: %d", res);
        return false;
    }
    vkResetFences(device_, 1, &fence_[f]);

    VkCommandBuffer cmd = cmd_[f];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = images_[index];
    to_dst.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_dst);

    VkClearColorValue color{};
    color.float32[0] = r;
    color.float32[1] = g;
    color.float32[2] = b;
    color.float32[3] = 1.0f;
    vkCmdClearColorImage(cmd, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_[f];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_done_[index];
    vkQueueSubmit(queue_, 1, &si, fence_[f]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_done_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    res = vkQueuePresentKHR(queue_, &pi);
    frame_ = (frame_ + 1) % kFrames;
    // SUBOPTIMAL just means a pre-rotation mismatch; keep presenting.
    return res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}

void VkSmoke::Shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        DestroySurface();
        for (int i = 0; i < kFrames; ++i) {
            vkDestroyFence(device_, fence_[i], nullptr);
            vkDestroySemaphore(device_, acquired_[i], nullptr);
        }
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}
