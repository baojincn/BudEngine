cmake_minimum_required(VERSION 4.0)

# 设置vcpkg工具链
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" 
       AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE 
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
endif()

# Begin, global settings
project(BudEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_language(ASM_MASM)

# Profiler support, CMakePresets can override this option
if(NOT DEFINED BUD_ENABLE_PROFILING)
	option(BUD_ENABLE_PROFILING "Enable Tracy Profiler" ON)
endif()

# MSVC 模块支持
if(MSVC)
    set(CMAKE_CXX_MODULE_SCAN ON)
    add_compile_options(/TP)
endif()

# 检测平台
if(WIN32)
    set(PLATFORM_WINDOWS ON)
    message(STATUS "Building for Windows")
elseif(APPLE)
    set(PLATFORM_MACOS ON)
    message(STATUS "Building for macOS")
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_LINUX ON)
    message(STATUS "Building for Linux")
endif()

option(BUD_BUILD_SAMPLES "Build samples" ON)

# Begin, find dependencies
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
if(BUD_ENABLE_PROFILING)
	find_package(Tracy CONFIG REQUIRED)
endif()
# End, find dependencies

set(BUD_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
# End, global settings

# Begin, define engine core library
add_library(bud_engine_core STATIC
    "src/platform/window.cpp"
    "src/runtime/bud_engine.cpp"
)

if (BUD_ENABLE_PROFILING)
	target_compile_definitions(bud_engine_core PUBLIC TRACY_ENABLE)
	target_link_libraries(bud_engine_core PUBLIC Tracy::TracyClient)
endif()

if(MSVC)
	target_link_options(bud_engine_core PRIVATE "/CETCOMPAT:NO")
endif()

# Add assembly sources and module files
target_sources(bud_engine_core
    PRIVATE
        "src/platform/windows/switch_context_win_x86_64.asm"
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
        FILES
            "src/core/bud.core.cppm"
            "src/core/bud.math.cppm"
            "src/dod/bud.dod.cppm"
            "src/threading/bud.threading.cppm"
            "src/graphics/bud.graphics.cppm"
			"src/graphics/rhi/bud.graphics.rhi.cppm"
            
            "src/threading/bud.io.cppm"
            "src/platform/bud.platform.cppm"
            "src/runtime/bud.engine.cppm"
)

set_target_properties(bud_engine_core PROPERTIES CXX_SCAN_FOR_MODULES ON)

target_link_libraries(
    bud_engine_core
    PUBLIC
        SDL3::SDL3
        Vulkan::Vulkan
)

target_include_directories(bud_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# End, define engine core library

# Begin, triangle_sample
if(BUD_BUILD_SAMPLES)
    add_executable(triangle_sample samples/triangle/main.cpp)
    target_link_libraries(triangle_sample PRIVATE bud_engine_core)
    target_include_directories(triangle_sample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(triangle_sample PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

	set_property(TARGET triangle_sample PROPERTY CXX_SCAN_FOR_MODULES ON)

	if(MSVC)
        target_link_options(triangle_sample PRIVATE "/CETCOMPAT:NO")
    endif()
endif()
# End, triangle_sample
