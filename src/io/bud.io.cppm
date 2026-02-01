module;

#include <vector>
#include <string>
#include <filesystem>
#include <optional>
#include <functional>

// 保持第三方库的 include
#include <tiny_gltf.h> 

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL // for glm::hash
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

export module bud.io;

import bud.threading;

export namespace bud::io {


	export struct MeshData {
		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec3 normal;
			glm::vec2 texture_uv;
			float texture_index;

			bool operator==(const Vertex& other) const;
		};

		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<std::string> texture_paths;
	};
}

// 特化 glm::vec3 和 glm::vec2 的哈希函数
export namespace std {
	template<> struct hash<bud::io::MeshData::Vertex> {
		auto operator()(bud::io::MeshData::Vertex const& vertex) const {
			auto h1 = hash<glm::vec3>()(vertex.pos);
			auto h2 = hash<glm::vec3>()(vertex.color);
			auto h3 = hash<glm::vec3>()(vertex.normal);
			auto h4 = hash<glm::vec2>()(vertex.texture_uv);
			auto h5 = hash<float>()(vertex.texture_index);

			return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
		}
	};
}

export namespace bud::io {

	export class FileSystem {
	public:
		static std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& path);


		static std::optional<std::vector<char>> read_binary(const std::filesystem::path& path);
	};


	// RAII 封装：自动释放 stbi 内存
	export class Image {
	public:
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char* pixels = nullptr;

		// 禁用拷贝，允许移动 (Move-only)
		Image() = default;
		Image(const Image&) = delete;
		Image& operator=(const Image&) = delete;

		Image(Image&& other) noexcept;

		Image& operator=(Image&& other) noexcept;

		~Image();

		bool is_valid() const;

	private:
		void cleanup();

		void move_from(Image&& other);
	};


	export class ImageLoader {
	public:
		static std::optional<Image> load(const std::filesystem::path& path);
	};


	export class ModelLoader {
	public:
		static std::optional<MeshData> load_obj(const std::filesystem::path& path);

		static std::optional<MeshData> load_gltf(const std::filesystem::path& path);
	private:
		static MeshData convert_to_mesh_data(const tinygltf::Model& model);
	};

	export class AssetManager {
	public:
		AssetManager(bud::threading::TaskScheduler* scheduler);

		void load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded);


		void load_image_async(const std::string& path, std::function<void(Image)> on_loaded);


		void load_file_async(const std::string& path, std::function<void(std::vector<char>)> on_loaded);


	private:
		bud::threading::TaskScheduler* task_scheduler;
	};
}

