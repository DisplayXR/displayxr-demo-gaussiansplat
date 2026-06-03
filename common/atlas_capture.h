// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  App-side helpers for the runtime-owned atlas capture (the 'I' key).
 *
 * The GPU→CPU readback now lives in the runtime: the app calls
 * `xrCaptureAtlasEXT` (XR_EXT_atlas_capture) and the runtime writes the PNG
 * from the compositor's own atlas image — no app-side staging texture. What
 * remains app-side is purely UX: output-path / filename numbering and the
 * platform white-flash overlay. These live in `atlas_capture.cpp` (Windows)
 * and `atlas_capture_macos.mm` (macOS); apps link exactly one.
 *
 * Captures land in the user's Pictures folder and auto-increment as
 * `<stem>-<N>_<cols>x<rows>_atlas.png` (the runtime appends "_atlas.png" to
 * the prefix returned by MakeCaptureAtlasPrefix).
 */

#pragma once

#include <stdint.h>
#include <string>

#ifdef _WIN32
#include <windows.h>  // HWND for the flash overlay
#endif

namespace dxr_capture {

// ---------------------------------------------------------------------------
// Output path / filename helpers (cross-platform)
// ---------------------------------------------------------------------------

// Returns "<user pictures>/DisplayXR" (Windows: %USERPROFILE%\Pictures\DisplayXR;
// macOS: ~/Pictures/DisplayXR), creating it if missing. Empty on failure.
std::string PicturesDirectory();

// Scan `dir` for files matching "<stem>-<N>_<cols>x<rows>.png" and return
// max(N) + 1 (or 1 if there are no matches). Lets users accumulate captures
// without overwriting prior ones.
int NextCaptureNum(const std::string& dir,
                   const std::string& stem,
                   uint32_t cols,
                   uint32_t rows);

// Convenience: PicturesDirectory() + NextCaptureNum() + assemble full path.
// Falls back to working directory if Pictures resolution fails.
std::string MakeCapturePath(const std::string& stem,
                            uint32_t cols,
                            uint32_t rows);

// Path PREFIX (no extension) for xrCaptureAtlasEXT, which appends "_atlas.png".
// Numbers against existing "<stem>-<N>_<cols>x<rows>_atlas.png" files (the
// runtime-produced names) so repeat captures accumulate instead of overwriting.
// Returns "<dir>/<stem>-<N>_<cols>x<rows>" (no "_atlas", no ".png").
std::string MakeCaptureAtlasPrefix(const std::string& stem,
                                   uint32_t cols,
                                   uint32_t rows);

// ---------------------------------------------------------------------------
// Visual feedback — brief white flash overlay (~250 ms fade).
// ---------------------------------------------------------------------------

#ifdef _WIN32
// WM_TIMER ID used by the fade animation. App's WindowProc must dispatch
// `case WM_TIMER: if (wParam == kFlashTimerId) { TickCaptureFlash(hwnd); return 0; }`.
constexpr UINT_PTR kFlashTimerId = 0xDF1A5;

// Custom message ID for cross-thread flash request. Apps add a case in
// WindowProc that calls TriggerCaptureFlash(hwnd). Render thread fires
// PostFlashRequest(hwnd) — all HWND ops then run on the message-pump thread.
constexpr UINT kFlashUserMsg = WM_USER + 0x51;

// Show the white overlay over `parent`'s client area and start the fade
// timer. MUST run on the message-pump thread that owns `parent`. From the
// render thread, post a kFlashUserMsg to the window and call this from
// WindowProc instead.
void TriggerCaptureFlash(HWND parent);

// Tick the fade. Call from WM_TIMER when wParam == kFlashTimerId.
void TickCaptureFlash(HWND parent);

// Convenience wrapper for the cross-thread post.
inline void PostFlashRequest(HWND hwnd) {
    PostMessageW(hwnd, kFlashUserMsg, 0, 0);
}
#endif

#ifdef __APPLE__
// macOS: pass an `NSView*` (the content view that should be flashed). Safe
// to call from any thread — internally dispatches to the main queue, where
// AppKit / Core Animation must be touched.
void TriggerCaptureFlash(void* nsviewBridged);
#endif

}  // namespace dxr_capture
