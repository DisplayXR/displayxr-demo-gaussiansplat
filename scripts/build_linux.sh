#!/usr/bin/env bash
#
# scripts/build_linux.sh — Build the Linux demo binary (build-green, issue #60).
#
# Mirrors scripts/build_macos.sh per the runtime repo's
# docs/guides/linux-demo-port.md, with the Linux swaps: system Vulkan
# (libvulkan-dev — no MoltenVK, no ICD manifest), a from-source OpenXR loader
# pinned to loader 1.1.43 (the org-wide loader pin; keep equal to CI), and no
# installer step (Linux packaging is out of scope until on-screen lands).
#
# This demo has NO FFmpeg. The Linux leg builds the GRAPHICS splat renderer
# (GsAdrenoRenderer: splat.vert / splat.frag) — linux/CMakeLists.txt sets
# GS_RENDERER=GRAPHICS, which overrides gs_renderer_select.h's own desktop-x86_64
# default of COMPUTE. The graphics path is NOT Android/Apple-Silicon-only: the
# desktop head-to-head showed it also wins on immediate-mode GPUs, because its
# radix sort sorts N gaussians instead of N x 8 fragments. The build needs
# glslangValidator (SPIR-V) and zlib (Niantic SPZ loader; fetched by 3dgs_common
# if the system copy is absent).
#
# Usage:
#   ./scripts/build_linux.sh
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.43).
#                    Keep this pin equal to CI — CI runs this script.
#   GS_RENDERER      Splat renderer: GRAPHICS (default) | COMPUTE | AUTO.
#                    Passed through to CMake. AUTO defers to gs_renderer_select.h
#                    (COMPUTE on desktop x86_64).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
GS_RENDERER="${GS_RENDERER:-GRAPHICS}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so" ] || \
   [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION -> $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
# Vulkan resolves via the system libvulkan-dev; the OpenXR loader via
# CMAKE_PREFIX_PATH; glslangValidator via glslang-tools; zlib via the system
# copy (else 3dgs_common FetchContent-fetches it).
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGS_RENDERER="$GS_RENDERER" \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build build

BIN="$REPO_ROOT/build/linux/gaussian_splatting_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

# --- 2. point at the canonical run script ---------------------------------
# The run script is a TRACKED file at scripts/run_gaussiansplat_linux.sh, not
# a heredoc emitted into build/. Two reasons: the runtime's
# scripts/run_linux_demo.sh harness only looks in the demo's scripts/ dir
# (maxdepth 1) and hard-errors when it finds nothing there, and a generated
# copy is invisible to review — which is how the previous one came to default
# XR_RUNTIME_JSON to $HOME/.config/openxr/1/active_runtime.json and
# XRT_PLUGIN_SEARCH_PATH to /usr/local/lib/displayxr/plugins. Neither path
# exists on a from-source dev box: the first silently loads whatever runtime
# is installed instead of the one just built, and the second presents as "no
# display processor found" at xrCreateInstance.
RUN="$REPO_ROOT/scripts/run_gaussiansplat_linux.sh"

echo ""
echo "Built: $BIN"
if [ -x "$RUN" ]; then
    echo "Run against a dev runtime: $RUN"
else
    echo "warning: $RUN is missing — the runtime's run_linux_demo.sh harness needs it" >&2
fi
