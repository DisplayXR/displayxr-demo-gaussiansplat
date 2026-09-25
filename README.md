# DisplayXR Demo — Gaussian Splatting

Real-time 3D Gaussian Splatting viewer for glasses-free 3D displays, built on the DisplayXR runtime via OpenXR with Vulkan. Loads `.spz`, `.ply` and `.sog` files, renders with asymmetric per-eye Kooima projection for the full stereo/multiview experience.

Two framing models, chosen by scene type — an object-centric scene is fitted to the virtual display, and a scene lifted from a photograph is shown through the camera that took it. See [Two rigs](#two-rigs-display-and-camera).

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
| `Ctrl+O` or top-bar **Open…** | Load a different `.ply` / `.spz` / `.sog` file |
| Drag-and-drop (Windows, macOS) | Load a `.ply` / `.spz` / `.sog` dropped onto the window |
| Space | Reset pose, zoom, depth, auto-orbit, flip |
| Tab | Toggle HUD |
| One-finger drag (Android) | Orbit the splat |
| Two-finger pinch (Android) | Zoom |
| Double-tap (Android) | Focus: recenter the orbit on the tapped point |
| Long-press (Android) | Reset orbit / zoom / focus |
| Ctrl+T | Toggle transparent background (desktop see-through; Windows only, requires runtime ≥ v1.3.0) |
| Esc | Quit |

On the **camera rig** (below) some of these mean something else, because the
viewpoint is the photograph's and is not the user's to move: a left-click drag
turns the *scene* about the pivot instead of rotating the display, capped at a
15° comfort cone; the idle turntable is off; and Space returns to the capture
camera.

**Double-click focuses on both rigs**, and it is the same gesture meaning the
same thing — *look at that* — with a different consequence. The display rig
moves its orbit centre to the picked splat. The camera rig moves the **focus
point**, so the pivot plane and the convergence follow it there while the
camera stays exactly where it is. Both are eased (0.18 per frame, the gallery's
own constant): convergence is depth, and a jump in it reads as the whole scene
lurching toward the viewer. Space eases the focus back to whatever the
waterfall chose.

### Linux (X11 or native Wayland)

One binary runs on X11 (XWayland included) or native Wayland. The window is
displayxr-common's `displayxr::linux_window`, and the platform is chosen by
capability at startup with `--platform=x11|wayland|auto`. The default `auto`
picks native Wayland when the compositor is ready (fractional-scale +
viewporter + the window-geometry extension) and X11 otherwise.

The Linux leg carries the mouse, keyboard and mode-switch bindings above,
transcribed from the Windows handler so a gesture means the same thing on both
(`linux/main.cpp`'s `HandleWindowEvent` cites the `input_handler.cpp` line for
each one). Left-drag orbit works on **both** rigs — it used to be camera-rig only,
which meant the bundled `butterfly.spz` could not be turned at all.

Two Linux-only differences, and they exist for the weave rather than for taste:

**The window draws its own header bar, and you drag the bar to move it.** The
interlace phase is a function of the window's absolute position in physical
panel pixels, so a window-manager-owned drag re-lands the phase on an arbitrary
pixel every frame and the 3D shimmers. A mutter `_NET_WM_MOVERESIZE` grab
cannot be intercepted by the client, so the only cure is to take the drag away
from the WM: the app owns the move and asks the display processor, through
`xrWeaveSnapWindowRectDXR` (`XR_DXR_weave`), where the window may land. It then
goes exactly there — snapping, not correcting, so the woven pattern is
identical at every position the drag visits. On native Wayland the compositor
runs the drag, constrained to the same positions. The bar sits above the 3D
area, not over it, and is the stand-in for the title bar Windows gets from the
OS. `DXR_X11_WM_DECORATIONS=1` gives you a normal WM-decorated X11 window back
and forfeits the snap.

On a runtime without `XR_DXR_weave`, or a display processor with no lattice of
its own (`sim_display`), the call is absent or returns the target unchanged and
the drag is simply unsnapped — the app logs which of those happened once, at
startup, and never hard-requires the extension.

**Not yet on Linux:** the HUD and toast overlays, double-click focus, WASD/QE
fly, atlas capture (`I`), MCP agent tools, transparency (`Ctrl+T`) and
drag-and-drop. `Tab` and `F` therefore do nothing there.

## Two rigs: display and camera

The viewer supports **both** framing models, selected by scene type.

**Display rig** — object-centric scenes (the bundled `butterfly.spz`, product
and model turntables). Find the main object, centre the display on it and size
the display to frame it; orbit the display around it. See [the fit](#the-display-rig-fit).

**Camera rig** — open scenes lifted from a photograph (an ML-SHARP `.sog` from
the DisplayXR Gallery). The splat's origin **is** the left capture camera
(OpenCV convention: +x right, +y down, +z forward, metres) and the gaussians
were unprojected through a known focal, so the rest view conserves the
recording camera's intrinsics — *the render IS the left photo*. No auto-fit, no
turntable: a photo-lifted cloud has no support more than ~15° off the capture
axis, so spinning it shows floaters rather than parallax.

### The waterfall

Nothing here is a single switch — each of the three decisions is a list of
sources tried in order, and **the viewer logs and HUD-displays which level it
landed on**, because a wrong answer and a right one look identical until you
know which level produced it.

| decision | 1st | 2nd | 3rd | 4th | 5th |
|---|---|---|---|---|---|
| **rig** | `--rig=` | `camera.rig` | block present → camera | **photo-lift signature → camera** | → display |
| **intrinsics** | `camera.intrinsics` | `--fx --fy --cx --cy --size` | estimated from the cloud | 28 mm-eq at the measured aspect |
| **focus** | `--pivot=` | `camera.focus.point` | median disparity of the cloud | scene bounds, then 2 m |

Flags outrank the block field by field, so `--cx` alone is a correction to an
otherwise good camera rather than a request to discard it.

**A file with no block at all can still reach the camera rig.** A `.spz` or
`.ply` conversion of a gallery lift carries no metadata whatsoever, and framing
one as an object gives it a crop at the wrong field of view. So when nothing has
declared a rig, the viewer asks the cloud itself, and takes the camera rig only
if all three of these hold:

| | test | measured: photo lifts | measured: object scans |
|---|---|---|---|
| (a) | ≥ 99 % of gaussians in front of the origin | **100.0000 %** | 2.26 % · 70.00 % |
| (b) | the angular-extent focal is a plausible lens (14–85 mm-eq) | 21.0 mm ✓ | 15.7 mm ✓ · 28.0 mm ✗ |
| (c) | the origin lies ≥ 0.05 × the blob's extent outside it | **0.144** | 0.000 · 0.000 |

(a) is the decisive one and says what the other two only corroborate: a lift is
unprojected *forward* through one lens, so everything it contains is in front of
the camera, while a scan is captured from all around and much of it sits behind.
(c) says the same thing from the other side — a camera stands outside what it
photographed, whereas a scanned object surrounds the origin it was scanned
about. The viewer logs all three with their thresholds, and on a miss says which
one decided, because "framed as an object" and "framed as a photograph" look
identical from outside until you know.

`--rig=display` overrides it, and a scene that declares anything at all never
reaches this step.

**Intrinsics can be recovered from the cloud**, which is why a block with no
`intrinsics` is still a useful block. A photo-lifted cloud remembers its camera
whether or not anyone wrote it down: every gaussian was unprojected along a ray
through the lens, so the P1/P99 angular extent about the rest camera *is* the
frustum it was fitted in. On the gallery's harbour asset that recovers the
half-tangents to **+0.18 %** (21 mm-eq against a true 21 mm-eq) and renders to
within **0.1/255** of the declared camera. Percentiles rather than extremes,
because a lift always leaves strays far outside the frame and any one of them
would set the focal by itself; gated on the implied 35 mm-equivalent focal
landing in 14–85 mm, falling back to 28 mm-eq with the measured **aspect** kept,
since a cloud's shape says what orientation the photograph was even when its
tail ruins the scale.

One deliberate exception: the estimator's measured **asymmetry is discarded and
the principal point centred**. The asymmetry is real but it is not the lens — a
lifted cloud spans the union of what both cameras saw plus whatever the model
extrapolated, so its angular centre drifts off the optical axis in an unrelated
direction. Measured on the harbour asset, applying it costs about seven points
of grey MAE (32.5/29.3 against the photographs, versus 23.6/23.6 centred).

### The display-rig fit

Two questions, one answer each: **where** to put the display (the orbit centre,
which is also the pivot and the convergence — one point, as on the camera rig)
and **how big** to make it (`virtualDisplayHeight`).

**Where** is an opacity-weighted voxel flood-fill from the densest voxel: a
figure is a contiguous 3D blob, and the walls and floor around it are separated
by air the fill cannot cross, so it finds the subject rather than the room.
Floaters — training artefacts scattered far outside the model — are trimmed
first (`KAWS FAMILY.spz` throws ~3 % of its splats out to ±240 while the model
spans ±15). If the fill finds nothing, a [p5, p95] box is the documented
fallback.

The fill runs at a ladder of thresholds and has to decide which rung is the
subject. It stops on **mass**: the tightest blob that already holds a third of
the scene's opacity-weighted total, plus a knee guard that refuses a step
growing the blob's largest extent by more than 1.6×, which is the fill leaking
out of the object into whatever it is standing in.

That question is *scale-free*, and it has to be. The rule this replaced asked
what fraction of the **grid** a blob covered — so on a small statue in a big
museum room, where the grid spans 14.6 × 6.2 × 13.2 m, every rung that isolated
the statue covered under 0.2 % of the grid, all of them were rejected, and the
fallback returned the room. `DXR_FIT_LADDER=1` prints every rung with its mass
fraction and extent:

```
t=0.20   58 vox  21.2% mass  1.37 x 1.07 x 1.24
t=0.10  110 vox  29.9%       1.59 x 1.46 x 1.44
t=0.07  178 vox  35.9%       1.82 x 1.95 x 1.44   <- the statue, accepted
t=0.005                      9.78 x 3.90 x 7.63   <- the room
```

**This used to be implemented twice, differently**, and the two renderer legs
are chosen at compile time — so the same asset framed one way on Windows and
another on a Mac, a tablet or a Windows-on-ARM box. On `butterfly.spz` they
disagreed by **47 %** (1.979 vs 1.343 vH on the same viewport). There is now
one implementation, `3dgs_common/gs_scene_fit.{h,cpp}`, and every platform gets
the flood-fill answer. **Windows does not move; macOS and Android move to the
Windows framing.**

**How big** is the width-aware rule — the subject caps at `fill` of the
viewport on both axes. `butterfly.spz` lands at 1.979 vH in landscape,
`KAWS FAMILY.spz` at 2.354 (its statue is 1.95 m, so it fills 83 % of the
display height).

There is also a **depth budget**, which is **off by default**. The fit centre is
the ZDP, so content `d` off that plane is disparity on the panel; capping that
disparity bounds how deep a scene may be before it has to be pushed back. The
model is scale-free, and over budget it **grows `vHeight`; it never moves the
pivot**, because the pivot is the orbit centre and the convergence and those
three stay one point. It is opt-in (`--fit=depth`) until the comfort cap has
been judged on real 3D hardware — shipping it on would make every deep scene
frame smaller for a reason nobody has verified yet. The `Fit:` line prints what
it *would* ask for regardless, so the number stays visible while it is not
acting.

| flag | |
|---|---|
| `--fit=flood` | **default**: shared bounds, width-aware rule (KAWS 2.354) |
| `--fit=depth` | additionally let the disparity budget bound it (KAWS 3.173) |
| `--fit=legacy` | **this platform's old framing**, for regression checks — the graphics leg's raw [p5, p95], the compute leg's flood-fill |
| `--fit-disparity=<vH>` | the budget ceiling as a fraction of the display height, default `0.03`. The one knob that moves a depth-bound fit: `0.06` halves what it asks for. Only bites under `--fit=depth`. |

The fit does not run at all on the camera rig — a photograph is framed by its
own camera, and a flood-fill over an open scene finds no subject to frame
(on `ports.sog` it fills 18 of 262 144 voxels).

### How the rig reaches the runtime

The eyes belong to the **runtime** — head-tracked, correct for the panel — so
the app *declares* the rig rather than computing eye math. It chains an
`XR_DXR_view_rig` `XrCameraRigDXR` descriptor built from the photo's intrinsics
onto `xrLocateViews` and renders the `XrView{pose, fov}` that comes back. Eye
separation is the capture baseline in **absolute** world metres and is never
normalised against the convergence distance.

Two details worth knowing:

- **The descriptor cannot carry an off-centre principal point** — it has
  `verticalFov` and nothing else about the image plane. The `cx`/`cy` shift (the
  deconvergence a stereo lift bakes in) is applied app-side as a tangent-space
  offset on top of the returned FOV. It is exactly zero for a centred principal
  point. Raising a `principalPoint` field upstream is the real fix.
- **The rest view is anchored to the capture camera.** `XrCameraRigDXR` measures
  eye displacement from the *panel's* axis, and a panel's nominal viewer
  generally does not sit on it (this one is 10 cm above the panel centre, as a
  seated viewer is). At rest the rig therefore renders a viewpoint offset from
  the camera that took the photo. The viewer samples that untracked offset once
  and cancels it, so head motion is measured **from the photograph's viewpoint**.
  Motion itself is unaffected — it is a delta from that rest.

Where the canvas aspect differs from the photo's, the vertical FOV is narrowed
so the frame is a **crop** of the photo at its own angular scale, never an
overscan beyond the frustum the gaussians were fitted in. When the aspects
match, it is the photo's FOV exactly.

### The `camera` block

A photo-lifted `.sog` carries the intrinsics in an **optional** top-level
`camera` object in `meta.json`, a sibling of `count`, with `version` still `2`.
Readers that predate it ignore it, which is every reader shipping today:

```json
"camera": {
  "convention": "opencv",
  "rig": "camera",
  "rest": { "position": [0,0,0], "rotation": [0,0,0,1] },
  "intrinsics": { "fx": 1194.666, "fy": 1194.666, "cx": 1024.0, "cy": 576.0,
                  "width": 2048, "height": 1152 },
  "stereo": { "baseline_m": 0.063 },
  "focus": { "point": [0,0,1.683], "subject_m": 2.14, "near_m": 0.73,
             "far_m": 66.2, "source": "convergence" },
  "dxr": { "ipd_factor": 1.0, "parallax_factor": 1.0 }
}
```

`rotation` is xyzw, camera→world. Intrinsics are in pixels of **one eye's**
image (`width`×`height`); `cx` may be off-centre. `stereo` is optional — the
second capture camera sits at +x · `baseline_m`.

Everything except `convention` is optional, and a v1 block (the first four
keys) behaves exactly as it always did:

- **`rig`** — `"camera"` or `"display"`. A product shot lifted from a
  photograph is still a product shot, and this lets the producer say so without
  the user passing a flag.
- **`focus.point`** — **THE** focus, in rest-camera space. One point does three
  jobs, on both rigs: the orbit centre, the pivot plane that stays put under
  head motion, and the convergence depth. They are stored as one field because
  a viewer that orbits about one place and converges at another shows the user
  two different scenes depending on whether they are moving. `subject_m`,
  `near_m` and `far_m` alongside it are informational scene facts; `source`
  records how the producer chose the point and is reported, never acted on.
- **`dxr.ipd_factor` / `dxr.parallax_factor`** — absolute scalars applied on top
  of the measured-IPD scaling, so an asset whose depth reads too strong can be
  calmed at source. Absolute: never normalised against convergence.

A block whose `convention` is not `opencv`, or whose intrinsics are missing or
degenerate, is ignored with a warning and the scene falls back to the display
rig. Framing a scene through a camera that never existed is worse than not
framing it at all.

**Convergence is not in the block**, so the pivot — the plane that stays put
under head motion and the vertex of the comfort cone — is taken from `--pivot=`
if given, else from the cloud's own **median forward depth** (measured before
any decimation, so it does not move with a performance knob), else 2 m. The HUD
names which.

That omission has a visible consequence, and it is the single biggest
difference between this viewer's output and the source photographs. The
rendered pair is converged at the **pivot**; a raw capture is converged
wherever the camera was. On the gallery's `ports` asset the two photographs
carry +36.8 eye-px of mutual offset while the rendered pair carries +1.6, so
each rendered eye sits about 18 px from its photograph — which is most of the
23.6/255 grey MAE between them. Re-converging the render onto the pair (a 16 px
`--cx` nudge) halves that to 11.8. Neither number is a defect: they are two
different, both-correct convergences. But a viewer cannot reproduce the
capture's convergence from the block as specified, because the block does not
carry it — the gallery's own model uses `min(dConv, dSubject)` and only
`dSubject` survives the round trip. **Adding a convergence (or a
`stereo.convergence_m`) to the block is the fix**, and until then `--pivot=` is
the manual override.

A smaller, separate residual: this asset also wants its principal point about
9 px **below** the image centre — a shift that helps both eyes equally, unlike
the convergence one, which helps one and hurts the other. Worth about 2/255.

## Command line and the `displayxr-view:` protocol

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
| *(positional)* | Scene path, `.ply`, `.spz` or `.sog`. Replaces the bundled `butterfly.spz` auto-load. |
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

**On Linux** (`gaussian_splatting_handle_vk_linux`) `--transparent`, `--rect`, `--pose` and `--margin` (and `--vh`) follow
the same rules. `--rect` is the CONTENT rect in desktop device pixels: X root
coordinates on X11; on native Wayland the runtime's device convention (a
monitor's logical origin times its scale, plus the monitor-relative logical
offset times the same scale — what the window-geometry feed reports). It is
placed by displayxr-common's `DxrLinuxWindow::request_initial_rect`: on X11 the
window is created there; on Wayland, where a client cannot place itself, the
surface is sized at the target monitor's scale and moved through the
`window-geometry@displayxr.org` GNOME extension once its first frame is
presented. Expect up to 1 px of rounding at a fractional scale, or under
XWayland at a scaled desktop. A `--rect` window is always windowed, even at the
panel's size. `--src` URLs are not downloaded on Linux yet: pass a local path.

Flags are `--key=value`, never `--key value`. `--` ends flag parsing.

### Rig flags

These describe the **camera rig** (above) and are read on every desktop arm.
They let a scene be framed through its capture camera even when the file does
not carry a `camera` block — and they override it when it does.

| Flag | Meaning |
|---|---|
| `--rig=display\|camera` | Force a rig, whatever the file says. Default: `camera` if the scene declares a `camera` block, `display` otherwise. |
| `--fx=<px>` `--fy=<px>` | Focal length in pixels of one eye's image. Giving only one sets both (square pixels). |
| `--cx=<px>` `--cy=<px>` | Principal point in the same pixels. Default: centred. An off-centre `cx` is a stereo lift's deconvergence. |
| `--size=WxH` | Eye-image size in pixels, e.g. `--size=2048x1152`. Needed with `--fx` when the file has no block. |
| `--baseline=<m>` | Capture baseline in metres. Default 0.063. The second capture camera sits at +x · this. |
| `--pivot=<m>` | Focus depth in metres — the plane that stays put under head motion, the orbit centre and the convergence, which are one thing. Default: `camera.focus.point`, else the cloud's median disparity. |
| `--focus-weight=centre` | Take the median-disparity focus from the middle of the frame only. Right for a portrait, wrong for a landscape, so it is off by default. |
| `--mode=<index>` | Initial rendering mode — `0` is the runtime's 2D passthrough, `1` the first 3D mode. Makes a rest view reproducible from a script. |
| `--window=WxH` | Open the window at this size in points. The camera rig derives its horizontal FOV from the canvas aspect, so this is a real input to the framing — it is how a portrait asset gets checked. |
| `--fit=legacy\|flood\|depth` | Display-rig fit mode, default `flood`. See [the fit](#the-display-rig-fit). |
| `--fit-disparity=<vH>` | Depth-budget ceiling as a fraction of the display height, default `0.03`. |

Framing the DisplayXR Gallery's `ports` scene through its own camera:

```bash
gaussian_splatting_handle_vk_macos scene.sog \
  --rig=camera --fx=1194.665984 --fy=1194.665984 \
  --cx=1024 --cy=576 --size=2048x1152 --baseline=0.063
```

Once the `.sog` carries its `camera` block, all of that is just
`gaussian_splatting_handle_vk_macos scene.sog`.

**The macOS arm parses these flags and the shared ones above.** It previously
read `argv[1]` and nothing else; it now runs the same `--key=value` parser the
Windows arm does, so `--src=`, `--vh=`, a positional path and the protocol URL
mean there what they mean here. The Android arm has no command line: it is
driven entirely by the `camera` block.

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

The **download** path still admits only `.ply` and `.spz`: a `.sog` URL is
refused at the extension check rather than half-loaded. That is a deliberate
lag behind the loader, not an oversight — the protocol path is reachable from
any page on an origin the user once approved, so its allowlist is widened
separately from the local one. A local `.sog` (positional path, `--src=` with a
file path, the Open dialog, drag-and-drop) loads normally.

## Performance knobs (`DXR_GS_*`)

Every renderer-side performance lever is an **environment variable**, read once
when the renderer initialises, on all four platforms and on both splat
renderers (`GsRenderer`, the compute compositor used on x86 Windows, and
`GsAdrenoRenderer`, the graphics/TBDR path used on Linux, Android, Apple
Silicon and Windows-on-ARM). They are environment variables and not
command-line flags so they can never collide with the viewer's scene/URL
argument parsing.

> **Which renderer on Linux.** The Linux leg defaults to **GRAPHICS**
> (`GsAdrenoRenderer`), set by `GS_RENDERER` in `linux/CMakeLists.txt` — not
> COMPUTE. The desktop head-to-head showed the graphics path also wins on
> immediate-mode GPUs, because its radix sort sorts N gaussians rather than
> N x 8 fragments. Override at configure time with
> `-DGS_RENDERER=COMPUTE` (legacy) or `-DGS_RENDERER=AUTO`
> (`gs_renderer_select.h`'s own default, which on desktop x86_64 is COMPUTE).
> The `GsAdreno: knobs ...` vs `GsRenderer: knobs ...` startup log line tells
> you which one a given build actually linked.

On **Android** each one also falls back to its historical `debug.dxr.gs.*`
system property when the environment variable is unset, so
`adb shell setprop debug.dxr.gs.scale 0.6` keeps working exactly as before.

**Every optimisation defaults to the setting that does not change a pixel.**
The two enabled-by-default levers are lossless by construction — they only
remove work the fragment stage was already discarding — and the ones that can
change the image are off until you ask for them.

| Variable | Default | Lossy? | What it does |
|---|---|---|---|
| `DXR_GS_EXTENT` | `opacity` | no | Quad/tile extent is `sqrt(2·λ·ln(255·opacity))`, clamped to the historical 3σ — the radius beyond which the fragment shader's own `alpha < 1/255` test discards everything. Set to `3sigma` to restore the old formula. Bites on faint splats (`opacity < 0.353`); a dense opaque scene clamps to 3σ and is unaffected. |
| `DXR_GS_INVISIBLE_CULL` | `1` | no | Drop gaussians whose effective opacity is below `1/255`. `alpha ≤ opacity` at every pixel, so these can never pass the fragment test. `0` disables. |
| `DXR_GS_SCALE` | `1.0` | **yes** | Render the whole pipeline at `scale ×` the eye viewport, then upscale-blit. Range `(0.05, 1.0]`. Measured a *regression* on macOS/MoltenVK (the render pass still clears and stores the full-size attachment) — measure before trusting it on a new backend. |
| `DXR_GS_KEEP` | `1.0` | **yes** | Load-time decimation: keep this fraction of gaussians, hash-selected so the thinning is spatially uniform regardless of file order. |
| `DXR_GS_CULL_ALPHA` | `0` | **yes** | Load-time cull of gaussians below this opacity. Unlike `DXR_GS_INVISIBLE_CULL` this removes splats that *are* visible. |
| `DXR_GS_MAX_RADIUS_FRAC` | `0` (off) | **yes** | Cap the on-screen splat radius at this fraction of the render height, to bound the overdraw of sky-sized mega-splats. |
| `DXR_GS_COMPACT` | `0` (off) | no | Draw only the gaussians that survived preprocess, via `vkCmdDrawIndirect` (graphics path only; the compute path already compacts through its prefix sum). Lossless, but measured *slower* on macOS/MoltenVK and it removes almost nothing at a rest pose. |
| `DXR_GS_DUMP` | unset | no | Write the internal render target (pre-blit, pre-upscale) to this path as a PNG, once, then stop. For diffing two runs. |
| `DXR_GS_DUMP_FRAME` | `240` | no | Which eye `DXR_GS_DUMP` fires on. |

**Which lever actually moves a big scene.** Measured on a 1.18 M-gaussian
photo-lifted capture (macOS/MoltenVK, pinned pose, one eye): the splat draw is
**proportional to the gaussian count and independent of resolution** — 30.4 ms
at 1280x720 and 30.1 ms at 1920x1080, halving to 15.2 ms for half the
gaussians. Clamping every splat to a ~5x5 px quad removes only 8 %. So on a
large scene `DXR_GS_KEEP` is the only lever with real leverage; the
fragment-side ones (`DXR_GS_EXTENT`, `DXR_GS_MAX_RADIUS_FRAC`, `DXR_GS_SCALE`)
have almost nothing to take. Small scenes (~180 k) behave differently — there
the GPU is under-occupied and the per-gaussian cost hides.

Two more exist on the macOS build for benchmarking only: `DXR_GS_NOORBIT`
pins the camera (the 10 s idle turntable otherwise changes the workload
between samples) and `DXR_GS_WINDOW=WxH` sizes the window. The renderer logs
one `GsAdreno: knobs …` / `GsRenderer: knobs …` line per session with the
resolved values — a timing measurement without that line is not reproducible.
Per-stage GPU timings come from the throttled `GS_TS` line.

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
├── 3dgs_common/            Vulkan compute pipeline, PLY + SPZ + SOG loaders, camera rig
├── common/                 Shared helpers: Kooima math, input, HUD
├── openxr_includes/        Vendored OpenXR headers (incl. DisplayXR extensions)
└── scripts/                Build scripts for each platform
```

The `common/` and `openxr_includes/` directories are also used by other DisplayXR demos — content is synchronized from the DisplayXR runtime source tree on each release tag.

## License

Apache-2.0 — see `LICENSE`. (Vendored OpenXR extension headers under
`openxr_includes/` remain BSL-1.0 — see their SPDX headers.)
