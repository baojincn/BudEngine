cmake_minimum_required(VERSION 3.30)

project(bud_asset_pipeline LANGUAGES CXX)

find_package(metis CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(mujoco CONFIG REQUIRED)

add_library(bud_asset_pipeline STATIC
    core/support.cpp
    core/asset_format.cpp
    core/serializer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bud.raw_mesh.cpp
    ${CMAKE_SOURCE_DIR}/src/robots/bud.robot.types.cpp
    ${CMAKE_SOURCE_DIR}/src/robots/bud.robot.mujoco.cpp
    core/raw_texture.cpp
    core/internal_mesh.cpp
    importers/obj_importer.cpp
    importers/gltf_importer.cpp
    importers/fbx_importer.cpp
    importers/dae_importer.cpp
    importers/stl_importer.cpp
    importers/urdf_parser.cpp
    importers/mjcf_parser.cpp
    third_party/pugixml/pugixml.cpp
    importers/texture_importer.cpp
    builders/virtual_geometry_builder.cpp
    builders/material_builder.cpp
    builders/material_asset_builder.cpp
    builders/texture_builder.cpp
    builders/mesh_builder.cpp
    builders/cloth_builder.cpp
    builders/cascade_builder.cpp
    builders/urdf_builder.cpp
    builders/mjcf_builder.cpp
    builders/mujoco_model_builder.cpp
    builders/collision_builder.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/bud.scene.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/bud.scene.builder.cpp
)

target_link_libraries(bud_asset_pipeline
    PUBLIC
        meshoptimizer::meshoptimizer
        assimp::assimp
        metis
        nlohmann_json::nlohmann_json
        Jolt::Jolt
        mujoco::mujoco
)

target_include_directories(bud_asset_pipeline
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${Stb_INCLUDE_DIR}
)

if(MSVC)
    target_compile_options(bud_asset_pipeline PRIVATE /W3)
else()
    target_compile_options(bud_asset_pipeline PRIVATE -Wall -Wextra)
endif()
