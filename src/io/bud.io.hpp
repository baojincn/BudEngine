#pragma once

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

#include "src/threading/bud.threading.hpp"

#include <nlohmann/json_fwd.hpp>
#include "src/core/bud.math.hpp"
#include "src/core/bud.asset.types.hpp"

namespace bud::io {
	struct MeshSubset {
		uint32_t index_start;
		uint32_t index_count;
		uint32_t meshlet_start;
		uint32_t meshlet_count;
		uint32_t material_index;
		bud::math::AABB aabb;
	};

	struct MeshData {
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
		std::vector<MeshSubset> subsets;

		// Meshlet data
		std::vector<bud::asset::MeshletDescriptor> meshlets;
		std::vector<bud::asset::MeshletCullData> meshlet_cull_data;
		std::vector<uint32_t> meshlet_vertices;
		std::vector<uint32_t> meshlet_triangles;
	};
}

// 特化 glm::vec3 和 glm::vec2 的哈希函数
namespace std {
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

namespace bud::io {

	class VirtualFileSystem {
	public:
		VirtualFileSystem();
		~VirtualFileSystem() = default;

		std::optional<std::filesystem::path> resolve_path(const std::filesystem::path& path);
		std::optional<std::vector<char>> read_binary(const std::filesystem::path& path);
		bool write_binary(const std::filesystem::path& path, const std::vector<char>& data);
		void append_text_async(const std::filesystem::path& path, std::string text, bud::threading::Counter* counter = nullptr, bud::threading::TaskScheduler* scheduler = nullptr);
	private:
		std::filesystem::path root_directory;
	};


	// RAII 封装：自动释放 stbi 内存
	class Image {
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


	class ImageLoader {
	public:
    ImageLoader(VirtualFileSystem* virtual_file_system);
    std::optional<Image> load(const std::filesystem::path& path);

private:
    VirtualFileSystem* virtual_file_system;
	};


	class ModelLoader {
	public:
    ModelLoader(VirtualFileSystem* virtual_file_system);
    std::optional<MeshData> load_obj(const std::filesystem::path& path);
    std::optional<MeshData> load_gltf(const std::filesystem::path& path);
    std::optional<MeshData> load_bud_mesh(const std::filesystem::path& path);
	private:
		MeshData convert_to_mesh_data(const tinygltf::Model& model);

		VirtualFileSystem* virtual_file_system;
	};

	class AssetManager {
	public:
    AssetManager(VirtualFileSystem* virtual_file_system, bud::threading::TaskScheduler* scheduler);

		void load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded);
		void load_image_async(const std::string& path, std::function<void(Image)> on_loaded);
		void load_file_async(const std::string& path, std::function<void(std::vector<char>)> on_loaded);
		void load_json_async(const std::string& path, std::function<void(nlohmann::json)> on_loaded);
		void save_json_async(const std::string& path, const nlohmann::json& json, std::function<void(bool)> on_finished = nullptr);
		void save_file_async(const std::string& path, std::vector<char> data, std::function<void(bool)> on_finished = nullptr);

	private:
    VirtualFileSystem* virtual_file_system;
    bud::threading::TaskScheduler* task_scheduler;

    ImageLoader image_loader;
    ModelLoader model_loader;
	};
}

