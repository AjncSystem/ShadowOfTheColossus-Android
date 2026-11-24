// SPDX-License-Identifier: GPL-2.0-or-later
// sotc-android: stand-ins for the parts of the GPU library left out of the Android build
// (see docs/PLANO.md, "M4"): DLSS frame generation / super resolution (NVIDIA only), FSR 3
// frame generation, and the BPE heap guard that SotC-PC's probe.c installs with an int3 on x86.
#include "video_core/renderer_vulkan/vk_dlssg.h"
#include "video_core/renderer_vulkan/vk_fsrfg.h"

extern "C" uintptr_t runtime_sotc_heap(void) {
    return 0;
}
extern "C" uintptr_t runtime_sotc_heap_table(void) {
    return 0;
}

namespace Core::Loader {
class SymbolsResolver;
}
namespace Libraries::AvPlayer {
// No FFmpeg yet: the AvPlayer symbols are not registered, so its imports stop the guest.
void RegisterLib(Core::Loader::SymbolsResolver*) {}
} // namespace Libraries::AvPlayer

namespace Vulkan::Dlssg {
PFN_vkGetInstanceProcAddr LoadInterposer() {
    return nullptr;
}
void OnDeviceCreated(const Instance&) {}
bool CapturingInputs() {
    return false;
}
bool CameraLogOnly() {
    return false;
}
void NoteCamera(const AmdGpu::Regs&, u32) {}
void NoteDraw(const AmdGpu::Regs&, u32, VideoCore::ImageId, VideoCore::ImageId,
              VideoCore::TextureCache&, Runtime&, Scheduler&) {}
bool SuperResolution() {
    return false;
}
bool SuperResolutionUpscaling() {
    return false;
}
void NoteHdrCurve(float) {}
void NoteVignette(const float*) {}
void NoteComposite(VideoCore::ImageId, VideoCore::ImageId, VideoCore::TextureCache&) {}
bool UiTarget(VAddr, u32, u32, u32&, u32&) {
    return false;
}
void NoteUiShadow(vk::Image, u32, u32) {}
void AfterPresent(u32) {}
bool Enabled() {
    return false;
}
bool WantsImmediatePresent() {
    return false;
}
void PrepareInputs(u32, vk::CommandBuffer, VideoCore::TextureCache&, Runtime&, u32, u32) {}
void NoteHudless(u32, vk::Image, vk::ImageView, vk::Format, u32, u32) {}
bool SuperResolutionAvailable() {
    return false;
}
void SetHdr(bool) {}
float HdrCurve() {
    return 0.0f;
}
bool CompareMode() {
    return false;
}
const char* ModeName() {
    return "off";
}
void ToggleSuperResolution() {}
bool Vignette(float*) {
    return false;
}
Upscaled Upscale(u32, Scheduler&, VideoCore::TextureCache&, Runtime&, u32, u32) {
    return {};
}
void FrameSubmitted(u32) {}
void NoInputs(u32) {}
void BeforePresent(u32, vk::Rect2D, vk::Extent2D) {}
void Suspend() {}
} // namespace Vulkan::Dlssg

namespace Vulkan::FsrFg {
void Init(const Instance&) {}
bool Supported() {
    return false;
}
void ConvertDepth(u32, vk::CommandBuffer, vk::ImageView, u32, u32, bool) {}
void NoDepth(u32) {}
void NoteGameFrame() {}
vk::Image Generate(vk::CommandBuffer, u32, vk::Image, u32, u32, vk::Image) {
    return {};
}
void Submitted(Scheduler&, u64) {}
void WaitForGameFrame() {}
void NotePresent() {}
} // namespace Vulkan::FsrFg
