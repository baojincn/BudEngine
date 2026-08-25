cmake_minimum_required(VERSION 3.30)

project(ShaderLayoutCodegen LANGUAGES CXX)

add_executable(ShaderLayoutCodegen
    main.cpp
)

target_link_libraries(ShaderLayoutCodegen PRIVATE nlohmann_json::nlohmann_json)

target_include_directories(ShaderLayoutCodegen PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
