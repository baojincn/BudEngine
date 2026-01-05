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
	option(BUD_ENABLE_PROFILING "Enable Tracy Profiler" OFF)
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

file(GLOB SHADER_FILES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.vert"	# Vertex Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.frag"	# Fragment Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.comp"	# Compute Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.geom"	# Geometry Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.tesc"	# Tessellation Control Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.tese"	# Tessellation Evaluation Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.mesh"	# Mesh Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.task"	# Task Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rgen"	# Ray Generation Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rmiss"	# Ray Miss Shader
	"${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/*.rchit"	# Ray Closest Hit Shader
)

# Find glslc compiler and set up custom commands to compile shaders
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

add_library(bud_engine_core STATIC
	${SHADER_FILES}
  )

# Ensure shaders are compiled before building the core library
if(TARGET CompileShaders)
    add_dependencies(bud_engine_core CompileShaders)
endif()

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
		"src/platform/window.cpp"
		"src/runtime/bud_engine.cpp"
		"src/graphics/bud.graphics.cpp"
		"src/graphics/vulkan/bud.graphics.vulkan.cpp"
		"src/third_party/bud_third_party.cpp"
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
        FILES
            "src/core/bud.core.cppm"
            "src/core/bud.math.cppm"
            "src/io/bud.io.cppm"
            "src/dod/bud.dod.cppm"
            "src/runtime/bud.engine.cppm"
            "src/platform/bud.platform.cppm"
            "src/threading/bud.threading.cppm"

			"src/graphics/bud.graphics.cppm"

			"src/graphics/vulkan/bud.graphics.vulkan.cppm"
			"src/graphics/vulkan/bud.vulkan.memory.cppm"
			"src/graphics/d3d12/bud.graphics.d3d12.cppm"
			"src/graphics/d3d12/bud.d3d12.memory.cppm"
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
	set_target_properties(triangle_sample PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

	set_property(TARGET triangle_sample PROPERTY CXX_SCAN_FOR_MODULES ON)

	if(MSVC)
        target_link_options(triangle_sample PRIVATE "/CETCOMPAT:NO")
    endif()
endif()
# End, triangle_sample
