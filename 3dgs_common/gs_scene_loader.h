// Copyright 2025, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  PLY scene loading utilities for 3DGS
 */

#pragma once

#include <string>
#include <cstdint>
#include <vector>

// GPU vertex layout matching the shader's Vertex struct (240 bytes).
// position(vec4) + scale_opacity(vec4) + rotation(vec4) + sh[48]
struct GsVertex {
    float position[4];       // xyz, w=1
    float scale_opacity[4];  // exp(sx), exp(sy), exp(sz), sigmoid(opacity)
    float rotation[4];       // normalized quaternion (w,x,y,z)
    float sh[48];            // spherical harmonics (interleaved RGB)
};
static_assert(sizeof(GsVertex) == 240, "GsVertex must be 240 bytes");

//! Which rig frames a scene. Lives here rather than in gs_camera_rig.h so the
//! loader can carry the file's own hint without depending on the rig module
//! (gs_camera_rig.h includes THIS header, not the other way round).
enum class GsRigKind { Display, Camera };

//! The recording camera a photo-lifted scene was predicted through.
//!
//! Filled from the OPTIONAL top-level `camera` block of a SOG `meta.json`
//! (see gs_sog_loader.h for the schema). `present == false` for every scene
//! that does not carry one — which is every `.ply`, every `.spz`, and every
//! `.sog` written before the block existed — and the viewer then frames the
//! scene with its usual DISPLAY rig (auto-fit the AABB to the virtual
//! display, orbit the display around the subject). Presence is the ONLY
//! signal: a scene that declares its capture camera is asking to be framed
//! through it ("the render IS the left photo"), i.e. the CAMERA rig.
//!
//! Intrinsics are in pixels of ONE EYE's image (`width` x `height`), in the
//! OpenCV convention the block names: +x right, +y down, +z forward, metres.
//! The loader stores them verbatim — it does NOT right them into the app's
//! canonical RUB (+x right, +y up, +z back), because the gaussians it returns
//! have already been righted and a camera at the origin looking down OpenCV
//! +z is, after that same half-turn about x, a camera at the origin looking
//! down RUB -z. So the rest camera is the identity pose in app space and only
//! the y/z SIGNS of anything derived from the intrinsics need flipping.
struct GsSceneCamera {
    bool present = false;

    //! Coordinate convention the block declared. Only "opencv" is understood;
    //! anything else is refused (present stays false) rather than guessed at.
    std::string convention;

    //! Rest pose of the capture camera, camera->world. Position in metres,
    //! rotation as xyzw. Identity for a single-camera lift (the splat origin
    //! IS the left capture camera), but carried so a multi-camera asset can
    //! place it.
    float restPosition[3] = {0.0f, 0.0f, 0.0f};
    float restRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    //! Pinhole intrinsics in EYE-image pixels. `cx` may be off-centre — the
    //! horizontal shift a stereo lift bakes in when it deconverges the pair.
    float fx = 0.0f, fy = 0.0f, cx = 0.0f, cy = 0.0f;
    int   width = 0, height = 0;

    //! Capture baseline, metres, when the block carried a `stereo` object.
    //! The second capture camera sits at +x * baselineM. Optional: a mono
    //! lift has no baseline and the viewer falls back to a nominal one.
    bool  hasStereo = false;
    float baselineM = 0.0f;

    // ── v2 additions. All optional; a v1 block leaves every one of them
    //    unset and behaves exactly as it did. ─────────────────────────────

    //! `camera.rig` — the file's own hint about which rig suits it. A
    //! photo-lifted scene that nevertheless wants the display rig (a product
    //! shot lifted from a photograph, say) can say so without the user
    //! passing a flag. Unset means "camera", since carrying the block at all
    //! is already that request.
    bool       hasRigHint = false;
    GsRigKind  rigHint    = GsRigKind::Camera;

    //! `camera.focus.point` — THE focus, in rest-camera space, in the block's
    //! OpenCV convention (+y down, +z forward). One point serves three roles
    //! that must never disagree: the orbit centre, the pivot plane that stays
    //! put under head motion, and the convergence depth. On BOTH rigs.
    //!
    //! The loader stores it verbatim; the rig module rights it into app space
    //! alongside the rest pose.
    bool  hasFocus = false;
    float focusPoint[3] = {0.0f, 0.0f, 0.0f};

    //! `camera.focus.source` — how the producer arrived at that point
    //! ("convergence", "manual", "auto"). Reported, never acted on: the
    //! viewer's behaviour must not depend on a producer's self-description.
    std::string focusSourceLabel;

    //! `camera.focus.{subject_m,near_m,far_m}` — informational scene facts.
    //! Not used to frame anything; they exist so a viewer can say something
    //! true about the scene's depth range without measuring it.
    bool  hasSubjectM = false; float subjectM = 0.0f;
    bool  hasNearM    = false; float nearM    = 0.0f;
    bool  hasFarM     = false; float farM     = 0.0f;

    //! `camera.dxr.{ipd_factor,parallax_factor}` — ABSOLUTE scalars the
    //! producer wants applied on top of the measured-IPD scaling, so an asset
    //! whose depth reads too strong can be calmed at source. Absolute, never
    //! normalised against convergence (see GsCameraRig::IpdScale). Default 1.
    bool  hasDxrIpd      = false; float dxrIpdFactor      = 1.0f;
    bool  hasDxrParallax = false; float dxrParallaxFactor = 1.0f;
};

// Parse a binary PLY file and return GPU-ready vertices.
// Applies: sigmoid(opacity), exp(scale), normalize(rotation), SH de-interleave.
// Returns true on success. On failure, vertices is empty.
bool ParsePlyFile(const std::string& path,
                  std::vector<GsVertex>& vertices);

// Validate that a file path points to a valid .ply Gaussian splatting scene.
// Returns true if the file exists and has a .ply extension.
bool ValidatePlyFile(const std::string& path);

// Validate that a file path points to a valid 3DGS scene file (.ply, .spz or
// .sog). Returns true if the file exists and has a supported extension.
bool ValidateSceneFile(const std::string& path);

// Extract the filename (without directory) from a full path.
std::string GetPlyFilename(const std::string& path);

// Get a human-readable size string (e.g., "12.3 MB") for the file.
std::string GetPlyFileSize(const std::string& path);
