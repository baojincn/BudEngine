cmake_minimum_required(VERSION 3.30)

project(BudAssetImporter LANGUAGES CXX)

add_executable(BudAssetImporter
    main.cpp
)

target_link_libraries(BudAssetImporter
    PRIVATE
        bud_asset_pipeline
)

target_include_directories(BudAssetImporter PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)

if(MSVC)
    target_link_options(BudAssetImporter PRIVATE "/CETCOMPAT:NO")
endif()
