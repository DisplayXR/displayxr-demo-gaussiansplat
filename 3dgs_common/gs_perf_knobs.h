// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  `DXR_GS_*` performance knobs, shared by both splat renderers.
 *
 * Every rendering optimisation in this repo carries a KILL SWITCH and every
 * one of them has to be reachable on all four legs (Windows, macOS, Linux,
 * Android) without a CLI flag — the demo's `main.*` argv parsing is owned by
 * the scene-loading side and a perf lever has no business colliding with it.
 * So the levers are read from the ENVIRONMENT, once, inside the renderer that
 * owns them.
 *
 * Android keeps its historical `debug.dxr.gs.*` system properties working:
 * every lookup checks the env var FIRST and falls back to the property, so an
 * existing `adb shell setprop debug.dxr.gs.scale 0.6` behaves exactly as
 * before while `DXR_GS_SCALE=0.6` now works everywhere.
 *
 * The full table (var, default, lossy?, effect) lives in README.md §
 * "Performance knobs (DXR_GS_*)" — keep the two in sync.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace gsperf {

//! Copy the value of `envName` (or, on Android, of `androidProp` when the env
//! var is unset/empty) into `out`. Returns false and leaves `out` untouched
//! when neither is set. `androidProp` may be null.
bool lookup(const char *envName, const char *androidProp, char *out, size_t outSize);

//! Numeric knob. Returns `def` when unset or unparseable.
float getFloat(const char *envName, const char *androidProp, float def);

//! Flag knob. "0", "off", "false", "no" (case-insensitive) are false; any
//! other non-empty value is true. Returns `def` when unset.
bool getBool(const char *envName, const char *androidProp, bool def);

//! True when the value is set AND case-insensitively equals `match`.
bool equals(const char *envName, const char *androidProp, const char *match);

//! Write an 8-bit RGBA image as a PNG (zlib-deflated, one IDAT). Used only by
//! the one-shot `DXR_GS_DUMP` capture — never on a hot path. Returns false on
//! any I/O or compression failure.
bool writePng(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height);

// ── Preprocess push-constant flag bits ───────────────────────────────────
// Mirrored by `flags` in preprocess.comp / adreno_preprocess.comp. Keeping
// them as runtime bits (not #defines baked into the SPIR-V) is what makes
// every lever a live kill switch rather than a build-time choice.
//! Quad extent = the radius at which the fragment shader's own `alpha < 1/255`
//! test starts discarding, clamped to the historical 3σ. Clear → plain 3σ.
constexpr uint32_t kFlagOpacityExtent = 1u;
//! Drop gaussians whose effective opacity is below 1/255 — they can never pass
//! the fragment test at any pixel. Clear → keep them (historical).
constexpr uint32_t kFlagInvisibleCull = 2u;

//! One resolved set of `DXR_GS_*` levers. Read once per renderer `init()`.
struct Knobs {
    float renderScale = 1.0f;       //!< DXR_GS_SCALE        / debug.dxr.gs.scale
    float keepFrac = 1.0f;          //!< DXR_GS_KEEP         / debug.dxr.gs.keep
    float cullAlpha = 0.0f;         //!< DXR_GS_CULL_ALPHA   (lossy, default off)
    bool opacityExtent = true;      //!< DXR_GS_EXTENT=3sigma disables
    bool invisibleCull = true;      //!< DXR_GS_INVISIBLE_CULL=0 disables
    bool compactDraw = true;        //!< DXR_GS_COMPACT=0 disables (graphics path)
    float maxRadiusFrac = 0.0f;     //!< DXR_GS_MAX_RADIUS_FRAC (lossy, default off)
    char dumpPath[512] = {0};       //!< DXR_GS_DUMP=<file.png>, empty = no dump
    unsigned long long dumpFrame = 240;  //!< DXR_GS_DUMP_FRAME (eye counter)
};

//! Resolve every knob. `defaultRenderScale`/`defaultKeepFrac` are the platform
//! defaults the caller would otherwise have used, so an unset env var never
//! changes behaviour.
Knobs load(float defaultRenderScale, float defaultKeepFrac);

//! `flags` word for the preprocess push constant, from a resolved Knobs.
inline uint32_t preprocFlags(const Knobs &k)
{
    return (k.opacityExtent ? kFlagOpacityExtent : 0u) |
           (k.invisibleCull ? kFlagInvisibleCull : 0u);
}

}  // namespace gsperf
