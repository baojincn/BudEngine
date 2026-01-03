module;

#include <vector>
#include <string>
#include <filesystem>
#include <iostream>
#include <print>
#include <optional>

// [注意] 这里只包含头文件，绝对不要定义 IMPLEMENTATION
// 因为实现已经在 bud_third_party.cpp 里生成了
#include <tiny_gltf.h> 

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module bud.io;

export namespace bud::io {

	// 这是传给 RHI 的通用网格数据结构
	struct MeshData {
		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec2 texCoord;
		};

		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	class ModelLoader {
	public:
		// 加载 glTF 模型并返回 MeshData
		static std::optional<MeshData> load_gltf(const std::filesystem::path& path) {
			tinygltf::Model model;
			tinygltf::TinyGLTF loader;
			std::string err;
			std::string warn;

			std::string path_str = path.string();
			bool ret = false;

			// 自动检测是二进制(.glb)还是文本(.gltf)
			if (path.extension() == ".glb") {
				ret = loader.LoadBinaryFromFile(&model, &err, &warn, path_str);
			}
			else {
				ret = loader.LoadASCIIFromFile(&model, &err, &warn, path_str);
			}

			if (!warn.empty()) {
				std::println("[glTF Warn]: {}", warn);
			}
			if (!err.empty()) {
				std::println("[glTF Error]: {}", err);
			}
			if (!ret) {
				std::println("Failed to parse glTF: {}", path_str);
				return std::nullopt;
			}

			return convert_to_mesh_data(model);
		}

	private:
		static MeshData convert_to_mesh_data(const tinygltf::Model& model) {
			MeshData meshData;

			// 简单演示：只加载第一个 Mesh 的第一个 Primitive
			if (model.meshes.empty()) return meshData;

			const auto& gltfMesh = model.meshes[0];
			if (gltfMesh.primitives.empty()) return meshData;

			const auto& primitive = gltfMesh.primitives[0];

			// 1. 读取顶点数据 (Position)
			const float* positionBuffer = nullptr;
			const float* texCoordBuffer = nullptr;
			size_t vertexCount = 0;

			if (primitive.attributes.find("POSITION") != primitive.attributes.end()) {
				const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];
				const auto& view = model.bufferViews[accessor.bufferView];
				positionBuffer = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
				vertexCount = accessor.count;
			}

			// 2. 读取纹理坐标 (TEXCOORD_0)
			if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end()) {
				const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
				const auto& view = model.bufferViews[accessor.bufferView];
				texCoordBuffer = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
			}

			// 3. 组装顶点
			for (size_t i = 0; i < vertexCount; i++) {
				MeshData::Vertex vertex{};

				if (positionBuffer) {
					vertex.pos = glm::vec3(positionBuffer[i * 3 + 0], positionBuffer[i * 3 + 1], positionBuffer[i * 3 + 2]);
				}

				// 默认白色
				vertex.color = glm::vec3(1.0f, 1.0f, 1.0f);

				if (texCoordBuffer) {
					vertex.texCoord = glm::vec2(texCoordBuffer[i * 2 + 0], texCoordBuffer[i * 2 + 1]);
				}

				meshData.vertices.push_back(vertex);
			}

			// 4. 读取索引 (Indices)
			if (primitive.indices >= 0) {
				const auto& accessor = model.accessors[primitive.indices];
				const auto& bufferView = model.bufferViews[accessor.bufferView];
				const auto& buffer = model.buffers[bufferView.buffer];

				const void* dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

				// glTF 索引可能是不同类型，需要转换
				if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
					const uint32_t* buf = static_cast<const uint32_t*>(dataPtr);
					for (size_t i = 0; i < accessor.count; i++) meshData.indices.push_back(buf[i]);
				}
				else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
					const uint16_t* buf = static_cast<const uint16_t*>(dataPtr);
					for (size_t i = 0; i < accessor.count; i++) meshData.indices.push_back(buf[i]);
				}
				else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
					const uint8_t* buf = static_cast<const uint8_t*>(dataPtr);
					for (size_t i = 0; i < accessor.count; i++) meshData.indices.push_back(buf[i]);
				}
			}

			return meshData;
		}
	};
}
