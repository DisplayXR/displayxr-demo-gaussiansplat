# Copyright 2025, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build (displayxr-demo-gaussiansplat repo). Identical to the
# in-tree CMakeLists.txt since 3dgs_common has no path references that reach
# outside its own directory. Synced by publish-demo-gaussiansplat.yml.

cmake_minimum_required(VERSION 3.21)
project(gs_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

# ── Niantic SPZ format support (FetchContent) ────────────────────────────
include(FetchContent)

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "System zlib not found — fetching via FetchContent")
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
    )
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    set(SPZ_ZLIB_TARGET zlibstatic)
else()
    set(SPZ_ZLIB_TARGET ZLIB::ZLIB)
endif()

# zstd — required since SPZ header version 4: the NGSP container stores each
# attribute as its own ZSTD stream (load-spz.cc: decompressNgspStreams). Built
# from source the same way spz is (SOURCE_SUBDIR _skip + our own add_library),
# so it inherits this directory's CMAKE_MSVC_RUNTIME_LIBRARY (static CRT) and
# needs none of zstd's own build options. Only common/ + compress/ +
# decompress/ are needed; ZSTD_DISABLE_ASM keeps the x86-64 .S out (MSVC
# cannot assemble it, and the C fallback is correct everywhere).
find_package(zstd QUIET)
if(TARGET zstd::libzstd_static)
    # A system zstd is used only when it offers a STATIC target: the app links
    # the static CRT and ships no side-by-side DLLs.
    set(SPZ_ZSTD_TARGET zstd::libzstd_static)
else()
    message(STATUS "Static system zstd not found — fetching via FetchContent")
    FetchContent_Declare(zstd
        URL      https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
        URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
        SOURCE_SUBDIR _skip
    )
    FetchContent_MakeAvailable(zstd)
    file(GLOB _ZSTD_SRCS
        "${zstd_SOURCE_DIR}/lib/common/*.c"
        "${zstd_SOURCE_DIR}/lib/compress/*.c"
        "${zstd_SOURCE_DIR}/lib/decompress/*.c"
    )
    add_library(zstd_lib STATIC ${_ZSTD_SRCS})
    target_include_directories(zstd_lib PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(zstd_lib PRIVATE ZSTD_DISABLE_ASM=1 XXH_NAMESPACE=ZSTD_)
    if(MSVC)
        set_target_properties(zstd_lib PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    endif()
    set(SPZ_ZSTD_TARGET zstd_lib)
endif()

# Pinned to upstream main, NOT a tag: v4 (`LATEST_SPZ_HEADER_VERSION = 4`,
# smallest-three quaternions from v3, ZSTD NGSP container from v4) landed after
# v3.0.0. Files written by @playcanvas/splat-transform 3.3.3 are v4, and the
# v1.1.0 loader this repo used to pin accepted versions 1–2 only — it returned
# an empty cloud whose `convertCoordinates()` then did `numCoeffs / numPoints`
# with numPoints == 0, i.e. the INT_DIVIDE_BY_ZERO (0xC0000094) crash. Keep this
# SHA identical to android/src/main/cpp/CMakeLists.txt.
FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG        affd0ecea7fbb4c265ee119475af7ee5b2997482
    SOURCE_SUBDIR  _skip
)
FetchContent_MakeAvailable(spz)

# No source patching any more: upstream now includes <algorithm> and <limits>
# itself and no longer compile-time-divides by zero, so the historical
# splat-types.{h,cc} rewrites are gone. `constexpr `→`inline ` in particular
# MUST NOT come back — upstream has `inline constexpr` tables and a
# `constexpr int SH_MAX_DEGREE` used as an array bound, both of which that
# blanket replace would break. M_PI in splat-types.h is still MSVC-sensitive,
# but upstream's splat-c-types.h now #defines _USE_MATH_DEFINES itself right
# before <math.h>, so nothing is needed from us.
add_library(spz_lib STATIC
    ${spz_SOURCE_DIR}/src/cc/load-spz.cc
    ${spz_SOURCE_DIR}/src/cc/splat-c-types.cc
    ${spz_SOURCE_DIR}/src/cc/splat-types.cc
)
target_include_directories(spz_lib PUBLIC ${spz_SOURCE_DIR}/src/cc)
target_link_libraries(spz_lib PRIVATE ${SPZ_ZLIB_TARGET} ${SPZ_ZSTD_TARGET})
set_target_properties(spz_lib PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # No _USE_MATH_DEFINES / /FIalgorithm here any more: upstream's
    # splat-c-types.h defines the former itself (ours only produced a C4005
    # redefinition warning in every TU) and splat-types.h includes <algorithm>.
    set_target_properties(spz_lib PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ── SPIR-V shader compilation ────────────────────────────────────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
endif()

set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    # Compute compositor (GsRenderer) — immediate-mode desktop path.
    shaders/precomp_cov3d.comp
    shaders/preprocess.comp
    shaders/prefix_sum.comp
    shaders/preprocess_sort.comp
    shaders/hist.comp
    shaders/sort.comp
    shaders/tile_boundary.comp
    shaders/render.comp
    # Graphics pipeline (GsAdrenoRenderer) — TBDR path, also built on desktop so
    # the renderer is selectable at compile time (gs_renderer_select.h). The
    # glslangValidator loop infers stage from extension, so .vert/.frag here
    # compile the same way (header splat.vert.h, var splat_vert_data, etc.).
    # precomp_cov3d/hist/sort are shared with the compute path above.
    shaders/adreno_preprocess.comp
    shaders/splat_keys.comp
    shaders/splat.vert
    shaders/splat.frag
)

set(SHADER_HEADERS "")
foreach(GLSL_REL ${SHADER_SOURCES})
    get_filename_component(GLSL_NAME ${GLSL_REL} NAME)
    string(MAKE_C_IDENTIFIER ${GLSL_NAME} IDENTIFIER)
    set(VARNAME "${IDENTIFIER}_data")
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${GLSL_NAME}.h")
    set(GLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL_REL}")
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            -I${SHADER_INCLUDE_DIR}
            ${GLSL_SRC}
            --vn ${VARNAME}
            -o ${HEADER}
        DEPENDS ${GLSL_SRC} ${SHADER_INCLUDE_DIR}/common.glsl
        COMMENT "Compiling SPIR-V: ${GLSL_NAME}"
    )
    list(APPEND SHADER_HEADERS ${HEADER})
endforeach()

add_library(gs_renderer STATIC
    gs_renderer.h
    gs_renderer.cpp
    gs_adreno_renderer.h
    gs_adreno_renderer.cpp
    gs_renderer_select.h
    gs_scene_loader.h
    gs_scene_loader.cpp
    gs_spz_loader.h
    gs_spz_loader.cpp
    gs_vulkan_utils.h
    gs_vulkan_utils.cpp
    ${SHADER_HEADERS}
)

target_include_directories(gs_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(gs_renderer PUBLIC
    Vulkan::Vulkan
    spz_lib
)

# gs_spz_loader.cpp peels the gzip wrapper itself before handing the payload to
# spz (see the comment there), so it needs zlib directly — spz_lib links it
# PRIVATE and therefore does not propagate it.
target_link_libraries(gs_renderer PRIVATE ${SPZ_ZLIB_TARGET})
