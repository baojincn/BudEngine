cmake_minimum_required(VERSION 3.30)

project(SceneTool LANGUAGES CXX)

add_executable(SceneTool
    main.cpp
)

target_link_libraries(SceneTool
    PRIVATE
        bud_asset_pipeline
)

target_include_directories(SceneTool PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

if(MSVC)
    target_link_options(SceneTool PRIVATE "/CETCOMPAT:NO")
endif()
