# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working on this repo.

## Project Overview

DisplayXR Demo — Gaussian Splatting. Real-time 3D Gaussian Splatting viewer for glasses-free 3D displays, built on the DisplayXR OpenXR runtime via Vulkan. Loads `.spz` and `.ply` files, renders with asymmetric per-eye Kooima projection.

This is a **standalone repo**. It evolves independently — there is no source-mirror from the runtime. Edit code here directly; cut your own release tags here. The `common/` and `openxr_includes/` directories were originally seeded from the runtime tree but are now maintained in this repo.

## Runtime dependency

Requires the **DisplayXR runtime v1.3.0 or newer**. v1.3.0 ships the Vulkan transparent-window bridge (PR #215) that this demo now relies on unconditionally — the HWND is created with `WS_EX_NOREDIRECTIONBITMAP` and the session with `transparentBackgroundEnabled = XR_TRUE`, so older runtimes will produce a broken/black window. (v1.1.0+ is still required for the `XR_EXT_display_info` v12 rendering-mode fields the demo also queries.)

Install via `DisplayXRSetup-*.exe` from the [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases). Latest tag at the time of writing is `v1.3.0` (2026-05-09). The shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases)) is **optional** — only needed for the spatial workspace shell, which this demo does not use.

Apps load the runtime via the registry-resolved manifest (no `XR_RUNTIME_JSON` env var needed). On the dev machine, the installed runtime DLL lives at `C:\Program Files\DisplayXR\Runtime\DisplayXRClient.dll`.

## Repo layout

```
.
├── macos/                 Platform-specific entry + window handling (Cocoa/Metal/MoltenVK)
├── windows/               Platform-specific entry + window handling (Win32)
│   ├── main.cpp           HWND creation, WindowProc message pump
│   ├── xr_session.cpp     OpenXR session create, GraphicsBindingVulkan, win32_window_binding
│   └── displayxr/         Bundled runtime DLL artifacts at build time
├── 3dgs_common/           Vulkan compute pipeline, PLY + SPZ loaders, render pass
├── common/                Shared helpers: Kooima math, input, HUD
├── openxr_includes/       Vendored OpenXR + DisplayXR extension headers
├── installer/             Windows installer (NSIS or similar)
└── scripts/               Build scripts per platform
```

## Build commands

### Windows (preferred dev path)
```bat
scripts\build_windows.bat
```
Outputs to `build\windows\Release\gaussian_splatting_handle_vk_win.exe` plus bundled DLLs.

### macOS
```bash
brew install cmake ninja vulkan-sdk openxr-loader
./scripts/build_macos.sh
```
Outputs to `build/macos/gaussian_splatting_handle_vk_macos`.

## Input handling

Existing keyboard shortcuts are dispatched in `windows/main.cpp::WindowProc` under `WM_KEYDOWN` (around line 355). New shortcuts go there. The README documents the full list (WASD, M, F, V, L, Space, Tab, Esc, etc.).

## OpenXR + Vulkan integration notes

- Uses `XR_KHR_vulkan_enable` (creates own VkInstance/Device).
- Uses `XR_EXT_win32_window_binding` for app-owned HWND.
- Uses `XR_EXT_display_info` (v12+) for display dims + rendering modes.
- Submits a single `XrCompositionLayerProjection` per frame.

The runtime's VK native compositor handles the rest (atlas → display processor → present). The demo doesn't need to know the chroma-key / weave / DComp internals — those happen runtime-side based on the `XR_EXT_win32_window_binding` flags the demo sets at session create.

## Transparency support

The DisplayXR runtime **v1.3.0** (released 2026-05-09 — `DisplayXRSetup-1.3.0.801.exe` on the [runtime releases page](https://github.com/DisplayXR/displayxr-runtime/releases/tag/v1.3.0)) adds Vulkan transparent-window support via a VK→D3D11 KMT-shared-texture → DComp + flip-model swapchain bridge. App-side contract:

1. HWND created with `WS_EX_NOREDIRECTIONBITMAP` + null background brush.
2. `XrWin32WindowBindingCreateInfoEXT.transparentBackgroundEnabled = XR_TRUE` and `chromaKeyColor = 0` (DP picks default magenta) at session create.
3. Scene clears to `RGBA(0,0,0,0)` in regions that should be transparent.
4. Projection layer in `xrEndFrame` sets `layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`.

Reference Vulkan app with these patches: `cube_handle_vk_win` in the runtime repo (`test_apps/cube_handle_vk_win/{main.cpp, xr_session.cpp, vk_renderer.cpp}` under `displayxr-runtime`).

Anti-aliased edges become hard-mask alpha on Leia hardware (fundamental SR-weaver limitation — alpha is 0 or 1, no in-between).

## Coding conventions

- C++17 / Vulkan 1.0+, Objective-C++ on macOS.
- Naming: `lower_snake_case` for files/functions, `PascalCase` for C++ classes/structs.
- **Multiview-first language**: use `tile`, `view`, `atlas`. NEVER `stereo`, `left+right eye`, or `SBS` in new code, comments, docs, or chat.
- Logging via the demo's existing macros (see `common/logging.h` if present, or follow the pattern in surrounding code).

## Build quirks

The CMakeLists has historically broken on dev paths with spaces (e.g. `C:\Users\Sparks i7 3080\...`). Two specific fixes were applied previously; check `git log -- CMakeLists.txt 3dgs_common/CMakeLists.txt windows/CMakeLists.txt` for prior space-in-path patches before assuming the build works on a fresh clone in a quoted-path workspace. If `scripts\build_windows.bat` fails with a path-quoting error, that's the symptom.

## Releasing

Each demo cuts its own release tag (`vX.Y.Z`) on its own cadence. Manual flow:
1. Bump `installer/build-installer.bat` version (or equivalent).
2. Tag `vX.Y.Z` and push.
3. Run `installer\build-installer.bat` (or wait for CI if configured).
4. `gh release create` with the installer asset attached.

There is **no automated runtime-side trigger** — the demo's release cadence is independent of the runtime's.

## Testing

The dev machine has a Leia SR Windows display. After build, launch the produced `.exe` directly — no env var needed (the runtime is registry-resolved).

For autonomous transparent-window pixel inspection (when transparency lands): use `BitBlt` from the desktop window DC, NOT `PrintWindow`. PrintWindow returns opaque black for DComp-composed transparent windows.

## Sibling repos

| Repo | Purpose |
|---|---|
| [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime) | The runtime. Public. Releases ship `DisplayXRSetup-*.exe`. |
| [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases) | Shell installer releases (optional add-on, not required by this demo). |
| [`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity) | Unity plugin (`com.displayxr.unity`). Not used by this demo. |
| `displayxr-demo-<name>` | Other standalone demos with independent evolution. |
