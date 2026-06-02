# TEMPORARY — Windows / Leia verification for `feat/zdp-clip-soft-fade-pick`

> **Delete this file before merging.** It exists only to hand off hardware
> verification of the clip/pick rework to a Windows agent on the Leia SR display.
> All of this was verified on macOS (sim display) EXCEPT the transparent-mode
> paths, which macOS cannot exercise (no SR-weaver / DComp compositing).

## What changed (context)

The 3DGS rasterizer never tests `ndc.z`, so the projection near/far planes clip
nothing — only explicit view-space `p_view.z` culls do. This branch:
- Moves clip planes to vH-anchored absolute offsets: `near = ez − near_offset`,
  `far = ez + far_offset` (`near_offset = vH`; `far_offset = 1000·vH` opaque,
  `0` transparent → far at the ZDP).
- Adds a real view-space **near** cull (`clip_near`) + keeps the **far** cull.
- **Soft fade**: `clip_fade_frac = 0.15` fades opacity symmetrically over a band
  `frac·plane` at each side instead of a hard cut.
- **Pick**: `pickGaussian` rejects double-click/recenter candidates outside the
  rendered `[clipNear, clipFar]` window (view-space forward depth).

Key files: `3dgs_common/shaders/preprocess.comp`, `3dgs_common/gs_renderer.{h,cpp}`,
`common/display3d_view.{c,h}`, `windows/main.cpp`.

## Build & run

```bat
scripts\build_windows.bat
build\windows\Release\gaussian_splatting_handle_vk_win.exe windows\assets\butterfly.spz
```

Also test with KAWS (a denser scene with foreground pop-out):
`"%USERPROFILE%\Downloads\KAWS FAMILY.spz"` if present, else any .spz/.ply.

Requires DisplayXR runtime ≥ v1.3.0 (transparent-window bridge). Do NOT use F11
unless on a post-v1.3.2 runtime (see CLAUDE.md F11 caveat).

## Checklist

### 1. Opaque mode (default, no transparency)
- [ ] Scene renders normally; no missing/over-clipped content at normal zoom.
- [ ] Zoom in/out (mouse wheel): the near clip band scales with the virtual
      display — large scenes should NOT clip just from zooming.
- [ ] Near soft fade: orbit so splats pass through the near plane (`ez − vH`,
      i.e. ~one display-height in front of the ZDP). They should **fade in**,
      not pop. No hard banding edge.

### 2. Transparent / foreground mode  ← **the macOS-untested paths**
Toggle transparent background with **Ctrl+T** (runtime ≥ v1.3.0).
- [ ] Background goes see-through (desktop visible behind splats).
- [ ] **Far cull at ZDP**: content *behind* the display plane is hidden
      (foreground-only). Splats crossing the ZDP from front→back should **fade
      out** over the last 15% of `ez` (soft), not pop.
- [ ] Toggle Ctrl+T back to opaque: far content reappears; no crash/flicker.
- [ ] (If reproducible) the soft far fade affects color/density; the outer
      transparent silhouette stays hard-masked (expected SR-weaver behavior).

### 3. Double-click focus / recenter (pick visibility)
- [ ] Double-click a **visible** splat → display smoothly recenters on it.
- [ ] Double-click a **near floater** (splat between you and the near plane,
      i.e. clipped from the render) → **no recenter** (it's correctly ineligible).
- [ ] Double-click **empty space** (ray misses) → **no recenter**, no jump.
- [ ] In **transparent mode**, double-click a position whose only splats are
      *behind* the ZDP → **no recenter** (far reject active only in transparent).
- [ ] Recenter target looks correct (focuses the thing actually under the cursor,
      not a hidden/closer splat).

### 4. Regression sanity
- [ ] No new validation errors / device-lost in the console.
- [ ] Multi-view (Quad) mode if available: clipping + pick still correct per tile.
- [ ] Mono mode (M) and view toggles behave.

## If something is wrong

- **No clipping change when expected** → confirm `near_z`/`far_z` reach
  `renderEye` (the projection planes alone clip nothing; see CLAUDE.md
  "Projection / clip planes").
- **Everything clipped / black** → likely a sign/units mismatch between
  `eye_display.z` and the shader's `p_view.z`; instrument `clip_near` vs
  `p_view.z` in `preprocess.comp`.
- **Pick rejects visible splats** → the forward axis `−(V[2],V[6],V[10])` or the
  `clipNear/clipFar` values passed at `windows/main.cpp` are off.

Report findings back; then this file gets deleted and the branch can merge.
