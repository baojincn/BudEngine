module;

#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <optional>

// 保持第三方库的 include
#include <tiny_gltf.h> 
#include <stb_image.h> // 把 stb_image 的依赖移到这里

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module bud.io;

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
	export struct MeshData {
		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec2 texCoord;
		};

		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	export class ModelLoader {
	public:
		static std::optional<MeshData> load_gltf(const std::filesystem::path& path) {
			// ... (保持你之前的 ModelLoader 代码不变) ...
			// 篇幅原因省略，这部分代码可以直接保留
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
			// ... (保持你之前的转换代码不变) ...
			MeshData meshData;
			// 复制原来的逻辑即可
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
				if (texCoordBuffer) vertex.texCoord = glm::vec2(texCoordBuffer[i * 2 + 0], texCoordBuffer[i * 2 + 1]);
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
