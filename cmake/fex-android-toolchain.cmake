# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the unmodified FEX tree (externals/FEX) for Android/bionic arm64.
# FEX only accepts CMAKE_SYSTEM_NAME Linux, and bionic is Linux, so this file presents the NDK
# clang + sysroot as a plain aarch64 Linux cross toolchain. No FEX source file is edited; bionic
# gaps are covered from our side (cmake/fex-bionic-compat).
#
# Required: environment variable SOTC_NDK (or ANDROID_NDK_HOME) pointing at the NDK.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Passed with -DSOTC_NDK=... by FexExternal.cmake, or from the environment for manual builds.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SOTC_NDK)
if (SOTC_NDK)
  set(_ndk "${SOTC_NDK}")
else()
  set(_ndk "$ENV{SOTC_NDK}")
endif()
if (NOT _ndk)
  set(_ndk "$ENV{ANDROID_NDK_HOME}")
endif()
if (NOT _ndk)
  message(FATAL_ERROR "Set SOTC_NDK to the Android NDK directory")
endif()
file(TO_CMAKE_PATH "${_ndk}" _ndk)

if (CMAKE_HOST_WIN32)
  set(_host windows-x86_64)
  set(_exe .exe)
elseif (CMAKE_HOST_APPLE)
  set(_host darwin-x86_64)
  set(_exe "")
else()
  set(_host linux-x86_64)
  set(_exe "")
endif()
set(_llvm "${_ndk}/toolchains/llvm/prebuilt/${_host}")

set(SOTC_ANDROID_API 31)
set(_target aarch64-linux-android${SOTC_ANDROID_API})

set(CMAKE_C_COMPILER "${_llvm}/bin/clang${_exe}")
set(CMAKE_CXX_COMPILER "${_llvm}/bin/clang++${_exe}")
set(CMAKE_ASM_COMPILER "${_llvm}/bin/clang${_exe}")
set(CMAKE_C_COMPILER_TARGET ${_target})
set(CMAKE_CXX_COMPILER_TARGET ${_target})
set(CMAKE_ASM_COMPILER_TARGET ${_target})
set(CMAKE_AR "${_llvm}/bin/llvm-ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_llvm}/bin/llvm-ranlib${_exe}" CACHE FILEPATH "")
set(CMAKE_STRIP "${_llvm}/bin/llvm-strip${_exe}" CACHE FILEPATH "")
set(CMAKE_SYSROOT "${_llvm}/sysroot")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# FEX is linked into libmain.so, which uses the shared NDK libc++. The prelude adds what the NDK
# libc++ lacks (std::atomic_ref).
get_filename_component(_compat "${CMAKE_CURRENT_LIST_DIR}/fex-bionic-compat" ABSOLUTE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -include \"${_compat}/atomic_ref_polyfill.h\"")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
