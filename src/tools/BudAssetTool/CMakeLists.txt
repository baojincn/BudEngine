cmake_minimum_required(VERSION 3.30)

project(BudAssetTool LANGUAGES CXX)

add_executable(BudAssetTool
    main.cpp
    bud.asset.processor.cpp
)

target_link_libraries(BudAssetTool
    PRIVATE
        meshoptimizer::meshoptimizer
        assimp::assimp
)

target_link_libraries(BudAssetTool PRIVATE bud_tool_support)

## Link SPIRV-Reflect: prefer the vcpkg imported target used by the top-level CMake
if (TARGET unofficial::spirv-reflect)
    target_link_libraries(BudAssetTool PRIVATE unofficial::spirv-reflect)
else()
    find_package(unofficial-spirv-reflect CONFIG QUIET)
    if (unofficial-spirv-reflect_FOUND)
        target_link_libraries(BudAssetTool PRIVATE unofficial::spirv-reflect)
    else()
        # Fallback: try to find a static or import library in common vcpkg layout
        find_library(SPIRV_REFLECT_STATIC NAMES spirv-reflect-static spirv-reflect HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/lib" NO_DEFAULT_PATH)
        if (SPIRV_REFLECT_STATIC)
            target_link_libraries(BudAssetTool PRIVATE ${SPIRV_REFLECT_STATIC})
        else()
            message(WARNING "SPIRV-Reflect not found for BudAssetTool; shader validation will be disabled for the tool target.")
            # Define a guard to exclude reflection code at build time (keeps consistent with include checks)
            target_compile_definitions(BudAssetTool PRIVATE BUD_HAVE_SPIRV_REFLECT=0)
        endif()
    endif()
endif()

# Include project root and include dir
target_include_directories(BudAssetTool PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

# Use top-level per-config runtime output directories instead of hardcoding
# RUNTIME_OUTPUT_DIRECTORY here. This ensures BudAssetTool is placed into
# the correct configuration folder (bin/debug, bin/release, etc.) when
# using multi-config generators like Visual Studio.

if(MSVC)
    target_link_options(BudAssetTool PRIVATE "/CETCOMPAT:NO")
endif()
