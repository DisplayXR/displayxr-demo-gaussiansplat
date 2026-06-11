# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
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
FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG        v1.1.0
    SOURCE_SUBDIR  _skip)
FetchContent_MakeAvailable(spz)

add_library(spz_lib STATIC
    ${spz_SOURCE_DIR}/src/cc/load-spz.cc
    ${spz_SOURCE_DIR}/src/cc/splat-c-types.cc
    ${spz_SOURCE_DIR}/src/cc/splat-types.cc)
target_include_directories(spz_lib PUBLIC ${spz_SOURCE_DIR}/src/cc)
target_link_libraries(spz_lib PRIVATE z)    # NDK sysroot zlib

# ── 3DGS compute shaders → SPIR-V header arrays ───────────────────────────
# Compiled from the SHARED 3dgs_common/shaders. var name = <name>_comp_data,
# header = <name>.comp.h (matches the #includes in gs_renderer.cpp).
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK on the build host")
endif()
set(SHADER_DIR "${GS_COMMON}/shaders")
set(GS_SHADERS precomp_cov3d.comp preprocess.comp prefix_sum.comp
    preprocess_sort.comp hist.comp sort.comp tile_boundary.comp render.comp)
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

add_library(gausssplat_vk_android SHARED
    main.cpp
    # ── SHARED renderer (3dgs_common/) ──
    ${GS_COMMON}/gs_renderer.cpp
    ${GS_COMMON}/gs_scene_loader.cpp
    ${GS_COMMON}/gs_spz_loader.cpp
    ${GS_COMMON}/gs_vulkan_utils.cpp
    ${SHADER_HEADERS})

target_include_directories(gausssplat_vk_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}                 # generated *.comp.h
    ${GS_COMMON}                                # gs_renderer.h, ...
    ${REPO_ROOT}/openxr_includes)               # DisplayXR XR_EXT_* headers

set_target_properties(gausssplat_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(gausssplat_vk_android
    native_app_glue
    spz_lib
    OpenXR::openxr_loader
    vulkan
    android
    log)
