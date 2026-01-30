@echo off
set "VS_PATH=E:\Program Files\Microsoft Visual Studio\18\Professional"
set "VC_VARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
set "VCPKG_VISUAL_STUDIO_PATH=%VS_PATH%"
set "VULKAN_SDK=C:\VulkanSDK\1.4.335.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo [Build Script] Initializing VS2026 (v18) Environment...
call "%VC_VARS%"
if %errorlevel% neq 0 (
    echo [Error] Failed to load vcvars64.bat
    exit /b 1
)

echo [Build Script] Environment ready. Checking tools...
set "CMAKE_EXE=E:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

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
