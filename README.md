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
| `Ctrl+O` or top-bar **Open…** | Load a different `.ply` / `.spz` file |
| Drag-and-drop (Windows, macOS) | Load a `.ply` / `.spz` dropped onto the window |
| Space | Reset pose, zoom, depth, auto-orbit, flip |
| Tab | Toggle HUD |
| One-finger drag (Android) | Orbit the splat |
| Two-finger pinch (Android) | Zoom |
| Double-tap (Android) | Focus: recenter the orbit on the tapped point |
| Long-press (Android) | Reset orbit / zoom / focus |
| Ctrl+T | Toggle transparent background (desktop see-through; Windows only, requires runtime ≥ v1.3.0) |
| Esc | Quit |

## Command line and the `displayxr-view:` protocol (Windows)

A web page — or a native app such as a CAD tool "undocking" a part — can spawn
this viewer as a floating, transparent, click-through-shaped window over the
desktop, already showing a given scene at a given screen rect. There is no
Ctrl+T step and no style change after the window exists: `--transparent` makes
the window borderless and topmost **from creation**, which is what a shaped
`WS_EX_NOREDIRECTIONBITMAP` window requires.

```bat
gaussian_splatting_handle_vk_win.exe C:\scenes\butterfly.spz
gaussian_splatting_handle_vk_win.exe --transparent --rect=300,300,800,800 --vh=0.2 C:\scenes\butterfly.spz
gaussian_splatting_handle_vk_win.exe --transparent --src=https://host/scene.spz
```

| Flag | Meaning |
|---|---|
| *(positional)* | Scene path, `.ply` or `.spz`. Replaces the bundled `butterfly.spz` auto-load. |
| `--transparent` | Borderless + topmost + click-through-shaped from the first frame. Ctrl+T still toggles it later. |
| `--rect=X,Y,W,H` | Window rect in **physical** virtual-screen pixels. Clamped into the 3D panel's monitor when the runtime confirms which monitor that is; never snapped. |
| `--src=<url\|path>` | Scene to load. An `http(s)` URL is downloaded into the cache below; a local path loads directly. |
| `--vh=<metres>` | Virtual display height the scene was authored at. Overrides the auto-fit guess and does not follow viewport changes. |
| `--pose=YAW,PITCH[,ZOOM]` | Open on the side of the scene the sender was showing, instead of the loader's yaw-0 anchor. Degrees, in the web SDK's `setPose({yaw, pitch, zoom})` convention; `ZOOM` is a multiplier on the framed fit (default 1). Space returns here. |
| `--margin=<0..1>` | Fraction of the window the framed scene may fill, replacing the default 80% cap. `--vh` still wins over both. |
| `--title=<text>` | **Appended** to the window title; never replaces it. |
| `--type=model\|splat` | Routing hint. `model` is forwarded to the DisplayXR 3D Model Viewer. |
| `--dpr=<float>` | The launching page's `devicePixelRatio`. Logged only. |
| `--max-bytes=<n>` | Download cap. Default 256 MiB. |
| `--no-cache` | Re-download even on a cache hit (dev aid). |
| `--allow-local` | Native callers only: permit a `file:`/local `src` inside a protocol URL. A web page cannot set this — it is an argv flag, not a URL field. |

Flags are `--key=value`, never `--key value`. `--` ends flag parsing.

### Protocol URLs

The same fields arrive as a query string on the `displayxr-view:` scheme:

```
displayxr-view://open?src=<pct>&type=model|splat&rect=X,Y,W,H&vh=0.2&pose=-61,-3&margin=0.82&dpr=2.5&title=<pct>&transparent=1&v=1
```

`open` is the verb; `v=1` lets this viewer reject a future grammar loudly rather
than half-honour it. Every value is percent-encoded by the sender
(`encodeURIComponent`); only `%XX` is decoded — `+` is **not** a space. A
protocol launch is **transparent by default** (displayxr-common >= v2.9.1);
`transparent=0` opts out for a framed window. `--transparent` on the command
line stays opt-in.

The viewer registers `HKCU\Software\Classes\displayxr-view` for itself at
launch, not from the installer (the installer runs elevated, so its `HKCU`
writes would land in the elevating admin's hive). Every DisplayXR viewer
registers the same scheme so the browser only asks the user once; whichever one
the OS starts forwards a URL whose `type=` belongs to a sibling, looked up via
`HKLM\Software\DisplayXR\Demos\<Viewer>\InstallPath`. A second launch does not
open a second window — it hands its URL to the running instance over
`WM_COPYDATA`, which re-applies the rect, the vH, the pose, the margin and the
scene.

### Opening pose (`pose=`) and fit margin (`margin=`)

A page that undocks an asset it has already turned should not hand the user a
window showing a different side of it. `pose=` carries the page's own orbit and
`margin=` its fit, so the undocked window opens on the picture the user was
already looking at.

The two viewers rotate opposite things, so **the signs are mirrored** — the page
(`inline3d-viewer.js`) leaves its camera on +Z and rotates the *subject*
(`_pivot.rotation.set(pitch, yaw, 0, 'XYZ')`), while this demo leaves the scene
alone and orbits the *display rig* around it. Turning an object by `R` and
viewing it from a fixed camera `C` is the same picture as viewing the untouched
object from `Rᵀ·C`, which works out to

```
rig yaw   = −pose yaw
rig pitch = −pose pitch
scaleFactor = pose zoom          (bigger number = bigger subject, both sides)
```

Verified against `handbag_gen.spz` at the catalogue's `pose: {yaw: -61,
pitch: -3}`: the atlas capture reproduces the page's own thumbnail — front
panel and turn-lock toward the viewer, boxed side gusset on the left — while
`--pose=61,-3` shows the *back* of the bag with the gusset on the right, i.e.
the mapping is genuinely oriented rather than accidentally symmetric.

Both viewers upright the splats identically first (the page with
`mesh.quaternion.set(1,0,0,0)`, the SPZ loader with the source coordinate
system it honours since the v3/v4 fix), which is what makes a shared convention
possible at all.

`margin` is the fraction of the window the framed asset may fill — the same
knob as the built-in 80% cap, so `margin=0.8` and no `margin` mean the same
thing. `--vh` still overrides the fit entirely, and a viewport change still
re-derives the base without disturbing the pose or the zoom.

The idle turntable is held off while a launch pose is in effect and lifts on the
first mouse/keyboard input, so the opening pose is what the user actually sees
rather than something the screensaver has already spun away from.

### The policy, in two sentences

The browser's one-time "Open this app?" dialog is the only consent gate on the
protocol path and it is sticky per origin, so from then on **any** page on that
origin can drive this parser — which means the viewer has to be safe against a
hostile URL on its own merits. Therefore a protocol launch accepts `src` only
over `https:` (any host) or `http:` on loopback, refuses `file:`, UNC and bare
local paths outright, re-applies that same check to the URL a redirect chain
actually lands on, and bounds every length and rect; `--allow-local` lifts the
local-path restriction for native callers only, because a web page cannot pass
an argv flag.

A refused launch logs the reason, shows a message box and exits with code **2**
(**3** when the URL belongs to a sibling viewer that is not installed). Set
`DXR_LAUNCH_QUIET=1` in the process environment to suppress those launch-time
message boxes and rely on the log line plus the exit code — the modal is right
for a human-initiated launch but makes automated checks hang. Note that a
percent-encoded URL must **not** be passed through `cmd /c "set VAR=1 && app ..."`:
`cmd` expands `%XX%` inside it and hands the app a different string.

### Download cache

URL scenes land in `%LOCALAPPDATA%\DisplayXR\GaussianSplat\cache`, named by the
SHA-1 of the requested URL (never by anything in the URL's path, so a hostile
`src=.../../../x` has no traversal surface). A cache hit skips the network
entirely, which is what makes the second undock of the same scene instant.

Only `.ply` and `.spz` are loadable. **`.sog` is not supported** — a `.sog` URL
is refused at the extension check, not half-loaded.

## Agent tools (MCP)

When the runtime's MCP capability is enabled (`DISPLAYXR_MCP=1` or the
Capabilities registry key), the viewer registers its controls as agent
tools on the runtime-hosted per-process MCP server via
`XR_DXR_mcp_tools` (macOS build; appId `gaussiansplat`):
`load_splat`, `get_status`, `set_camera`, `orbit`, `reset_camera`,
`set_auto_orbit`, plus `set_transparent_background` on Windows (the agent
equivalent of Ctrl+T). Agents reach them through the `displayxr-mcp` adapter
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
