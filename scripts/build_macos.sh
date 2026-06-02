#!/usr/bin/env bash
#
# scripts/build_macos.sh — Build the demo binary, and optionally the .pkg
# installer for distribution.
#
# Usage:
#   ./scripts/build_macos.sh                run cmake build only
#   ./scripts/build_macos.sh --installer    also stage artifacts and build
#                                            _package/DisplayXRGaussianSplat-*.pkg
#
# Env:
#   DISPLAYXR_VERSION   version string baked into the .pkg + .app Info.plist
#                       (defaults to 0.0.0-dev if unset). CI sets this from
#                       the v* git tag.
#   OPENXR_VERSION      OpenXR-SDK release tag to build the loader from
#                       (defaults to 1.1.43, mirroring the runtime repo).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_INSTALLER=false
for arg in "${@}"; do
    case "$arg" in
        --installer) BUILD_INSTALLER=true ;;
        -h|--help)
            sed -n '1,17p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown arg: $arg" >&2
            exit 2
            ;;
    esac
done

# --- 0. Build OpenXR loader from source -----------------------------------
# Homebrew has no `openxr-loader` formula, so we build the loader from the
# Khronos SDK and install it under /tmp/openxr-install. This mirrors the
# DisplayXR runtime repo's approach (scripts/build_macos.sh:56–91). The
# BUILD_WITH_SYSTEM_JSONCPP=OFF + jsoncpp include-order patch are required
# to avoid baking a Homebrew jsoncpp path into the redistributable dylib
# (runtime repo issue #205).
OPENXR_VERSION="${OPENXR_VERSION:-1.1.43}"
OPENXR_DIR="/tmp/openxr-install"
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.dylib" ]; then
    echo "==> Building OpenXR loader $OPENXR_VERSION → $OPENXR_DIR"
    rm -rf /tmp/openxr-sdk
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
        https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk

    /usr/bin/sed -i '' \
        's|PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|' \
        /tmp/openxr-sdk/src/loader/CMakeLists.txt

    cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
        -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
        -DCMAKE_MAP_IMPORTED_CONFIG_RELEASE="Release;None;"
    cmake --build /tmp/openxr-sdk/build
    cmake --install /tmp/openxr-sdk/build
else
    echo "==> OpenXR loader cached at $OPENXR_DIR"
fi

# --- 1. cmake build -------------------------------------------------------
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build build

BIN="$REPO_ROOT/build/macos/gaussian_splatting_handle_vk_macos"
if [ ! -x "$BIN" ]; then
    echo "Error: expected binary not found at $BIN" >&2
    exit 1
fi

# --- 1b. Share the DisplayXR runtime's Vulkan loader ----------------------
# The DisplayXR runtime ships its OWN libvulkan.1.dylib (+ MoltenVK). If the
# app links a *different* loader (e.g. Homebrew's vulkan-loader), the process
# ends up with two loader instances: the app creates its VkInstance with one,
# but the runtime's xrGetVulkanGraphicsDeviceKHR enumerates that instance with
# the other — which has no record of it, so the loader reports "0 valid GPUs"
# and session create fails (XR_ERROR_RUNTIME_FAILURE, -3). Repoint the binary
# at the runtime's loader so both sides share one. Guarded on the runtime being
# installed (it must be, to run the app), so CI/installer builds on machines
# without the runtime are unaffected and keep their bundled loader. Idempotent.
RUNTIME_VK="/Library/Application Support/DisplayXR/lib/libvulkan.1.dylib"
if [ -f "$RUNTIME_VK" ]; then
    # Extract the full current install path (may contain spaces): strip the
    # leading indent and the trailing " (compatibility version …)" suffix.
    CURRENT_VK="$(otool -L "$BIN" | grep 'libvulkan\.1\.dylib' | head -1 \
        | sed -E 's/^[[:space:]]*(.*) \(compatibility.*$/\1/')"
    if [ -n "$CURRENT_VK" ] && [ "$CURRENT_VK" != "$RUNTIME_VK" ]; then
        install_name_tool -change "$CURRENT_VK" "$RUNTIME_VK" "$BIN"
        echo "==> Repointed libvulkan: $CURRENT_VK -> runtime loader (shared, avoids dual-loader)"
    else
        echo "==> libvulkan already shares the runtime loader"
    fi
fi

if [ "$BUILD_INSTALLER" != "true" ]; then
    echo ""
    echo "Run: $BIN"
    exit 0
fi

# --- 2. stage artifacts for the installer ---------------------------------
VERSION="${DISPLAYXR_VERSION:-0.0.0-dev}"
echo "==> Staging installer artifacts (version=$VERSION)"

ARTIFACT_DIR="$REPO_ROOT/_artifact"
rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR/bin" "$ARTIFACT_DIR/lib" "$ARTIFACT_DIR/assets" "$ARTIFACT_DIR/displayxr"

cp "$BIN" "$ARTIFACT_DIR/bin/"

# Bundled scene + manifest + icons live in the repo.
if [ -f "$REPO_ROOT/macos/assets/butterfly.spz" ]; then
    cp "$REPO_ROOT/macos/assets/butterfly.spz" "$ARTIFACT_DIR/assets/"
fi
if [ -d "$REPO_ROOT/macos/displayxr" ]; then
    cp -R "$REPO_ROOT/macos/displayxr/." "$ARTIFACT_DIR/displayxr/"
fi

# Bundled OpenXR loader (built from source above).
cp -P "$OPENXR_DIR"/lib/libopenxr_loader*.dylib "$ARTIFACT_DIR/lib/"

# Bundled Vulkan + MoltenVK via Homebrew. Resolve through `brew --prefix
# <formula>` so we don't hardcode arm64 (/opt/homebrew) vs x86_64
# (/usr/local) — CI runners on macos-14 and local Apple Silicon dev
# machines diverge here.
copy_from_brew_formula() {
    local formula="$1"
    local pattern="$2"
    local prefix
    prefix="$(brew --prefix "$formula" 2>/dev/null || true)"
    if [ -z "$prefix" ] || [ ! -d "$prefix" ]; then
        echo "Error: brew formula '$formula' not found — \`brew install $formula\` first" >&2
        return 1
    fi
    local found=0
    while IFS= read -r f; do
        cp -P "$f" "$ARTIFACT_DIR/lib/"
        found=1
    done < <(find "$prefix/lib" -maxdepth 1 \( -name "$pattern" -type f -o -name "$pattern" -type l \) 2>/dev/null)
    if [ "$found" = 0 ]; then
        echo "Error: no '$pattern' under $prefix/lib" >&2
        return 1
    fi
}

copy_from_brew_formula vulkan-loader 'libvulkan*.dylib'
copy_from_brew_formula molten-vk     'libMoltenVK*.dylib'

# Sanity-check the three families actually got something.
for required in libopenxr_loader libvulkan libMoltenVK; do
    if ! ls "$ARTIFACT_DIR/lib/${required}"* >/dev/null 2>&1; then
        echo "Error: $required not staged under _artifact/lib/" >&2
        echo "       Install with: brew install vulkan-loader molten-vk" >&2
        exit 1
    fi
done

# --- 3. .pkg ---------------------------------------------------------------
PKG_DIR="$REPO_ROOT/_package"
mkdir -p "$PKG_DIR"
PKG_PATH="$PKG_DIR/DisplayXRGaussianSplat-${VERSION}.pkg"

DISPLAYXR_VERSION="$VERSION" \
    bash "$REPO_ROOT/installer/macos/build_installer.sh" "$ARTIFACT_DIR" "$PKG_PATH"

echo ""
echo "==> Installer: $PKG_PATH"
ls -lh "$PKG_PATH"
