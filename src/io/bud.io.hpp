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
		uint32_t material_index;
		bud::math::AABB aabb;
	};

	struct MeshData {

		struct Material {
			uint32_t base_color_texture = 0; // index into texture_paths
			uint32_t normal_texture = 0xFFFFFFFF;
			uint32_t metallic_roughness_texture = 0xFFFFFFFF;
			uint32_t emissive_texture = 0xFFFFFFFF;
			glm::vec4 base_color_factor = glm::vec4(1.0f);
			float metallic_factor = 0.0f;
			float roughness_factor = 0.5f;
			uint8_t alpha_mode = 0; // 0=OPAQUE,1=MASK,2=BLEND
			uint8_t double_sided = 0;
			float alpha_cutoff = 0.5f;
			std::string name;
		};

		struct Vertex {
			glm::vec3 pos;
			glm::vec3 color;
			glm::vec3 normal;
			glm::vec2 texture_uv;
			float texture_index;

			bool operator==(const Vertex& other) const;
		};

		std::string source_path;
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<std::string> texture_paths;
		std::vector<Material> materials;
		std::vector<MeshSubset> subsets;
		bool has_virtual_geometry = false;
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
		void write_text_async(const std::filesystem::path& path, std::string text, bud::threading::Counter* counter = nullptr, bud::threading::TaskScheduler* scheduler = nullptr);
		std::filesystem::path get_root_path() const { return root_path; }
		std::optional<nlohmann::json> read_json(const std::filesystem::path& path);
		bool                          write_json(const std::filesystem::path& path, const nlohmann::json& json);

	private:
		std::filesystem::path root_path;
	};


	class Image {
	public:
		int width = 0;
		int height = 0;
		int channels = 0;
		unsigned char* pixels = nullptr;

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
		std::optional<MeshData> load_bud_asset(const std::filesystem::path& path);

	private:
		VirtualFileSystem* virtual_file_system;
	};

	struct BudAssetPackage {
		std::string path;
		bud::asset::BudAssetHeader header{};
		std::vector<bud::asset::AssetChunkEntry> chunks;

		const bud::asset::AssetChunkEntry* find_chunk(bud::asset::AssetChunkType type) const {
			for (const auto& c : chunks) {
				if (c.chunk_type == static_cast<uint32_t>(type))
					return &c;
			}
			return nullptr;
		}
	};

	class AssetManager {
	public:
    AssetManager(VirtualFileSystem* virtual_file_system, bud::threading::TaskScheduler* scheduler);

		void load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded);
		void load_image_async(const std::string& path, std::function<void(Image)> on_loaded);
		void load_file_async(const std::string& path, std::function<void(std::vector<char>)> on_loaded);
		void load_file_chunk_async(const std::string& path, uint64_t offset, uint64_t size,
			std::function<void(std::vector<char>)> on_loaded);
		void load_budasset_async(const std::string& path, std::function<void(std::shared_ptr<BudAssetPackage>)> on_loaded);
		void load_budasset_chunk_async(std::shared_ptr<BudAssetPackage> package, bud::asset::AssetChunkType type,
			std::function<void(std::vector<char>)> on_loaded);
		void load_json_async(const std::string& path, std::function<void(nlohmann::json)> on_loaded);
		void save_json_async(const std::string& path, const nlohmann::json& json, std::function<void(bool)> on_finished = nullptr);
		void save_file_async(const std::string& path, std::vector<char> data, std::function<void(bool)> on_finished = nullptr);

		VirtualFileSystem* get_vfs() { return virtual_file_system; }

		bool save_json_sync(const std::string& path, const nlohmann::json& json);
		std::optional<nlohmann::json> load_json_sync(const std::string& path);

	private:
		VirtualFileSystem* virtual_file_system;
		bud::threading::TaskScheduler* task_scheduler;

		ImageLoader image_loader;
		ModelLoader model_loader;
	};
}

