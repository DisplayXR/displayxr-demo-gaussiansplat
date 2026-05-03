@echo off
setlocal
set "REPO=%~dp0.."
set "BIN_DIR=%REPO%\build\windows"
set "OUT_DIR=%~dp0"
if "%OUT_DIR:~-1%"=="\" set "OUT_DIR=%OUT_DIR:~0,-1%"
if "%VERSION%"=="" set "VERSION=1.2.0"
if "%VERSION_MAJOR%"=="" set "VERSION_MAJOR=1"
if "%VERSION_MINOR%"=="" set "VERSION_MINOR=2"
if "%VERSION_PATCH%"=="" set "VERSION_PATCH=0"

if not exist "%BIN_DIR%\gaussian_splatting_handle_vk_win.exe" (
    echo ERROR: demo binary not found at %BIN_DIR%\gaussian_splatting_handle_vk_win.exe
    echo Run scripts\build-with-deps.bat first.
    exit /b 1
)

"C:\Program Files (x86)\NSIS\makensis.exe" /DVERSION=%VERSION% /DVERSION_MAJOR=%VERSION_MAJOR% /DVERSION_MINOR=%VERSION_MINOR% /DVERSION_PATCH=%VERSION_PATCH% "/DBIN_DIR=%BIN_DIR%" "/DSOURCE_DIR=%REPO%" "/DOUTPUT_DIR=%OUT_DIR%" "%~dp0DisplayXRGaussianSplatInstaller.nsi" || exit /b 1

echo === DONE ===
echo Installer: %OUT_DIR%DisplayXRGaussianSplatSetup-%VERSION%.exe
