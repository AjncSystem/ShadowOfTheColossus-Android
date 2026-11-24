# SPDX-License-Identifier: GPL-2.0-or-later
# sotc-android: the GPU library (shadPS4 video core + shader recompiler + Gnm/VideoOut + shim)
# built as a static library for Android ARM64. Mirrors SotC-PC gpu/CMakeLists.txt without the
# Windows/Linux-desktop parts and the features left out for now (see docs/PLANO.md, "M4").
# Included from app/src/main/cpp/CMakeLists.txt, which defines EXT and SOTC_ROOT.

set(GPU_DIR ${CMAKE_CURRENT_LIST_DIR})
# Header-only dependencies, at the versions SotC-PC builds with (MSYS2 CLANG64 packages).
set(GPU_HEADER_DEPS
    ${EXT}/Vulkan-Headers/include            # v1.4.363 (vulkan.hpp)
    ${EXT}/VulkanMemoryAllocator/include     # v3.4.0
    ${EXT}/robin-map/include)                # v1.4.1

# ---- third-party ----
set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${GPU_DIR}/third_party/sirit ${CMAKE_BINARY_DIR}/sirit EXCLUDE_FROM_ALL)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(${GPU_DIR}/third_party/miniz ${CMAKE_BINARY_DIR}/miniz EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/zydis ${CMAKE_BINARY_DIR}/zydis EXCLUDE_FROM_ALL)

# ---- host shaders -> headers (glslangValidator on the build machine) ----
find_program(GLSLANG_COMPILER NAMES glslangValidator glslang
    HINTS "$ENV{SOTC_GLSLANG_DIR}" "I:/msys64/clang64/bin" "C:/msys64/clang64/bin" REQUIRED NO_CMAKE_FIND_ROOT_PATH)
set(SHADER_SRC ${GPU_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_BINARY_DIR}/gpu-include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(shadow_scroll.frag shadow_scroll_frag)
add_host_shader(fg_depth.comp fg_depth_comp)
add_host_shader(fg_ui.comp fg_ui_comp)
add_host_shader(dlss_mvec_fill.comp dlss_mvec_fill_comp)
add_host_shader(dlss_color_sanitize.comp dlss_color_sanitize_comp)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

# ---- sources ----
file(GLOB_RECURSE GPU_VIDEO_CORE CONFIGURE_DEPENDS ${GPU_DIR}/shadps4/video_core/*.cpp)
file(GLOB_RECURSE GPU_RECOMPILER CONFIGURE_DEPENDS ${GPU_DIR}/shadps4/shader_recompiler/*.cpp)
# Left out on Android for now: RenderDoc, DLSS (NVIDIA Streamline), FSR3 frame generation, FSR4.
list(FILTER GPU_VIDEO_CORE EXCLUDE REGEX "(renderdoc|vk_dlssg|vk_fsrfg|vk_fsr4)\\.cpp$")
set(GPU_COMMON
    ${GPU_DIR}/shadps4/common/decoder.cpp ${GPU_DIR}/shadps4/common/error.cpp
    ${GPU_DIR}/shadps4/common/io_file.cpp ${GPU_DIR}/shadps4/common/native_clock.cpp
    ${GPU_DIR}/shadps4/common/number_utils.cpp ${GPU_DIR}/shadps4/common/path_util.cpp
    ${GPU_DIR}/shadps4/common/rdtsc.cpp ${GPU_DIR}/shadps4/common/spin_lock.cpp
    ${GPU_DIR}/shadps4/common/recursive_lock.cpp ${GPU_DIR}/shadps4/common/string_util.cpp
    ${GPU_DIR}/shadps4/common/thread.cpp ${GPU_DIR}/shadps4/common/signal_context.cpp
    ${GPU_DIR}/shim/common/logging/log.cpp)
file(GLOB GPU_SHIM CONFIGURE_DEPENDS ${GPU_DIR}/shim/*.cpp ${GPU_DIR}/shim/*/*.cpp)
# The leak-hunting allocator wrapper is a Windows-only diagnostic.
list(FILTER GPU_SHIM EXCLUDE REGEX "bbport_heap_track\\.cpp$")
set(GPU_SOTC ${GPU_DIR}/sotc/dwarf.cpp ${GPU_DIR}/sotc/sotc_patches.cpp ${GPU_DIR}/android_stubs.cpp)

add_library(bbgpu STATIC
    ${GPU_VIDEO_CORE} ${GPU_RECOMPILER} ${GPU_COMMON} ${GPU_SHIM} ${GPU_SOTC}
    ${GPU_DIR}/shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    ${GPU_DIR}/shadps4/core/libraries/videoout/driver.cpp
    ${GPU_DIR}/shadps4/core/libraries/videoout/video_out.cpp
    ${GPU_DIR}/shadps4/core/libraries/kernel/equeue.cpp
    ${GPU_DIR}/third_party/imgui/imgui.cpp ${GPU_DIR}/third_party/imgui/imgui_draw.cpp
    ${GPU_DIR}/third_party/imgui/imgui_tables.cpp ${GPU_DIR}/third_party/imgui/imgui_widgets.cpp
    ${GPU_DIR}/third_party/imgui/backends/imgui_impl_vulkan.cpp)
add_dependencies(bbgpu host_shaders)
set_target_properties(bbgpu PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(bbgpu PRIVATE
    ${GPU_DIR}/shim ${GPU_DIR}/shadps4 ${SHADER_INCLUDE}
    ${GPU_DIR}/third_party/gcn/include ${GPU_DIR}/third_party/half/include
    ${GPU_DIR}/third_party/imgui ${GPU_DIR}/third_party/imgui/backends ${GPU_DIR}/third_party/stb
    ${GPU_DIR}/third_party/magic_enum/include ${GPU_DIR}/third_party/magic_enum/include/magic_enum
    ${GPU_HEADER_DEPS} ${EXT}/ext-boost
    ${EXT}/FEX/External/fmt/include ${EXT}/FEX/External/xxhash)
target_compile_definitions(bbgpu PRIVATE
    SOTC_ANDROID=1 BOOST_ASIO_STANDALONE NDEBUG IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    VK_USE_PLATFORM_ANDROID_KHR
    BB_FONT_PATH="${GPU_DIR}/third_party/fonts/DejaVuSans.ttf")
# -fexperimental-library: std::jthread/stop_token are still "experimental" in the NDK r28 libc++.
target_compile_options(bbgpu PRIVATE -O2 -fno-omit-frame-pointer -fexperimental-library
    -include ${GPU_DIR}/sotc_android_compat.h
    -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-unused-parameter
    -Wno-nullability-completeness -Wno-format-security)
set_source_files_properties(${GPU_DIR}/shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${GPU_DIR}/third_party/fonts/DejaVuSans.ttf)
target_link_libraries(bbgpu PRIVATE SDL3::SDL3 sirit miniz Zydis fexcore)
