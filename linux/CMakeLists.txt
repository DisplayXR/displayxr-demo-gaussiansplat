# Copyright 2025, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Standalone build — Linux leg of displayxr-demo-gaussiansplat (issue #60,
# M8 Linux epic). Mirrors macos/CMakeLists.txt: assumes the top-level
# CMakeLists.txt already added ../common and ../3dgs_common (exposing the
# sr_common_base and gs_renderer targets) and ../openxr_includes.
#
# Build-green scope: compile gaussian_splatting_handle_vk_linux on
# ubuntu-latest. Window arm is hosted-NULL (the runtime self-creates the
# window); the app links no windowing toolkit. The generic COMPUTE splat
# renderer (GsRenderer) is selected on desktop x86_64 by gs_renderer_select.h.

cmake_minimum_required(VERSION 3.21)

if(APPLE OR WIN32)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CMake package (from CMAKE_PREFIX_PATH, e.g. the from-source
# loader in scripts/build_linux.sh) → system library → fatal.
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}")
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Build the loader from source (scripts/build_linux.sh) or install libopenxr-loader1.")
endif()

add_executable(gaussian_splatting_handle_vk_linux
    main.cpp
)

target_include_directories(gaussian_splatting_handle_vk_linux PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/3dgs_common
)

target_link_libraries(gaussian_splatting_handle_vk_linux PRIVATE
    sr_common_base
    gs_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    ${CMAKE_DL_LIBS}
)

# Copy bundled default scene next to the exe (auto-loaded at startup). Shared
# with the macОS leg — no need to duplicate the 3.9 MB asset under linux/.
set(BUNDLED_SCENE "${CMAKE_SOURCE_DIR}/macos/assets/butterfly.spz")
if(EXISTS "${BUNDLED_SCENE}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_SCENE}"
            "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_linux>/butterfly.spz"
        COMMENT "Copying bundled butterfly.spz next to exe")
endif()
