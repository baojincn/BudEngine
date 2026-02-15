cmake_minimum_required(VERSION 4.0)

# Setup vcpkg toolchain
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

# Profiler support
if(NOT DEFINED BUD_ENABLE_PROFILING)
	option(BUD_ENABLE_PROFILING "Enable Tracy Profiler" OFF)
endif()

# Nsight Aftermath integration
if(NOT DEFINED BUD_ENABLE_AFTERMATH)
    option(BUD_ENABLE_AFTERMATH "Enable NVIDIA Nsight Aftermath GPU crash dumps" ON)
endif()

# Platform detection
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

# ---------------------------------------------------------------------------
# Nsight Aftermath SDK Configuration (Simplified)
# ---------------------------------------------------------------------------
set(AFTERMATH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/NsightAftermath)
set(AFTERMATH_INCLUDE_DIR ${AFTERMATH_ROOT}/include)
set(AFTERMATH_LIB_DIR ${AFTERMATH_ROOT}/lib/x64)

if(BUD_ENABLE_AFTERMATH)
    # Find the base library for linking (crash-dump lib not required)
	find_library(AFTERMATH_LIB NAMES GFSDK_Aftermath_Lib.x64 PATHS ${AFTERMATH_LIB_DIR} NO_DEFAULT_PATH)

	if(AFTERMATH_LIB)
		message(STATUS "Nsight Aftermath base lib found in ${AFTERMATH_LIB_DIR}")
	else()
		message(WARNING "BUD_ENABLE_AFTERMATH=ON but GFSDK_Aftermath_Lib.x64.lib not found in ${AFTERMATH_LIB_DIR}. Disabling.")
		set(BUD_ENABLE_AFTERMATH OFF CACHE BOOL "" FORCE)
	endif()
endif()
# End, global settings


# Begin, define engine core library
file(GLOB SHADER_FILES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.vert"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.frag"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.comp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.geom"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.tesc"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.tese"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.mesh"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.task"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rgen"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rmiss"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rchit"
)

# Ensure new prepass shaders are included
list(APPEND SHADER_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/zprepass.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/zprepass.frag"
)

# Find glslc compiler
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin")

if(NOT GLSLC_EXECUTABLE)
    message(WARNING "Not found glslc, please install Vulkan SDK")
else()
    message(STATUS "Found Shader compiler: ${GLSLC_EXECUTABLE}")
    
    set(SPV_OUTPUTS "")

    foreach(SHADER_SOURCE ${SHADER_FILES})
        get_filename_component(SHADER_EXT ${SHADER_SOURCE} EXT)

		if(NOT SHADER_EXT STREQUAL ".spv")
            set(SHADER_OUTPUT "${SHADER_SOURCE}.spv")

			add_custom_command(
                OUTPUT ${SHADER_OUTPUT}
                COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
                DEPENDS ${SHADER_SOURCE}
                COMMENT "Compiling ${SHADER_SOURCE} -> ${SHADER_OUTPUT}"
                VERBATIM
            )
            
            list(APPEND SPV_OUTPUTS ${SHADER_OUTPUT})
        endif()
    endforeach()

    add_custom_target(CompileShaders ALL DEPENDS ${SPV_OUTPUTS})
endif()

source_group("Shaders" FILES ${SHADER_FILES})

add_library(bud_engine_core STATIC ${SHADER_FILES})

# Ensure shaders are compiled before building the core library
if(TARGET CompileShaders)
    add_dependencies(bud_engine_core CompileShaders)
endif()

if (BUD_ENABLE_PROFILING)
	target_compile_definitions(bud_engine_core PUBLIC TRACY_ENABLE)
	target_link_libraries(bud_engine_core PUBLIC Tracy::TracyClient)
endif()

if(BUD_ENABLE_AFTERMATH)
	target_include_directories(bud_engine_core PRIVATE ${AFTERMATH_INCLUDE_DIR})
	if(AFTERMATH_LIB)
		target_compile_definitions(bud_engine_core PUBLIC BUD_ENABLE_AFTERMATH)
		target_link_libraries(bud_engine_core PRIVATE ${AFTERMATH_LIB})
	endif()
endif()

if(MSVC)
	target_link_options(bud_engine_core PRIVATE "/CETCOMPAT:NO")
endif()

# Add sources
target_sources(bud_engine_core
    PRIVATE
        "src/platform/windows/switch_context_win_x86_64.asm"
		"src/platform/window.cpp"
		"src/runtime/bud.engine.cpp"
		"src/graphics/bud.graphics.cpp"
		"src/graphics/bud.graphics.scene.cpp"
        "src/graphics/bud.graphics.passes.cpp"
        "src/graphics/bud.graphics.renderer.cpp"
        "src/graphics/bud.graphics.graph.cpp"
        
		"src/graphics/vulkan/bud.graphics.vulkan.cpp"
		"src/graphics/vulkan/bud.vulkan.memory.cpp"
		"src/graphics/vulkan/bud.vulkan.descriptors.cpp"
		"src/graphics/vulkan/bud.vulkan.pipeline.cpp"
		"src/graphics/vulkan/bud.vulkan.pool.cpp"

		"src/io/bud.io.cpp"
		"src/threading/bud.threading.cpp"
        "src/third_party/bud_third_party.cpp"

		"src/runtime/bud.input.cpp"
		"src/runtime/bud.scene.cpp"

    PUBLIC
		"src/core/bud.core.hpp"
		"src/core/bud.math.hpp"
		"src/io/bud.io.hpp"
		"src/dod/bud.dod.hpp"
		"src/runtime/bud.engine.hpp"
		"src/platform/bud.platform.hpp"
		"src/threading/bud.threading.hpp"
		"src/runtime/bud.input.hpp"
		"src/runtime/bud.scene.hpp"

		"src/graphics/bud.graphics.hpp"
		"src/graphics/bud.graphics.rhi.hpp"
		"src/graphics/bud.graphics.scene.hpp"
		"src/graphics/bud.graphics.types.hpp"
		"src/graphics/bud.graphics.memory.hpp"
		"src/graphics/bud.graphics.pool.hpp"
		"src/graphics/bud.graphics.passes.hpp"
		"src/graphics/bud.graphics.renderer.hpp"
		"src/graphics/bud.graphics.graph.hpp"

		"src/graphics/vulkan/bud.graphics.vulkan.hpp"
		"src/graphics/vulkan/bud.vulkan.memory.hpp"
		"src/graphics/vulkan/bud.vulkan.descriptors.hpp"
		"src/graphics/vulkan/bud.vulkan.pipeline.hpp"
		"src/graphics/vulkan/bud.vulkan.pool.hpp"
		"src/graphics/vulkan/bud.vulkan.types.hpp"
		"src/graphics/vulkan/bud.vulkan.utils.hpp"
)

target_link_libraries(bud_engine_core PUBLIC SDL3::SDL3 Vulkan::Vulkan)

target_include_directories(bud_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# End, define engine core library

# Begin, triangle_sample
if(BUD_BUILD_SAMPLES)
    add_executable(triangle_sample samples/triangle/main.cpp)
    target_link_libraries(triangle_sample PRIVATE bud_engine_core)
    target_include_directories(triangle_sample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    
    # Output directory
    set_target_properties(triangle_sample PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
	set_target_properties(triangle_sample PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

	if(MSVC)
        target_link_options(triangle_sample PRIVATE "/CETCOMPAT:NO")
    endif()

    # -----------------------------------------------------------------------
    # Simple Copy Logic: Directly Copy DLL to the Exe Directory
    # -----------------------------------------------------------------------
	if(BUD_ENABLE_AFTERMATH)
		add_custom_command(TARGET triangle_sample POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_CURRENT_SOURCE_DIR}/vendor/NsightAftermath/lib/x64/GFSDK_Aftermath_Lib.x64.dll"
			$<TARGET_FILE_DIR:triangle_sample>
			COMMENT "Copying Aftermath DLL into output binary directory"
		)
	endif()
endif()
# End, triangle_sample
