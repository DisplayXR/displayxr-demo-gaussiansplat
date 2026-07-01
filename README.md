# DisplayXR Demo — Gaussian Splatting

Real-time 3D Gaussian Splatting viewer for glasses-free 3D displays, built on the DisplayXR runtime via OpenXR with Vulkan. Loads `.spz` and `.ply` files, renders with asymmetric per-eye Kooima projection for the full stereo/multiview experience.

> **Requires the DisplayXR runtime v1.3.0 or newer** (Windows) / **the latest macOS runtime `.pkg`**. Download the matching installer from the [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases): `DisplayXRSetup-*.exe` on Windows or `DisplayXR-Installer-*.pkg` on macOS. v1.3.0 ships the Vulkan transparent-window bridge that this demo's HWND + session unconditionally rely on; older runtimes will produce a broken/black window. The shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases)) is optional — install it on top of the runtime only if you want the spatial workspace shell.

## Download

Prebuilt binaries are attached to every [release](https://github.com/DisplayXR/displayxr-demo-gaussiansplat/releases):

- **Windows** — `gaussian_splatting_handle_vk_win-v<version>.zip` (unzip, run the exe next to its bundled DLLs)
- **macOS** — `DisplayXRGaussianSplat-<version>.pkg` (double-click to install — lands at `/Applications/Gaussian Splat Viewer.app`). Requires the DisplayXR runtime `.pkg` to be installed first.

A test scene, `butterfly.spz`, is bundled and auto-loads at startup.

## Controls

| Input | Action |
|---|---|
| WASD / Q / E | Strafe the virtual display in 3D |
| Left-click drag | Rotate the virtual display |
| Scroll / trackpad | Zoom (virtual display height) |
| Double-click | Focus on the splat under the cursor (smooth pose transition) |
| `-` / `=` | Decrease / increase depth + IPD together (10 %–100 %) |
| `M` | Auto-orbit: slow turntable rotation when idle for > 10 s |
| `F` | Flip scene Y-axis (fix for splats trained in the opposite Y convention) |
| `V` | Cycle rendering modes advertised by the display runtime |
| `L` or top-bar **Open…** | Load a different `.ply` / `.spz` file |
| Drag-and-drop (macOS) | Load a `.ply` / `.spz` dropped onto the window |
| Space | Reset pose, zoom, depth, auto-orbit, flip |
| Tab | Toggle HUD |
| One-finger drag (Android) | Orbit the splat |
| Two-finger pinch (Android) | Zoom |
| Double-tap (Android) | Focus: recenter the orbit on the tapped point |
| Long-press (Android) | Reset orbit / zoom / focus |
| Ctrl+T | Toggle transparent background (desktop see-through; Windows only, requires runtime ≥ v1.3.0) |
| Esc | Quit |

## Agent tools (MCP)

When the runtime's MCP capability is enabled (`DISPLAYXR_MCP=1` or the
Capabilities registry key), the viewer registers its controls as agent
tools on the runtime-hosted per-process MCP server via
`XR_EXT_mcp_tools` (macOS build; appId `gaussiansplat`):
`load_splat`, `get_status`, `set_camera`, `orbit`, `reset_camera`,
`set_auto_orbit`. Agents reach them through the `displayxr-mcp` adapter
(`--target pid:<PID>`, or namespaced as `gaussiansplat__<tool>` via
`--target workspace`) and can verify camera moves with the runtime's
`capture_frame` tool. With the gate off (the default) the tools are
absent and the viewer behaves identically.

## Build from source

### Prerequisites (both platforms)
- CMake ≥ 3.21 + Ninja
- [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) (includes `glslangValidator`)
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (find_package-visible)
- A DisplayXR-compatible runtime (install via `DisplayXRSetup-*.exe` from [displayxr-runtime releases](https://github.com/DisplayXR/displayxr-runtime/releases) for the real 3D display path; the demo also runs against the null compositor with `SIM_DISPLAY_OUTPUT=anaglyph`)

### macOS
```bash
brew install cmake ninja vulkan-sdk openxr-loader
./scripts/build_macos.sh
# Run
./build/macos/gaussian_splatting_handle_vk_macos
```

### Windows
```bat
REM Set OpenXR_ROOT to your OpenXR SDK install if find_package can't see it.
scripts\build_windows.bat
REM Run
build\windows\Release\gaussian_splatting_handle_vk_win.exe
```

## Repo layout

```
.
├── macos/                  Platform-specific entry + window handling
├── windows/                Platform-specific entry + window handling
├── 3dgs_common/            Vulkan compute pipeline, PLY + SPZ loaders
├── common/                 Shared helpers: Kooima math, input, HUD
├── openxr_includes/        Vendored OpenXR headers (incl. DisplayXR extensions)
└── scripts/                Build scripts for each platform
```

The `common/` and `openxr_includes/` directories are also used by other DisplayXR demos — content is synchronized from the DisplayXR runtime source tree on each release tag.

## License

Apache-2.0 — see `LICENSE`. (Vendored OpenXR extension headers under
`openxr_includes/` remain BSL-1.0 — see their SPDX headers.)
