# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Native build for the Android leg of displayxr-demo-gaussiansplat (3D
# gaussian-splat viewer). Reuses the cube_handle_vk_android OpenXR-Android
# harness (loader → instance → Vulkan device → stereo swapchains → session)
# and binds to the DisplayXR runtime out-of-process (ADR-025). The renderer is
# the SHARED 3dgs_common/ sources, referenced by relative path — NOT copied —
# so the Android leg and the windows/ + macos/ legs build the exact same
# renderer (32-bit packed sort keys, no shaderInt64 — Adreno-compatible).

cmake_minimum_required(VERSION 3.22.1)
project(gausssplat_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)          # gs_renderer requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root is four levels up from android/src/main/cpp.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(GS_COMMON "${REPO_ROOT}/3dgs_common")

# ── Niantic SPZ format support (FetchContent; zlib comes from the NDK) ────
include(FetchContent)

# zstd — required since SPZ header version 4 (the NGSP container stores each
# attribute as its own ZSTD stream). The NDK sysroot ships zlib but NOT zstd,
# so it is built from source here, exactly as the desktop leg does in
# 3dgs_common/CMakeLists.txt. Plain C, no platform assumptions —
# ZSTD_DISABLE_ASM drops the x86-64 .S so every ABI (arm64-v8a included) uses
# the C decoder.
FetchContent_Declare(zstd
    URL      https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
    URL_HASH SHA256=8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1
    SOURCE_SUBDIR _skip)
FetchContent_MakeAvailable(zstd)
file(GLOB _ZSTD_SRCS
    "${zstd_SOURCE_DIR}/lib/common/*.c"
    "${zstd_SOURCE_DIR}/lib/compress/*.c"
    "${zstd_SOURCE_DIR}/lib/decompress/*.c")
add_library(zstd_lib STATIC ${_ZSTD_SRCS})
target_include_directories(zstd_lib PUBLIC "${zstd_SOURCE_DIR}/lib")
target_compile_definitions(zstd_lib PRIVATE ZSTD_DISABLE_ASM=1 XXH_NAMESPACE=ZSTD_)

# Pinned to upstream main, NOT a tag — v4 landed after v3.0.0. Keep this SHA
# identical to 3dgs_common/CMakeLists.txt.
FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG        affd0ecea7fbb4c265ee119475af7ee5b2997482
    SOURCE_SUBDIR  _skip)
FetchContent_MakeAvailable(spz)

add_library(spz_lib STATIC
    ${spz_SOURCE_DIR}/src/cc/load-spz.cc
    ${spz_SOURCE_DIR}/src/cc/splat-c-types.cc
    ${spz_SOURCE_DIR}/src/cc/splat-types.cc)
target_include_directories(spz_lib PUBLIC ${spz_SOURCE_DIR}/src/cc)
target_link_libraries(spz_lib PRIVATE z zstd_lib)    # NDK sysroot zlib + fetched zstd

# libwebp — required by the SOG reader: a `.sog` bundle stores every splat
# attribute as a WebP texture (means / quats / scales / sh0 / shN), and nothing
# already in the tree decodes WebP (stb_image does not, and the drift guard
# forbids re-vendoring an image decoder here anyway — see
# .github/workflows/no-vendored-math.yml). Fetched the same way zstd is: a
# release tarball pinned by SHA256 (a moving branch is what the SPZ comment
# above records the pain of), from the project's own download host rather than
# GitHub's generated archives.
#
# DECODE ONLY. Every WEBP_BUILD_* tool is forced OFF — they pull libpng /
# giflib / SDL, none of which a build machine is required to have — and we link
# `webpdecoder`, the decoder-only library, not `webp`. WEBP_BUILD_EXTRAS off
# also keeps its `set_version` / stub-file machinery out of the way. The
# targets inherit this directory's CMAKE_MSVC_RUNTIME_LIBRARY (static CRT), the
# same mechanism zlib and zstd rely on.
# Keep this block identical to 3dgs_common/CMakeLists.txt.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP      OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO   OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX    OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_FUZZTEST   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libwebp
    URL      https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz
    URL_HASH SHA256=7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c
)
FetchContent_MakeAvailable(libwebp)
if(TARGET webpdecoder)
    set(SOG_WEBP_TARGET webpdecoder)
else()
    # Older libwebp releases only expose the combined library.
    set(SOG_WEBP_TARGET webp)
endif()
if(MSVC)
    set_target_properties(${SOG_WEBP_TARGET} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ── 3DGS compute shaders → SPIR-V header arrays ───────────────────────────
# Compiled from the SHARED 3dgs_common/shaders. var name = <name>_comp_data,
# header = <name>.comp.h (matches the #includes in gs_renderer.cpp).
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK on the build host")
endif()
set(SHADER_DIR "${GS_COMMON}/shaders")
# Adreno/TBDR-native graphics-pipeline splat path (gs_adreno_renderer.cpp):
# preprocess(N) -> keygen -> radix sort(N) -> instanced quad draw. NONE of the
# desktop compute-compositor shaders (preprocess/preprocess_sort/prefix_sum/
# tile_boundary/render) are built — the hardware ROP composites in tile memory.
# glslangValidator infers the stage from the extension, so splat.vert/.frag
# compile the same way (header splat.vert.h, var splat_vert_data, etc.).
set(GS_SHADERS precomp_cov3d.comp adreno_preprocess.comp hist.comp sort.comp
    splat_keys.comp splat.vert splat.frag)
set(SHADER_HEADERS "")
foreach(S ${GS_SHADERS})
    string(MAKE_C_IDENTIFIER ${S} VN)          # preprocess_sort.comp -> preprocess_sort_comp
    set(HDR "${CMAKE_CURRENT_BINARY_DIR}/${S}.h")
    add_custom_command(
        OUTPUT ${HDR}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2 -I${SHADER_DIR}
            ${SHADER_DIR}/${S} --vn ${VN}_data -o ${HDR}
        DEPENDS ${SHADER_DIR}/${S} ${SHADER_DIR}/common.glsl
        COMMENT "Compiling SPIR-V: ${S}")
    list(APPEND SHADER_HEADERS ${HDR})
endforeach()

# ── NativeActivity glue (ships with the NDK) ─────────────────────────────
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

# displayxr-common's header-only view-configuration helper
# (common/dxr_view_config.h: DxrSelectViewConfigType) — the SAME header the
# desktop legs reach through displayxr::rules, so there is one implementation
# (runtime#1612). Fetch sources only; don't run its CMake, which carries
# desktop platform deps. Keep the pin identical to common/CMakeLists.txt.
FetchContent_Declare(displayxr_common_rules
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v2.16.0
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(displayxr_common_rules)
if(NOT displayxr_common_rules_POPULATED)
    FetchContent_Populate(displayxr_common_rules)
endif()

add_library(gausssplat_vk_android SHARED
    main.cpp
    # ── Adreno/TBDR-native renderer (3dgs_common/) — NOT the desktop
    #    gs_renderer.cpp compute compositor; this is the mobile graphics path. ──
    ${GS_COMMON}/gs_adreno_renderer.cpp
    ${GS_COMMON}/gs_perf_knobs.cpp
    ${GS_COMMON}/gs_scene_loader.cpp
    ${GS_COMMON}/gs_spz_loader.cpp
    # gs_scene_loader.cpp dispatches .sog to ParseSogFile and the camera rig to
    # GsMedianForwardDepth, so both are link-time requirements of the loader
    # even though this leg has no file picker — the bundled asset alone decides
    # nothing about which symbols the archive needs.
    ${GS_COMMON}/gs_sog_loader.cpp
    ${GS_COMMON}/gs_camera_rig.cpp
    # Renderer-independent display-rig fit (bounds + vHeight). Shared with the
    # desktop legs so the framing is identical on every platform; this leg has
    # to list it explicitly because the source list here is CURATED, not a glob.
    ${GS_COMMON}/gs_scene_fit.cpp
    ${GS_COMMON}/gs_vulkan_utils.cpp
    ${SHADER_HEADERS})

target_include_directories(gausssplat_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.comp.h
    ${GS_COMMON}                                # gs_renderer.h, ...
    ${libwebp_SOURCE_DIR}/src                   # webp/decode.h for gs_sog_loader
    ${REPO_ROOT}/openxr_includes                # DisplayXR XR_EXT_* headers
    ${displayxr_common_rules_SOURCE_DIR}/common) # dxr_view_config.h (displayxr-common, runtime#1612)

set_target_properties(gausssplat_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(gausssplat_vk_android
    native_app_glue
    spz_lib
    z                                           # gs_spz_loader peels gzip itself;
                                                # gs_sog_loader raw-inflates each
                                                # PKZip member with the same zlib
    ${SOG_WEBP_TARGET}                          # SOG attribute textures are WebP
    OpenXR::openxr_loader
    vulkan
    android
    log)
