# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build — lives inside displayxr-demo-gaussiansplat repo as
# windows/CMakeLists.txt. Assumes sibling dirs: ../common, ../3dgs_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../3dgs_common as subdirectories, exposing
# `sr_common_base`, `sr_common`, and `gs_renderer` targets.

cmake_minimum_required(VERSION 3.21)

if(NOT WIN32)
    return()
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found — skipping gaussian_splatting_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CONFIG pkg first, then a user-specified OpenXR_ROOT fallback.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS "${OpenXR_ROOT}/x64/lib" "${OpenXR_ROOT}/lib" "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS "${OpenXR_ROOT}/x64/bin" "${OpenXR_ROOT}/bin"
            NO_DEFAULT_PATH
        )
        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        else()
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        endif()
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Provide -DOpenXR_ROOT=<path> or install via vcpkg.")
endif()

add_executable(gaussian_splatting_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    # 'I' key atlas capture: VK readback (filename + flash come via sr_common_base)
    ${CMAKE_SOURCE_DIR}/common/atlas_capture_vk.cpp
    resource.rc
)

target_include_directories(gaussian_splatting_handle_vk_win PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
)

target_link_libraries(gaussian_splatting_handle_vk_win PRIVATE
    sr_common_base
    sr_common
    gs_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    d2d1
    dwrite
    comdlg32
)

if(MSVC)
    # /utf-8: tell MSVC the source files are UTF-8 (and the runtime
    # char set is UTF-8). Without this MSVC reads source literals
    # using the system codepage (Windows-1252 on US machines) and
    # mangles multi-byte glyphs like the "…" in "Open…" into
    # Mojibake (e.g. "Openâ€¦") at compile time. Affects the button
    # labels rendered by sr_common's HUD.
    target_compile_options(gaussian_splatting_handle_vk_win PRIVATE /utf-8)

    file(RELATIVE_PATH _manifest_rel
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sr_3dgs_openxr_ext_vk.manifest")
    set_target_properties(gaussian_splatting_handle_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${_manifest_rel}"
    )
endif()

# Copy OpenXR loader DLL next to exe. Use $<TARGET_FILE:...> rather than the
# IMPORTED_LOCATION property: the Khronos OpenXR CONFIG package only sets the
# *config-specific* IMPORTED_LOCATION_<CONFIG> (e.g. IMPORTED_LOCATION_RELWITHDEBINFO),
# leaving the generic IMPORTED_LOCATION unset. get_target_property(... IMPORTED_LOCATION)
# would return NOTFOUND, silently skipping the copy and shipping an exe with no
# loader next to it (the installer's `File openxr_loader.dll` then fails). The
# generator expression resolves the correct per-config DLL path.
if(TARGET OpenXR::openxr_loader)
    add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenXR::openxr_loader>"
            "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>"
        COMMENT "Copying OpenXR loader DLL"
    )
endif()

# SimulatedRealityVulkanBeta.dll (the Leia SR Vulkan weaver) is intentionally
# NOT bundled here. It is a Leia SDK binary shipped by the Leia plug-in
# installer next to DisplayXR-LeiaSR.dll, which loads it from its own directory
# (displayxr-leia-plugin, leia_dp_factory_vk). Vendor binaries must not appear
# in vendor-neutral DisplayXR demos.

# Copy bundled default scene next to the exe.
set(BUNDLED_SCENE "${CMAKE_CURRENT_SOURCE_DIR}/assets/butterfly.spz")
if(EXISTS "${BUNDLED_SCENE}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_SCENE}" "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>/butterfly.spz"
        COMMENT "Copying bundled butterfly.spz next to exe"
    )
endif()

# Copy the DisplayXR shell sidecar (.displayxr.json + icons) next to the exe
# so the installer / shell scanner picks it up.
include(${CMAKE_SOURCE_DIR}/common/displayxr_manifest.cmake)
displayxr_install_manifest(gaussian_splatting_handle_vk_win
    "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")
