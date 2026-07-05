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
# This demo has NO FFmpeg. The generic COMPUTE splat renderer (GsRenderer) is
# used on Linux desktop (gs_renderer_select.h); the Adreno/TBDR graphics path
# is Android/Apple-Silicon only. The build needs glslangValidator (SPIR-V) and
# zlib (Niantic SPZ loader; fetched by 3dgs_common if the system copy is absent).
#
# Usage:
#   ./scripts/build_linux.sh
#
# Env:
#   OPENXR_VERSION   OpenXR-SDK release tag for the loader (default 1.1.43).
#                    Keep this pin equal to CI — CI runs this script.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Build OpenXR loader from source -----------------------------------
# Distro loaders lag; build the pinned Khronos loader and install it under
# /tmp/openxr-install (mirrors build_macos.sh + the runtime repo's
# scripts/build_linux.sh --apps). Cached: skipped if both the .so and the
# CMake package config are already present.
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
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
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build build

BIN="$REPO_ROOT/build/linux/gaussian_splatting_handle_vk_linux"
[ -x "$BIN" ] || { echo "Error: expected binary not found at $BIN" >&2; exit 1; }

# --- 2. emit a run script pinned to a dev runtime -------------------------
# On-screen validation is a later pass (needs the Linux runtime + a GPU + an X
# server). This wires the dev runtime manifest + sim-display plug-in dir the
# same way the macOS run_*.sh scripts do.
RUN="$REPO_ROOT/build/run_gaussiansplat_linux.sh"
cat > "$RUN" <<'EOF'
#!/usr/bin/env bash
# Run the Linux Gaussian Splat demo against a dev DisplayXR runtime.
# Point XR_RUNTIME_JSON at the dev runtime manifest and XRT_PLUGIN_SEARCH_PATH
# at the sim-display plug-in dir before running. Overridable via the env.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
: "${XR_RUNTIME_JSON:=$HOME/.config/openxr/1/active_runtime.json}"
: "${XRT_PLUGIN_SEARCH_PATH:=/usr/local/lib/displayxr/plugins}"
: "${SIM_DISPLAY_OUTPUT:=anaglyph}"
export XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH SIM_DISPLAY_OUTPUT
export OXR_ENABLE_VK_NATIVE_COMPOSITOR=1
exec "$DIR/linux/gaussian_splatting_handle_vk_linux" "$@"
EOF
chmod +x "$RUN"

echo ""
echo "Built: $BIN"
echo "Run against a dev runtime: $RUN"
