@echo off
setlocal
:: Usage: scripts\build-with-deps.bat [x64|arm64]
:: arm64 cross-compiles the gauss-splat Windows app from an x64 host. On ARM64
:: the renderer auto-selects the graphics pipeline (gs_renderer_select.h sees
:: _M_ARM64 — Snapdragon-X/Windows-on-ARM is tile-based deferred). Builds into a
:: separate build_arm64\ tree. NOTE: builds but will not RUN until the DisplayXR
:: runtime + a vendor display-processor plug-in exist for ARM64 (LeiaSR is
:: x64-only today).
set ARCH=%~1
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="x64" (
    set "VCVARS=vcvars64.bat"
    set "BUILDDIR=build"
    set "ARCH_ARG="
) else if /i "%ARCH%"=="arm64" (
    set "VCVARS=vcvarsamd64_arm64.bat"
    set "BUILDDIR=build_arm64"
    set "ARCH_ARG=-DDISPLAYXR_TARGET_ARCH=ARM64"
) else (
    echo ERROR: unknown architecture "%ARCH%" ^(expected x64 or arm64^)
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\%VCVARS%" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: VS 2022 %VCVARS% not found
    if /i "%ARCH%"=="arm64" echo For arm64, add the "MSVC ... ARM64/ARM64EC build tools" VS component.
    exit /b 1
)
set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
cd /d "%~dp0\.."

REM --- Vulkan SDK ----------------------------------------------------------------
REM Use the VULKAN_SDK env var the LunarG installer sets (any version), instead of
REM a hardcoded C:/VulkanSDK/<ver> path. find_package(Vulkan) picks it up via the
REM VULKAN_SDK environment, so no -DCMAKE_PREFIX_PATH is needed.
if "%VULKAN_SDK%"=="" (
    echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK from https://vulkan.lunarg.com
    echo        and open a fresh terminal so VULKAN_SDK is exported, then re-run.
    exit /b 1
)
REM Put the SDK's Bin on PATH so 3dgs_common's find_program(glslangValidator)
REM resolves. find_package(Vulkan) locates the compilers via VULKAN_SDK, but the
REM bare find_program only searches PATH — and some installs (e.g. the winget
REM Vulkan SDK package) set VULKAN_SDK without adding Bin to PATH, so configure
REM fails with "glslangValidator required". CI does this in a dedicated step.
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

REM --- OpenXR loader -------------------------------------------------------------
REM Auto-provision the prebuilt Khronos loader, pinned to the same spec revision as
REM the vendored openxr_includes/ headers (XR_CURRENT_API_VERSION = 1.1.51). Cached
REM under build\openxr_sdk so a fresh clone builds with no manually-staged SDK. The
REM prebuilt zip ships every arch (x64/ARM64/...), so one download serves both the
REM x64 and arm64 configures. (Mirrors what .github/workflows/build-windows.yml does.)
set "OPENXR_VER=1.1.51"
set "OPENXR_DIR=%CD%\build\openxr_sdk"
if not exist "%OPENXR_DIR%\x64\lib\openxr_loader.lib" (
    echo === Provisioning OpenXR loader %OPENXR_VER% ===
    if not exist build mkdir build
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop';" ^
      "$u='https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VER%/openxr_loader_windows-%OPENXR_VER%.zip';" ^
      "Invoke-WebRequest -Uri $u -OutFile 'build\openxr_loader.zip';" ^
      "Expand-Archive -Path 'build\openxr_loader.zip' -DestinationPath 'build\openxr_sdk' -Force;" ^
      "Remove-Item 'build\openxr_loader.zip' -Force" || exit /b 1
)

echo === Configuring (%ARCH%) ===
cmake -S . -B %BUILDDIR% -G Ninja -DCMAKE_BUILD_TYPE=Release %ARCH_ARG% -DOpenXR_ROOT="%CD:\=/%/build/openxr_sdk" || exit /b 1
echo === Building ===
cmake --build %BUILDDIR% || exit /b 1
echo === DONE: %BUILDDIR%\windows\gaussian_splatting_handle_vk_win.exe ===
