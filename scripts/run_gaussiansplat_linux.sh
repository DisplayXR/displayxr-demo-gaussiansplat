#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the Linux Gaussian Splat demo against a local DisplayXR dev runtime.
#
# Mirrors displayxr-demo-modelviewer/scripts/run_modelviewer_linux.sh so the
# runtime's scripts/run_linux_demo.sh harness can launch this demo: that
# harness searches THIS directory (maxdepth 1) for run_*linux*.sh and
# hard-errors when it finds none, which is why the copy build_linux.sh emits
# into build/ is not enough — build/ is not on the harness's search path.
# Ship exactly ONE run_*linux*.sh here: the harness takes `find ... | head -1`.
#
# Every knob is `: "${VAR:=default}"`, so anything the harness (or you) already
# exported wins.
#
# Usage: scripts/run_gaussiansplat_linux.sh [scene.spz|.ply|.sog] [extra args...]
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${REPO_DIR}/build/linux"
BIN="${BIN_DIR}/gaussian_splatting_handle_vk_linux"
RUNTIME_BUILD="${REPO_DIR}/../displayxr-runtime/build"

# Dev runtime manifest from the sibling runtime checkout. Override by
# exporting XR_RUNTIME_JSON before invoking.
#
# NOTE this is deliberately NOT ~/.config/openxr/1/active_runtime.json: that
# path is the *installed* active runtime, it does not exist on a dev box that
# has only ever built from source, and pointing at it silently loads whatever
# runtime happens to be registered rather than the one you just built.
: "${XR_RUNTIME_JSON:=${RUNTIME_BUILD}/openxr_displayxr-dev.json}"
export XR_RUNTIME_JSON

# Plug-in discovery (POSIX search path). The runtime's scripts/build_linux.sh
# stages DisplayXR-SimDisplay.so + its manifest here; a Leia plug-in built from
# source lands here too. Again NOT /usr/local/lib/displayxr/plugins — that is
# an install location, and on a from-source box it is empty or absent, which
# presents as "no display processor found" at xrCreateInstance.
: "${XRT_PLUGIN_SEARCH_PATH:=${RUNTIME_BUILD}/_plugins}"
export XRT_PLUGIN_SEARCH_PATH

# Native Vulkan compositor (the only Linux compositor backend).
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"

# sim-display weave so output is eyeball-checkable without 3D hardware. On a
# real Leia panel the vendor plug-in wins and this is inert.
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"

# The script-built OpenXR loader. Unlike Windows/macOS the exe-adjacent copy
# is not on the Linux search path, and scripts/build_linux.sh installs the
# pinned loader under /tmp/openxr-install.
export LD_LIBRARY_PATH="${BIN_DIR}:/tmp/openxr-install/lib:${LD_LIBRARY_PATH:-}"

if [[ ! -f "${XR_RUNTIME_JSON}" ]]; then
    echo "warning: XR_RUNTIME_JSON not found: ${XR_RUNTIME_JSON}" >&2
    echo "         build the runtime (scripts/build_linux.sh there) or set XR_RUNTIME_JSON." >&2
fi
if [[ ! -d "${XRT_PLUGIN_SEARCH_PATH}" ]]; then
    echo "warning: XRT_PLUGIN_SEARCH_PATH not found: ${XRT_PLUGIN_SEARCH_PATH}" >&2
    echo "         without a display-processor plug-in xrCreateInstance will fail." >&2
fi
if [[ ! -x "${BIN}" ]]; then
    echo "error: ${BIN} not built. Run: ./scripts/build_linux.sh" >&2
    exit 1
fi

echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON}"
echo "XRT_PLUGIN_SEARCH_PATH=${XRT_PLUGIN_SEARCH_PATH}"
exec "${BIN}" "$@"
