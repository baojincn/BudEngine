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

if(spirv-reflect_FOUND)
    target_link_libraries(BudAssetTool PRIVATE spirv-reflect::spirv-reflect)
else()
    find_library(SPIRV_REFLECT_STATIC NAMES spirv-reflect-static spirv-reflect HINTS "${CMAKE_CURRENT_BINARY_DIR}/vcpkg_installed/x64-windows/lib" NO_DEFAULT_PATH)
    if(SPIRV_REFLECT_STATIC)
        target_link_libraries(BudAssetTool PRIVATE ${SPIRV_REFLECT_STATIC})
    endif()
endif()

# Include project root and include dir
target_include_directories(BudAssetTool PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

set_target_properties(BudAssetTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
    target_link_options(BudAssetTool PRIVATE "/CETCOMPAT:NO")
endif()
