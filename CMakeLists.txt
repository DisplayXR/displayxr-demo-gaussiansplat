# Copyright 2025, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists.txt for the displayxr-demo-gaussiansplat standalone
# repo. Orchestrates the shared `common` and `3dgs_common` libraries, then
# dispatches to macos/ or windows/ for the platform-specific executable.

cmake_minimum_required(VERSION 3.21)
project(displayxr-demo-gaussiansplat LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared libraries (built once, linked by the platform executable).
add_subdirectory(common)
add_subdirectory(3dgs_common)

# Platform-specific executable.
if(APPLE)
    add_subdirectory(macos)
elseif(WIN32)
    add_subdirectory(windows)
elseif(UNIX)
    # Desktop Linux (build-green, issue #60). OBJCXX stays gated on APPLE above,
    # so it is naturally dropped here. Window arm = hosted-NULL / SDL-provided;
    # no xlib binding path is invented (see linux/main.cpp TODO(Phase 3)).
    add_subdirectory(linux)
else()
    message(FATAL_ERROR "displayxr-demo-gaussiansplat supports macOS, Windows and Linux only.")
endif()
