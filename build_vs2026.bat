@echo off
setlocal enabledelayedexpansion

:: Find latest Visual Studio installation using vswhere
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
  set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    echo [Error] Visual Studio installation not found!
    exit /b 1
)

set "VC_VARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
set "VCPKG_VISUAL_STUDIO_PATH=%VS_PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.335.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo [Build Script] Initializing Visual Studio Environment from: %VS_PATH%...
call "%VC_VARS%"
if %errorlevel% neq 0 (
    echo [Error] Failed to load vcvars64.bat
    exit /b 1
)

echo [Build Script] Environment ready. Checking tools...
set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

:: Fallback to system cmake if the VS bundled one is not found
if not exist "%CMAKE_EXE%" (
    set "CMAKE_EXE=cmake"
)

echo [Build Script] Configuring CMake (Preset: debug)...
"%CMAKE_EXE%" --preset debug
if %errorlevel% neq 0 (
    echo [Error] CMake Configuration failed
    exit /b 1
)

echo [Build Script] Building (Preset: Debug x64)...
"%CMAKE_EXE%" --build --preset "Debug x64"
if %errorlevel% neq 0 (
    echo [Error] Build failed
    exit /b 1
)

echo [Build Script] Build Success!
exit /b 0
