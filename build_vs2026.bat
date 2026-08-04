@echo off
setlocal enabledelayedexpansion

:: Configuration Stage: Set explicit paths for VS 2026 (Professional) on D: drive
set "VS_PATH=d:\Program Files\Microsoft Visual Studio\18\Professional"
set "VC_VARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CL_EXE=%VS_PATH%\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64\cl.exe"

set "VULKAN_SDK=D:\VulkanSDK\1.4.335.0"
set "PATH=%VULKAN_SDK%\Bin;%PATH%"

:: 1. Force environment initialization
if not exist "%VC_VARS%" (
    echo [Error] vcvars64.bat not found at: %VC_VARS%
    exit /b 1
)

echo [Build Script] Initializing MSVC 2026 Environment...
call "%VC_VARS%"

:: 2. Ensure CMake uses the explicit compiler
echo [Build Script] Configuring CMake (Preset: debug)...
"%CMAKE_EXE%" --preset vs-multi ^
  "-DCMAKE_C_COMPILER=%CL_EXE%" ^
  "-DCMAKE_CXX_COMPILER=%CL_EXE%"

if %errorlevel% neq 0 (
    echo [Error] CMake Configuration failed
    exit /b 1
)

:: 3. Build the sample
echo [Build Script] Building (Preset: Debug x64)...
"%CMAKE_EXE%" --build --preset "Debug x64"
if %errorlevel% neq 0 (
    echo [Error] Build failed
    exit /b 1
)

echo [Build Script] Build Success!
exit /b 0
