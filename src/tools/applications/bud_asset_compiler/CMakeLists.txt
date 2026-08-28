cmake_minimum_required(VERSION 3.30)

project(BudAssetCompiler LANGUAGES CXX)

add_executable(BudAssetCompiler
    main.cpp
    bud.asset.processor.cpp
)

target_link_libraries(BudAssetCompiler
    PRIVATE
        bud_asset_pipeline
)

## Link SPIRV-Reflect: prefer the vcpkg imported target used by the top-level CMake
if (TARGET unofficial::spirv-reflect)
    target_link_libraries(BudAssetCompiler PRIVATE unofficial::spirv-reflect)
else()
    find_package(unofficial-spirv-reflect CONFIG QUIET)
    if (unofficial-spirv-reflect_FOUND)
        target_link_libraries(BudAssetCompiler PRIVATE unofficial::spirv-reflect)
    else()
        find_library(SPIRV_REFLECT_STATIC NAMES spirv-reflect-static spirv-reflect HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/lib" NO_DEFAULT_PATH)
        if (SPIRV_REFLECT_STATIC)
            target_link_libraries(BudAssetCompiler PRIVATE ${SPIRV_REFLECT_STATIC})
        else()
            message(WARNING "SPIRV-Reflect not found for BudAssetCompiler; shader validation will be disabled for the tool target.")
            target_compile_definitions(BudAssetCompiler PRIVATE BUD_HAVE_SPIRV_REFLECT=0)
        endif()
    endif()
endif()

target_include_directories(BudAssetCompiler PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

if(MSVC)
    target_link_options(BudAssetCompiler PRIVATE "/CETCOMPAT:NO")
endif()
