cmake_minimum_required(VERSION 3.30)

project(bud_tool_support LANGUAGES CXX)

add_library(bud_tool_support STATIC
    bud_tool_support.cpp
)

target_include_directories(bud_tool_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(bud_tool_support PRIVATE /W3)
else()
    target_compile_options(bud_tool_support PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(bud_tool_support PROPERTIES PUBLIC_HEADER bud_tool_support.hpp)

# Small example executable demonstrating the support API
add_executable(bud_tool_support_example
    example_main.cpp
)
target_link_libraries(bud_tool_support_example PRIVATE bud_tool_support)
target_include_directories(bud_tool_support_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Do not set a hardcoded RUNTIME_OUTPUT_DIRECTORY here; use the top-level
# per-config output directories so multi-config generators (Visual Studio)
# place binaries into configuration-specific folders (e.g. bin/debug).
