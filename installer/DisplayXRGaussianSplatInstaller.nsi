; DisplayXR Gaussian Splat Demo — Windows Installer
; Copyright 2026, DisplayXR
; SPDX-License-Identifier: BSL-1.0
;
; Build: makensis /DVERSION=1.2.0 /DBIN_DIR=<demo-build-dir> /DSOURCE_DIR=<demo-repo-root> /DOUTPUT_DIR=<output-dir> DisplayXRGaussianSplatInstaller.nsi
;
; Hard-prereqs the DisplayXR runtime (HKLM\Software\DisplayXR\Runtime\InstallPath).
; Installs the demo exe + bundled scene to Program Files\DisplayXR\Demos\GaussianSplat\.
; Drops a registered-mode app manifest + icons under %ProgramData%\DisplayXR\apps\
; so the DisplayXR Shell launcher discovers the tile (system-wide, since the
; installer runs elevated). See docs/specs/displayxr-app-manifest.md.

!ifndef VERSION
    !define VERSION "1.0.0"
!endif
!ifndef VERSION_MAJOR
    !define VERSION_MAJOR "1"
!endif
!ifndef VERSION_MINOR
    !define VERSION_MINOR "0"
!endif
!ifndef VERSION_PATCH
    !define VERSION_PATCH "0"
!endif

!ifndef BIN_DIR
    !define BIN_DIR "${__FILEDIR__}\..\build\windows"
!endif
!ifndef SOURCE_DIR
    !define SOURCE_DIR "${__FILEDIR__}\.."
!endif
!ifndef OUTPUT_DIR
    !define OUTPUT_DIR "${__FILEDIR__}"
!endif

;--------------------------------
; General

Name "DisplayXR Gaussian Splat Demo ${VERSION}"
OutFile "${OUTPUT_DIR}\DisplayXRGaussianSplatSetup-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\DisplayXR\Demos\GaussianSplat"
InstallDirRegKey HKLM "Software\DisplayXR\Demos\GaussianSplat" "InstallPath"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!insertmacro VersionCompare

; Minimum runtime version that ships SimulatedRealityVulkanBeta.dll alongside
; the runtime DLLs (added in runtime v1.2.0). The demo's Vulkan compositor
; path delay-loads the SR weaver, so older runtime installs would crash with
; STATUS_DLL_NOT_FOUND on session create.
!define MIN_RUNTIME_VERSION "1.2.0"

;--------------------------------
; UI

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "DisplayXR Gaussian Splat Demo Setup"
!define MUI_WELCOMEPAGE_TEXT "This will install the Gaussian Splat reference demo for the DisplayXR runtime.$\r$\n$\r$\nThe DisplayXR runtime must be installed first."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Pre-flight: hard-prereq the runtime

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "DisplayXR requires 64-bit Windows."
        Abort
    ${EndIf}

    ; HKLM\Software\DisplayXR\Runtime\InstallPath is set by the runtime
    ; installer's "DisplayXR Runtime" section. NSIS is a 32-bit executable so
    ; HKLM access is silently redirected through WOW6432Node by default; the
    ; runtime writes to the 64-bit view. Switch to 64-bit view to match.
    SetRegView 64
    ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
    ReadRegStr $1 HKLM "Software\DisplayXR\Runtime" "Version"
    SetRegView 32
    ${If} $0 == ""
        MessageBox MB_ICONSTOP "DisplayXR runtime is not installed.$\r$\n$\r$\nInstall the DisplayXR runtime first, then re-run this installer.$\r$\n$\r$\nGet it from:$\r$\nhttps://github.com/DisplayXR/displayxr-runtime/releases"
        Abort
    ${EndIf}

    ; Older runtimes don't bundle SimulatedRealityVulkanBeta.dll on PATH,
    ; so the demo would crash at session create. Enforce a minimum.
    ${VersionCompare} "$1" "${MIN_RUNTIME_VERSION}" $2
    ${If} $2 == 2
        MessageBox MB_ICONSTOP "DisplayXR runtime $1 is too old.$\r$\n$\r$\nThis demo requires runtime ${MIN_RUNTIME_VERSION} or later.$\r$\n$\r$\nUpdate from:$\r$\nhttps://github.com/DisplayXR/displayxr-runtime/releases"
        Abort
    ${EndIf}
FunctionEnd

;--------------------------------
; Install

Section "Gaussian Splat Demo" SecDemo
    SectionIn RO

    ; Match the runtime installer's 64-bit registry view so HKLM keys land
    ; in the canonical (non-WOW6432Node) hive.
    SetRegView 64

    ; All-users context — $APPDATA -> %ProgramData%, $SMPROGRAMS -> All Users.
    SetShellVarContext all

    ; Kill any running instance so we can overwrite the exe.
    nsExec::ExecToLog 'taskkill /f /im gaussian_splatting_handle_vk_win.exe'
    Pop $0

    SetOutPath "$INSTDIR"
    File "${BIN_DIR}\gaussian_splatting_handle_vk_win.exe"
    File "${BIN_DIR}\butterfly.spz"

    ; Drop the registered-mode app manifest + icons under %ProgramData%
    ; (system-wide, installer-elevated — matches §2.2 of the manifest spec).
    ; The shell launcher scans %ProgramData%\DisplayXR\apps\ on every workspace
    ; activate and picks up the tile.
    CreateDirectory "$APPDATA\DisplayXR\apps"
    SetOutPath "$APPDATA\DisplayXR\apps"

    ; Manifest + icons live together; icon paths inside the manifest are
    ; resolved relative to the manifest file (per spec §2.3).
    File "${SOURCE_DIR}\windows\displayxr\icon.png"
    File "${SOURCE_DIR}\windows\displayxr\icon_sbs.png"

    ; Generate the manifest with an absolute exe_path pointing at the
    ; install dir we just populated. We can't reuse the in-tree sidecar
    ; (which omits exe_path) because that's sidecar-mode; here we need
    ; registered-mode.
    FileOpen $0 "$APPDATA\DisplayXR\apps\gaussian_splatting.displayxr.json" w
    FileWrite $0 '{$\r$\n'
    FileWrite $0 '  "schema_version": 1,$\r$\n'
    FileWrite $0 '  "name": "Gaussian Splat Viewer",$\r$\n'
    FileWrite $0 '  "type": "3d",$\r$\n'
    FileWrite $0 '  "category": "demo",$\r$\n'
    FileWrite $0 '  "display_mode": "auto",$\r$\n'
    FileWrite $0 '  "description": "Interactive viewer for 3D Gaussian Splatting scenes (.spz / .ply). Drag-and-drop a scene or press L to load. Bundled with a butterfly demo scene.",$\r$\n'
    FileWrite $0 '  "icon": "icon.png",$\r$\n'
    FileWrite $0 '  "icon_3d": "icon_sbs.png",$\r$\n'
    FileWrite $0 '  "icon_3d_layout": "sbs-lr",$\r$\n'
    ; Use forward slashes in exe_path so the JSON parses with any strict
    ; library — the manifest spec accepts either separator and normalizes.
    ${WordReplace} "$INSTDIR" "\" "/" "+" $1
    FileWrite $0 '  "exe_path": "$1/gaussian_splatting_handle_vk_win.exe"$\r$\n'
    FileWrite $0 '}$\r$\n'
    FileClose $0

    ; Registry breadcrumbs.
    SetRegView 64
    WriteRegStr HKLM "Software\DisplayXR\Demos\GaussianSplat" "InstallPath" "$INSTDIR"
    WriteRegStr HKLM "Software\DisplayXR\Demos\GaussianSplat" "Version" "${VERSION}"

    ; Add/Remove Programs entry.
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "DisplayName" "DisplayXR Gaussian Splat Demo"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "DisplayIcon" "$INSTDIR\gaussian_splatting_handle_vk_win.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "Publisher" "DisplayXR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "DisplayVersion" "${VERSION}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "VersionMajor" ${VERSION_MAJOR}
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "VersionMinor" ${VERSION_MINOR}
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat" \
        "EstimatedSize" "$0"
SectionEnd

Section "Start Menu Shortcut" SecShortcut
    SetShellVarContext all
    CreateDirectory "$SMPROGRAMS\DisplayXR"
    CreateShortCut "$SMPROGRAMS\DisplayXR\Gaussian Splat Viewer.lnk" \
        "$INSTDIR\gaussian_splatting_handle_vk_win.exe" "" \
        "$INSTDIR\gaussian_splatting_handle_vk_win.exe" 0
SectionEnd

;--------------------------------
; Uninstall

Section "Uninstall"
    SetRegView 64
    SetShellVarContext all

    nsExec::ExecToLog 'taskkill /f /im gaussian_splatting_handle_vk_win.exe'
    Pop $0

    ; Remove the registered-mode manifest + icons.
    Delete "$APPDATA\DisplayXR\apps\gaussian_splatting.displayxr.json"
    Delete "$APPDATA\DisplayXR\apps\icon.png"
    Delete "$APPDATA\DisplayXR\apps\icon_sbs.png"
    RMDir "$APPDATA\DisplayXR\apps"

    ; Remove install dir contents.
    Delete "$INSTDIR\gaussian_splatting_handle_vk_win.exe"
    Delete "$INSTDIR\butterfly.spz"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    RMDir "$PROGRAMFILES64\DisplayXR\Demos"

    ; Start menu shortcut.
    Delete "$SMPROGRAMS\DisplayXR\Gaussian Splat Viewer.lnk"
    ; Don't RMDir $SMPROGRAMS\DisplayXR — the runtime's own shortcuts may
    ; still live there.

    DeleteRegKey HKLM "Software\DisplayXR\Demos\GaussianSplat"
    DeleteRegKey /ifempty HKLM "Software\DisplayXR\Demos"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRGaussianSplat"
SectionEnd

;--------------------------------
; Version metadata

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR Gaussian Splat Demo"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 DisplayXR"
VIAddVersionKey "FileDescription" "DisplayXR Gaussian Splat Demo Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
