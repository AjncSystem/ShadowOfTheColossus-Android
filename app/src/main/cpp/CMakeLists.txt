# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(sotc_android C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SOTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(EXT "${SOTC_ROOT}/externals")

# SDL3: window/surface, gamepad, audio and the Java <-> native main thread.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXT}/SDL" "${CMAKE_BINARY_DIR}/SDL")

# libadrenotools: loads a custom Vulkan driver (Turnip) from app storage. Its hook libraries
# (main_hook, file_redirect_hook, gsl_alloc_hook, hook_impl) must ship in the APK.
add_subdirectory("${EXT}/libadrenotools" "${CMAKE_BINARY_DIR}/adrenotools")

# FEXCore: x86-64 -> ARM64 JIT for the game's code.
include("${SOTC_ROOT}/cmake/FexExternal.cmake")

# The GPU library from SotC-PC (gpu/, M4), built for ARM64.
include("${CMAKE_CURRENT_SOURCE_DIR}/gpu/android.cmake")

# The PS4 runtime from SotC-PC (runtime/, see runtime/VENDOR.txt), built for ARM64.
add_library(atrac9 STATIC
    ${EXT}/third_party/LibAtrac9/C/src/band_extension.c ${EXT}/third_party/LibAtrac9/C/src/bit_allocation.c
    ${EXT}/third_party/LibAtrac9/C/src/bit_reader.c ${EXT}/third_party/LibAtrac9/C/src/decinit.c
    ${EXT}/third_party/LibAtrac9/C/src/decoder.c ${EXT}/third_party/LibAtrac9/C/src/huffCodes.c
    ${EXT}/third_party/LibAtrac9/C/src/imdct.c ${EXT}/third_party/LibAtrac9/C/src/libatrac9.c
    ${EXT}/third_party/LibAtrac9/C/src/quantization.c ${EXT}/third_party/LibAtrac9/C/src/scale_factors.c
    ${EXT}/third_party/LibAtrac9/C/src/tables.c ${EXT}/third_party/LibAtrac9/C/src/unpack.c
    ${EXT}/third_party/LibAtrac9/C/src/utility.c)
target_compile_options(atrac9 PRIVATE -O2 -w)

set(SOTC_RUNTIME_SOURCES
    runtime/runtime.c runtime/runtime_ajm.c runtime/runtime_audio.c runtime/runtime_content.c
    runtime/runtime_event.c runtime/runtime_file.c runtime/runtime_kernel.c
    runtime/runtime_memory.c runtime/runtime_mutex.c runtime/runtime_ngs2.c runtime/runtime_pad.c
    runtime/runtime_rtc.c runtime/runtime_rwlock.c runtime/runtime_savedata.c runtime/runtime_sema.c
    runtime/runtime_services.c runtime/runtime_sotc.c runtime/runtime_thread.c runtime/runtime_time.c
    runtime/android_stubs.c)
add_library(sotc_runtime STATIC ${SOTC_RUNTIME_SOURCES})
set_target_properties(sotc_runtime PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(sotc_runtime PRIVATE SOTC_ANDROID=1 _GNU_SOURCE)
# The runtime includes "third_party/LibAtrac9/..." relative to the SotC-PC root.
target_include_directories(sotc_runtime PRIVATE runtime "${EXT}")
target_compile_options(sotc_runtime PRIVATE -O2 -Wall -Wno-unused-function -Wno-unused-variable)
target_link_libraries(sotc_runtime PRIVATE SDL3::SDL3 atrac9 log)

add_library(main SHARED
    main.cpp
    gpu_driver.cpp
    gpu_driver.h
    vk_smoke.cpp
    vk_smoke.h
    log.h
    guest/guest_cpu.cpp
    guest/guest_cpu.h
    guest/m1_selftest.cpp
    guest/m1_selftest.h
    guest/x86_asm.h
    loader/boot_image.cpp
    loader/boot_image.h
    loader/game_process.cpp
    loader/game_process.h
    loader/m2_run.cpp
    loader/m2_run.h
    loader/m3_run.cpp
    loader/m3_run.h
    loader/fibers.cpp
    loader/fibers.h
    loader/sotc_modes.cpp
    loader/sotc_modes.h
    guest/host_bridge.S
    guest/sotc_guest.h
)
enable_language(ASM)
target_compile_definitions(main PRIVATE VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(main PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# The runtime and the GPU library call each other (event queues, memory, process time).
target_link_libraries(main PRIVATE SDL3::SDL3 adrenotools fexcore
    -Wl,--start-group sotc_runtime bbgpu -Wl,--end-group android log dl)

# Speed in every APK, debug ones included (the GPU command threads are the frame's critical path
# on the Odin 3): -O2 for all of libmain.so, ThinLTO across the runtime, the GPU library and the
# loader (inlining across files: the profile is flat), and no PLT for libmain.so's own calls
# (@plt was ~2% of the draw thread): -fno-semantic-interposition + -Bsymbolic-functions.
# SOTC_LTO=OFF builds without LTO (faster builds while debugging).
option(SOTC_LTO "ThinLTO for libmain.so" ON)
foreach(target main bbgpu sotc_runtime)
    target_compile_options(${target} PRIVATE -O2 -fno-semantic-interposition)
    if(SOTC_LTO)
        target_compile_options(${target} PRIVATE -flto=thin)
    endif()
endforeach()
target_link_options(main PRIVATE -Wl,-Bsymbolic-functions -O2)
if(SOTC_LTO)
    target_link_options(main PRIVATE -flto=thin -Wl,--thinlto-jobs=all)
endif()
add_dependencies(main main_hook file_redirect_hook gsl_alloc_hook hook_impl)
