# SPDX-License-Identifier: GPL-2.0-or-later
# Builds FEXCore (externals/FEX, pinned revision + patches/fex) as static libraries with
# cmake/fex-android-toolchain.cmake and exposes them as the `fexcore` interface target.
# Always a Release build: the JIT's own speed matters even in debug APKs.
#
# Host tools the FEX build runs: python3 (code generators) and gzip (man page). On Windows, gzip
# comes from Git for Windows (build.ps1 puts it on PATH).
include(ExternalProject)

set(SOTC_FEX_REVISION "f2b679f6028ce1c38875233aecfcf5d3f8ebecec")
set(FEX_SRC "${SOTC_ROOT}/externals/FEX")
set(FEX_BIN "${CMAKE_BINARY_DIR}/fex")

set(_fex_libs
  FEXCore/Source/libFEXCore.a
  FEXCore/Source/libFEXCore_Base.a
  FEXCore/Source/libJemallocLibs.a
  External/rpmalloc/librpmalloc.a
  Source/Common/libCommon.a
  Source/Tools/CommonTools/libCommonTools.a
  Source/Common/cpp-optparse/libcpp-optparse.a
  External/fmt/libfmt.a
  External/xxhash/cmake_unofficial/libxxhash.a
  External/SoftFloat-3e/libsoftfloat_3e.a
  External/cephes/libcephes_128bit.a
  External/tiny-json/libtiny-json.a
)
set(FEX_LIB_PATHS "")
foreach(lib IN LISTS _fex_libs)
  list(APPEND FEX_LIB_PATHS "${FEX_BIN}/${lib}")
endforeach()

ExternalProject_Add(fex_build
  SOURCE_DIR "${FEX_SRC}"
  BINARY_DIR "${FEX_BIN}"
  CMAKE_GENERATOR Ninja
  CMAKE_ARGS
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_TOOLCHAIN_FILE=${SOTC_ROOT}/cmake/fex-android-toolchain.cmake"
    "-DSOTC_NDK=${ANDROID_NDK}"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/data/local/tmp/fex
    -DTUNE_CPU=none
    -DBUILD_TESTING=OFF
    -DBUILD_FEX_LINUX_TESTS=OFF
    -DBUILD_THUNKS=OFF
    -DBUILD_FEXCONFIG=OFF
    -DENABLE_GDB_SYMBOLS=OFF
    -DENABLE_LTO=OFF
    -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
    -DENABLE_OFFLINE_TELEMETRY=OFF
    -DENABLE_VIXL_DISASSEMBLER=OFF
    -DENABLE_VIXL_SIMULATOR=OFF
    -DENABLE_ZYDIS=OFF
    -DENABLE_FEXCORE_PROFILER=OFF
    -DENABLE_CCACHE=OFF
  BUILD_COMMAND "${CMAKE_MAKE_PROGRAM}" -C "${FEX_BIN}" FEXCore JemallocLibs Common CommonTools
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${FEX_LIB_PATHS}
  USES_TERMINAL_BUILD ON
)

# Generated headers live in the build tree; create the folders so the interface target is valid
# before the first FEX build.
file(MAKE_DIRECTORY "${FEX_BIN}/generated" "${FEX_BIN}/include" "${FEX_BIN}/FEXCore/Source" "${FEX_BIN}/Source")

add_library(fexcore INTERFACE)
add_dependencies(fexcore fex_build)
target_include_directories(fexcore INTERFACE
  "${FEX_SRC}/FEXCore/include"
  "${FEX_SRC}/Source"
  "${FEX_SRC}/Source/Common"
  "${FEX_SRC}/Source/Tools/CommonTools"
  "${FEX_SRC}/FEXHeaderUtils"
  "${FEX_SRC}/CodeEmitter"
  "${FEX_SRC}/External/fmt/include"
  "${FEX_SRC}/External/unordered_dense/include"
  "${FEX_SRC}/External/range-v3/include"
  "${FEX_SRC}/External/tiny-json"
  "${FEX_BIN}/generated"
  "${FEX_BIN}/include"
  "${FEX_BIN}/FEXCore/Source"
  "${FEX_BIN}/Source"
)
target_compile_definitions(fexcore INTERFACE
  ARCHITECTURE_arm64=1
  FEXCORE_HAS_PRESERVE_ALL_ATTR=1
  "FEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
  FEX_HAS_PRESERVE_ALL_ATTR=1
  "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
  FEX_DISABLE_TELEMETRY=1
  _LARGEFILE64_SOURCE
  "SOTC_FEX_REVISION=\"${SOTC_FEX_REVISION}\""
)
target_compile_options(fexcore INTERFACE
  "SHELL:-include ${SOTC_ROOT}/cmake/fex-bionic-compat/atomic_ref_polyfill.h")
# The static libraries reference each other; resolve them as a group.
target_link_libraries(fexcore INTERFACE
  "-Wl,--start-group" ${FEX_LIB_PATHS} "-Wl,--end-group")
