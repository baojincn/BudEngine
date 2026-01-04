module;

#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <optional>

// 保持第三方库的 include
#include <tiny_obj_loader.h>
#include <tiny_gltf.h> 
#include <stb_image.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL // for glm::hash
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

export module bud.io;

export namespace bud::io {


	export struct MeshData {
		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec3 normal;
			glm::vec2 texture_uv;
			float texture_index;

			bool operator==(const Vertex& other) const {
				return pos == other.pos &&
					color == other.color &&
					normal == other.normal &&
					texture_uv == other.texture_uv &&
					texture_index == other.texture_index;
			}
		};

		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<std::string> texture_paths;
	};
}

// 特化 glm::vec3 和 glm::vec2 的哈希函数，以支持 MeshData::Vertex 的哈希
namespace std {
	template<> struct hash<bud::io::MeshData::Vertex> {
		size_t operator()(bud::io::MeshData::Vertex const& vertex) const {
			auto h1 = hash<glm::vec3>()(vertex.pos);
			auto h2 = hash<glm::vec3>()(vertex.color);
			auto h3 = hash<glm::vec3>()(vertex.normal);
			auto h4 = hash<glm::vec2>()(vertex.texture_uv);
			auto h5 = hash<float>()(vertex.texture_index);

			return h1 ^ (h2 << 1) ^ (h2 << 2) ^ (h3 << 3) ^ (h5 << 4);
		}
	};
}

export namespace bud::io {
	// ==========================================
	// 1. File System (基础 IO)
	// ==========================================
	export class FileSystem {
	public:
		// 通用二进制读取 (用于 Shader (SPV), Buffer 等)
		static std::optional<std::vector<char>> read_binary(const std::filesystem::path& path) {
			std::ifstream file(path, std::ios::ate | std::ios::binary);

			if (!file.is_open()) {
				std::println(stderr, "[IO] Failed to open file: {}", path.string());
				return std::nullopt;
			}

			size_t file_size = (size_t)file.tellg();
			std::vector<char> buffer(file_size);

			file.seekg(0);
			file.read(buffer.data(), file_size);
			file.close();

			return buffer;
		}
	};

	// ==========================================
	// 2. Image Loader (图片资源)
	// ==========================================

	// RAII 封装：自动释放 stbi 内存
	export struct Image {
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char* pixels = nullptr;

		// 禁用拷贝，允许移动 (Move-only)
		Image() = default;
		Image(const Image&) = delete;
		Image& operator=(const Image&) = delete;

		Image(Image&& other) noexcept {
			move_from(std::move(other));
		}
		Image& operator=(Image&& other) noexcept {
			if (this != &other) {
				cleanup();
				move_from(std::move(other));
			}
			return *this;
		}

		~Image() {
			cleanup();
		}

		bool is_valid() const { return pixels != nullptr; }

	private:
		void cleanup() {
			if (pixels) {
				stbi_image_free(pixels);
				pixels = nullptr;
			}
		}
		void move_from(Image&& other) {
			width = other.width;
			height = other.height;
			channels = other.channels;
			pixels = other.pixels;
			other.pixels = nullptr; // 接管所有权
		}
	};


	export class ImageLoader {
	public:
		static std::optional<Image> load(const std::filesystem::path& path) {
			Image img;
			std::string path_str = path.string();

			// 强制加载为 RGBA (4通道)，Vulkan 友好
			img.pixels = stbi_load(path_str.c_str(), &img.width, &img.height, &img.channels, STBI_rgb_alpha);

			if (!img.pixels) {
				std::println(stderr, "[IO] Failed to load image: {}", path_str);
				return std::nullopt;
			}

			img.channels = 4;
			return std::move(img);
		}
	};

	// ==========================================
	// 3. Model Loader (网格模型)
	// ==========================================
	export class ModelLoader {
	public:
		static std::optional<MeshData> load_obj(const std::filesystem::path& path) {
			tinyobj::attrib_t attrib;
			std::vector<tinyobj::shape_t> shapes;
			std::vector<tinyobj::material_t> materials;
			std::string warn, err;

			std::string path_str = path.string();
			std::string base_dir = path.parent_path().string() + "/"; // load .mtl

			bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, path_str.c_str(), base_dir.c_str());

			if (!warn.empty() && warn.find("Both") == std::string::npos)
				std::println("[OBJ Warn]: {}", warn);

			if (!err.empty())
				std::println("[OBJ Error]: {}", err);

			if (!ret)
				return std::nullopt;

			MeshData meshData;
			// 1. 收集所有用到的纹理
				// map: 材质名 -> 全局纹理 ID (0, 1, 2...)
			std::unordered_map<int, float> materialToTextureIndex;

			// 默认纹理 ID 为 0 (我们稍后把 0 号槽位留给那个 1x1 的白图或紫黑格)
			float defaultTexIndex = 0.0f;

			// Sponza 的材质里有漫反射贴图 (diffuse_texname)
			for (size_t i = 0; i < materials.size(); i++) {
				std::string texName = materials[i].diffuse_texname;
				if (!texName.empty()) {
					// 构造完整路径
					std::string fullPath = base_dir + texName;

					// 存入 list
					meshData.texture_paths.push_back(fullPath);

					// 记录映射关系: Material ID (i) -> Texture Array Index (size)
					// 注意：因为我们预留了 0 号作为 fallback，所以这里 +1.0f
					materialToTextureIndex[i] = static_cast<float>(meshData.texture_paths.size());
				}
				else {
					materialToTextureIndex[i] = 0.0f; // 没有贴图的材质用默认图
				}
			}

			// 如果一个纹理都没找到，至少保证 lists 不为空
			if (meshData.texture_paths.empty()) {
				// 只是为了占位，具体的 fallback 逻辑在 RHI 处理
			}

			std::unordered_map<MeshData::Vertex, uint32_t> uniqueVertices{};

			for (const auto& shape : shapes) {
				size_t index_offset = 0;
				// 遍历每一个面 (Face)
				for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
					int fv = shape.mesh.num_face_vertices[f]; // 通常是 3 (三角形)

					// 获取这个面的材质 ID
					int mat_id = shape.mesh.material_ids[f];
					float currentTexIndex = 0.0f;
					if (mat_id >= 0 && materialToTextureIndex.count(mat_id)) {
						currentTexIndex = materialToTextureIndex[mat_id];
					}

					// 遍历面的每个顶点
					for (size_t v = 0; v < fv; v++) {
						tinyobj::index_t idx = shape.mesh.indices[index_offset + v];

						MeshData::Vertex vertex{};
						// ... pos, texCoord 读取逻辑不变 ...
						vertex.pos = { attrib.vertices[3 * idx.vertex_index + 0], attrib.vertices[3 * idx.vertex_index + 1], attrib.vertices[3 * idx.vertex_index + 2] };

						if (idx.normal_index >= 0) {
							vertex.normal = { attrib.normals[3 * idx.normal_index + 0], attrib.normals[3 * idx.normal_index + 1], attrib.normals[3 * idx.normal_index + 2] };
						}
						else {
							// 如果没有法线，给个默认向上的
							vertex.normal = { 0.0f, 1.0f, 0.0f };
						}

						if (idx.texcoord_index >= 0) {
							vertex.texture_uv = { attrib.texcoords[2 * idx.texcoord_index + 0], 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1] };
						}
						vertex.color = { 1.0f, 1.0f, 1.0f };

						// [新增] 写入纹理索引
						vertex.texture_index = currentTexIndex;

						if (uniqueVertices.count(vertex) == 0) {
							uniqueVertices[vertex] = static_cast<uint32_t>(meshData.vertices.size());
							meshData.vertices.push_back(vertex);
						}
						meshData.indices.push_back(uniqueVertices[vertex]);
					}
					index_offset += fv;
				}
			}

			return meshData;
		}

		static std::optional<MeshData> load_gltf(const std::filesystem::path& path) {
			tinygltf::Model model;
			tinygltf::TinyGLTF loader;
			std::string err;
			std::string warn;
			std::string path_str = path.string();
			bool ret = false;
			if (path.extension() == ".glb") ret = loader.LoadBinaryFromFile(&model, &err, &warn, path_str);
			else ret = loader.LoadASCIIFromFile(&model, &err, &warn, path_str);

			if (!warn.empty()) std::println("[glTF Warn]: {}", warn);
			if (!err.empty()) std::println("[glTF Error]: {}", err);
			if (!ret) return std::nullopt;

			return convert_to_mesh_data(model);
		}
	private:
		static MeshData convert_to_mesh_data(const tinygltf::Model& model) {
			MeshData meshData;

			if (model.meshes.empty()) return meshData;
			const auto& gltfMesh = model.meshes[0];
			if (gltfMesh.primitives.empty()) return meshData;
			const auto& primitive = gltfMesh.primitives[0];
			const float* positionBuffer = nullptr;
			const float* texCoordBuffer = nullptr;
			size_t vertexCount = 0;
			if (primitive.attributes.find("POSITION") != primitive.attributes.end()) {
				const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];
				const auto& view = model.bufferViews[accessor.bufferView];
				positionBuffer = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
				vertexCount = accessor.count;
			}
			if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end()) {
				const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
				const auto& view = model.bufferViews[accessor.bufferView];
				texCoordBuffer = reinterpret_cast<const float*>(&model.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
			}
			for (size_t i = 0; i < vertexCount; i++) {
				MeshData::Vertex vertex{};
				if (positionBuffer) vertex.pos = glm::vec3(positionBuffer[i * 3 + 0], positionBuffer[i * 3 + 1], positionBuffer[i * 3 + 2]);
				vertex.color = glm::vec3(1.0f, 1.0f, 1.0f);
				if (texCoordBuffer) vertex.texture_uv = glm::vec2(texCoordBuffer[i * 2 + 0], texCoordBuffer[i * 2 + 1]);
				meshData.vertices.push_back(vertex);
			}
			if (primitive.indices >= 0) {
				const auto& accessor = model.accessors[primitive.indices];
				const auto& bufferView = model.bufferViews[accessor.bufferView];
				const auto& buffer = model.buffers[bufferView.buffer];
				const void* dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
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

