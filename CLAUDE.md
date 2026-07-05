# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working on this repo.

## Project Overview

DisplayXR Demo — Gaussian Splatting. Real-time 3D Gaussian Splatting viewer for glasses-free 3D displays, built on the DisplayXR OpenXR runtime via Vulkan. Loads `.spz` and `.ply` files, renders with asymmetric per-eye Kooima projection.

This is a **standalone repo**. It evolves independently — there is no source-mirror from the runtime. Edit code here directly; cut your own release tags here. The `common/` and `openxr_includes/` directories were originally seeded from the runtime tree but are now maintained in this repo.

## Runtime dependency

Requires the **DisplayXR runtime v1.3.0 or newer**. v1.3.0 ships the Vulkan transparent-window bridge (PR #215) that this demo now relies on unconditionally — the HWND is created with `WS_EX_NOREDIRECTIONBITMAP` and the session with `transparentBackgroundEnabled = XR_TRUE`, so older runtimes will produce a broken/black window. (v1.1.0+ is still required for the `XR_EXT_display_info` v12 rendering-mode fields the demo also queries.)

Install via `DisplayXRSetup-*.exe` from the [`displayxr-runtime` releases page](https://github.com/DisplayXR/displayxr-runtime/releases). The dev-orchestrator (`scripts/setup-displayxr.{sh,bat}`) and the meta-installer bundle ([`displayxr-installer`](https://github.com/DisplayXR/displayxr-installer)) both pin a known-good runtime version via `versions.json` — those installs are the recommended path for users. The shell ([`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases)) is **optional** for this demo — only needed for the spatial workspace shell, which this demo does not use.

**F11 fullscreen requires post-v1.3.2 runtime** (fixed in [displayxr-runtime#236](https://github.com/DisplayXR/displayxr-runtime/pull/236), merged 2026-05-17, shipping in v1.4.0+). On v1.3.2 and earlier, F11 fullscreens to the top-left corner then crashes inside the Vulkan ICD: the per-app VK native compositor's DComp transparent-window bridge had no resize path, so `comp_vk_native_target_resize` freed the aliased DComp views and then called `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` with a `VK_NULL_HANDLE` surface. The shell-launched path (out-of-process `d3d11_service` compositor) is unaffected — F11 always worked there because shell-mode bypasses the per-app VK compositor entirely.

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

### Linux (build-green only — issue #60, M8 Linux epic)
```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libvulkan-dev vulkan-validationlayers glslang-tools zlib1g-dev \
  libx11-dev libxcb1-dev libx11-xcb-dev libxxf86vm-dev libxcb-glx0-dev
./scripts/build_linux.sh
```
Outputs to `build/linux/gaussian_splatting_handle_vk_linux` plus
`build/run_gaussiansplat_linux.sh`. Uses system Vulkan (no MoltenVK) and a
from-source OpenXR loader pinned to `1.1.43` (equal to `.github/workflows/build-linux.yml`).
The Linux leg (`linux/main.cpp`) is a **hosted-NULL harness**: it passes no
window binding, so the runtime self-creates a window. Faithful app-owned
windowing/input and on-screen validation (needs the Linux runtime + a GPU + an
X server) is a later pass — see the `TODO(Phase 3)` in `linux/main.cpp` for
wiring `XR_EXT_xlib_window_binding` (runtime Phase 3a). The generic **COMPUTE**
splat renderer (`GsRenderer`) is used on Linux desktop; the Adreno/TBDR
graphics path (`gs_adreno_renderer`) is Android/Apple-Silicon only. Recipe:
the runtime repo's `docs/guides/linux-demo-port.md`.

## Input handling

Existing keyboard shortcuts are dispatched in `windows/main.cpp::WindowProc` under `WM_KEYDOWN` (around line 355). New shortcuts go there. The README documents the full list (WASD, M, F, V, L, Space, Tab, Esc, etc.).

## OpenXR + Vulkan integration notes

- Uses `XR_KHR_vulkan_enable` (creates own VkInstance/Device).
- Uses `XR_EXT_win32_window_binding` for app-owned HWND.
- Uses `XR_EXT_display_info` (v12+) for display dims + rendering modes.
- Submits a single `XrCompositionLayerProjection` per frame.

The runtime's VK native compositor handles the rest (atlas → display processor → present). The demo doesn't need to know the chroma-key / weave / DComp internals — those happen runtime-side based on the `XR_EXT_win32_window_binding` flags the demo sets at session create.

## Projection / clip planes

Near/far are **ZDP-anchored and per-eye**. `display3d_compute_view`/`_views` in `common/display3d_view.c` take `(near_offset, far_offset)` — **absolute** offsets, in virtual-display-height (vH) units, from each eye's perpendicular distance to the convergence plane (the virtual display / zero-disparity plane), where `eye_scaled.z` (`ez`) is that distance. Internally `near = ez − near_offset` and `far = ez + far_offset`, clamped so `near ≥ 1e-4` and `far > near` (`display3d_compute_projection` still does the low-level Kooima matrix math from the resolved absolute near/far — unchanged). The offsets are expressed in vH so they're independent of scene scale; `ez` is still needed to position them per-eye. Call-site values: `near_offset = vH` everywhere; `far_offset = 1000·vH` in opaque mode (far effectively at infinity), and **transparent mode passes `far_offset = 0`** so the far plane sits exactly at the ZDP (foreground-only — content behind the display is clipped to avoid see-through artifacts). `vH = tunables.virtual_display_height` (already zoom/scale-adjusted at the call site). The pick path (`display3d_compute_center_view`) just needs a well-conditioned frustum — the unprojected ray is a full line — so it passes `(vH, 1000·vH)` regardless of mode.

> **History:** this was previously a *fraction*-of-`ez` convention (`clip_front`/`clip_back`, `near = ez·(1−f)`, `far = ez·(1+b)`, defaults 0.5/2.0). It moved to absolute vH offsets so the clip band no longer scales with `ez`. Do NOT reintroduce either the old fractions or fixed scene-absolute near/far (e.g. `0.01 / 100.0`) at the call sites.

**The projection near/far planes do NOT clip splats.** This is the single most important thing to know here. `3dgs_common/shaders/preprocess.comp` projects each splat center to clip space but only ever uses `ndc.xy` (tile binning) and `p_view.z` (sort depth) downstream — it **never tests `ndc.z`**. So the `near_offset`/`far_offset` above only shape the projection matrix's depth range/precision and FOV; they remove nothing. **All geometric clipping is done by explicit view-space `p_view.z` culls in the shader**, driven by separate uniforms (`clip_near`, `clip_far`). `display3d_compute_view` outputs the resolved planes as `Display3DView.near_z`/`far_z`, and the render call sites pass them into `GsRenderer::renderEye(..., clipNearViewSpace, clipFarViewSpace, clipFadeFrac)`. If you change `near_offset`/`far_offset` and see no clipping change, this is why — check that the resolved `near_z`/`far_z` are reaching `renderEye`.

**Soft near clip, hard far clip.** `clip_fade_frac` (default 0.15) applies **only to the near plane**: it turns the near hard cut into an opacity rolloff over a band of width `frac·clip_near`, so splat centers crossing the near plane fade `0→1` over `[clip_near, clip_near·(1+frac)]` rather than pop. `clip_fade_frac = 0` restores a hard near cut. **The far plane is always a hard cut** — splats past `clip_far` are culled outright, no fade band (intentional: the far plane sits at the ZDP in foreground mode and a fade there is unwanted). Either plane `= 0` disables that side. `clip_near` (= `ez − vH`) is active in all modes; `clip_far` (= ZDP) only in transparent/foreground mode (opaque passes `far_z = ez + 1000·vH`, effectively no far cull). The Leia SR weaver still hard-masks the *final composite* alpha to 0/1, so the near fade softens color/density at the near plane, not the outer transparent silhouette. (History: the far plane previously had a symmetric soft fade too; removed 2026-06 — soft near only.)

**Pick respects the same visibility window.** `GsRenderer::pickGaussian(..., viewMatrix, clipNear, clipFar)` rejects double-click/recenter candidates whose **view-space forward depth** `dot(center − eye, fwd)` (fwd = `−(V[2],V[6],V[10])`) falls outside `[clipNear, clipFar]` — the same window the renderer clips to — so splats clipped in front of the near plane (or behind the ZDP in foreground mode) can never be focused. It uses forward depth, not ray-parameter `t`, so it's correct for off-axis rays. A full ray miss returns `false` → no recenter. The call sites pass `centerView.near_z` and (transparent only) `centerView.eye_display.z` as the far plane.

## Transparency support

The DisplayXR runtime since **v1.3.0** (the [v1.3.0 release](https://github.com/DisplayXR/displayxr-runtime/releases/tag/v1.3.0) added it; any current runtime supports it) provides Vulkan transparent-window support via a VK→D3D11 KMT-shared-texture → DComp + flip-model swapchain bridge. App-side contract:

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

**Dev-build dependency rule (don't regress).** `scripts\build-with-deps.bat`
**auto-provisions the OpenXR loader**, pinned to the same spec rev as the vendored
`openxr_includes/` headers (`XR_CURRENT_API_VERSION`, currently 1.1.51) — never
hardcode an SDK path (`C:/dev/openxr_sdk`, `C:/VulkanSDK/<ver>`); Vulkan comes
from the `VULKAN_SDK` env. A fresh clone must build with only VS 2022 + Ninja +
the Vulkan SDK. Keep all three pins equal: CI (`build-windows.yml`) == dev script
== header rev. This is a **dev clone-and-build** concern only — the released
installer always provisioned the loader via CI and bundles `openxr_loader.dll`
next to the exe, so it was never affected. (Fixed in #46; the broken script was
inherited from the modelviewer bootstrap.)

## Releasing

Each demo cuts its own release tag (`vX.Y.Z`) on its own cadence. The
preferred path is the user-level `/dxr-release` skill — it detects
this repo, tags HEAD, watches CI, and reports the dispatched bump +
installer mirror outcome. Manual fallback: `git tag -a vX.Y.Z -m ... && git push origin vX.Y.Z`.

**CI runs on every PR + push to main, not just tags.** `build-windows.yml` and `build-macos.yml` trigger on `pull_request` + `push:main` (build-validation — they compile the app + installer on both platforms with a placeholder version, and publish nothing) as well as on `v*` tags (release: build + attach installers + dispatch the bump). So every PR is build-checked on both platforms before merge — keep both workflows green. The release-attach + `DispatchVersionsBump` steps stay gated on a `v*` ref.

**Automatic versions.json bump on tag push.** As of 2026-05, this
repo's `build-windows.yml` ends with a `DispatchVersionsBump` job
that fires a `repository_dispatch` at
`displayxr-runtime/versions-bump.yml` with `field: "gauss_demo"`.
The runtime side updates `versions.json[gauss_demo]` AND mirrors the
file to `displayxr-installer/main`, so the dev orchestrator and the
meta-installer bundle both pick up the new pin within ~30 s of
build completion — no manual PR to either repo.

Full spec:
[`displayxr-runtime/docs/specs/runtime/versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md).

## Testing

The dev machine has a Leia SR Windows display. After build, launch the produced `.exe` directly — no env var needed (the runtime is registry-resolved).

For autonomous transparent-window pixel inspection (when transparency lands): use `BitBlt` from the desktop window DC, NOT `PrintWindow`. PrintWindow returns opaque black for DComp-composed transparent windows.

## Testing the dev build inside the DisplayXR Shell launcher

Workspace controllers (the DisplayXR Shell, third-party verticals) discover apps via manifests under two registered-mode dirs (per `docs/specs/displayxr-app-manifest.md` in the runtime repo, §5):

```
%LOCALAPPDATA%\DisplayXR\apps\          ← per-user, wins precedence
%ProgramData%\DisplayXR\apps\           ← system-wide, written by this demo's installer
```

The production installer (`installer\DisplayXRGaussianSplatInstaller.nsi`) writes a system-wide manifest pointing at `C:\Program Files\DisplayXR\Demos\GaussianSplat\gaussian_splatting_handle_vk_win.exe` — i.e. the **installed** binary, not your local dev build.

To make the shell launcher route to your **dev build** without uninstalling/reinstalling:

```bat
scripts\build_windows.bat
scripts\dev_register.bat
```

`dev_register.bat` drops a `%LOCALAPPDATA%\DisplayXR\apps\gaussian_splatting-dev.displayxr.json` with `exe_path` pointing at this repo's `build\windows\gaussian_splatting_handle_vk_win.exe`. Per the spec's dedup rule, `%LOCALAPPDATA%` wins over `%ProgramData%`, so the shell tile launches the dev binary. Restart the shell (or toggle the workspace) to pick up the new tile.

To remove the dev override (shell falls back to the installed `%ProgramData%` entry):

```bat
scripts\dev_register.bat --unregister
```

The build script itself does NOT auto-register — registration is opt-in to keep the build hermetic.

## Sibling repos

| Repo | Purpose |
|---|---|
| [`displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime) | The runtime. Public. Releases ship `DisplayXRSetup-*.exe`. |
| [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases) | Shell installer releases (optional add-on, not required by this demo). |
| [`displayxr-unity`](https://github.com/DisplayXR/displayxr-unity) | Unity plugin (`com.displayxr.unity`). Not used by this demo. |
| `displayxr-demo-<name>` | Other standalone demos with independent evolution. |

## MCP atlas capture (agent-side debugging)

`.mcp.json` registers the `displayxr` MCP server — the DisplayXR MCP adapter
installed by `DisplayXRMCPSetup` (`HKLM\Software\DisplayXR\Capabilities\MCP`).
When that capability is installed, **every OpenXR app process hosts an
in-process MCP server**, so a running `gaussian_splatting_handle_vk_win` exposes:

- `capture_frame` — writes the composed atlas as
  `%TEMP%\displayxr-mcp-capture-<pid>-<frame>.png` and returns the path
  (modes: `post-compose` default, `projection-only`). Read the PNG to see
  exactly what the display processor receives, per tile.
- `diff_projection`, `get_kooima_params`, `get_submitted_projection`,
  `get_display_info`, `get_runtime_metrics`, `tail_log`.

Workflow:

1. **Launch the app first**, then start the Claude session — or run `/mcp` →
   reconnect `displayxr` after launching (the adapter binds at spawn time).
2. `--target auto` attaches shell → service → unique app PID. If more than
   one OpenXR app is running, pin it: change args to `--target pid:<pid>`.
3. Call `capture_frame`, then Read the returned PNG path.

Non-Windows: set `DISPLAYXR_MCP_ADAPTER` to the adapter's install path before
launching Claude (the `.mcp.json` default is the Windows path).
