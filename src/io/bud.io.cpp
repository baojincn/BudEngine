#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <optional>
#include <functional>

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

#include "src/io/bud.io.hpp"
#include "src/threading/bud.threading.hpp"

using namespace bud::io;

bool MeshData::Vertex::operator==(const Vertex& other) const {
	return pos == other.pos &&
		color == other.color &&
		normal == other.normal &&
		texture_uv == other.texture_uv &&
		texture_index == other.texture_index;
}

std::optional<std::filesystem::path> FileSystem::resolve_path(const std::filesystem::path& path) {
	auto check_exists = [](const std::filesystem::path& p) {
		std::error_code ec;
		return std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec);
		};

	if (check_exists(path)) return path;
	if (check_exists(std::filesystem::path("../") / path)) {
		auto p = std::filesystem::path("../") / path;
		std::println("[IO] Resolved path via fallback: {}", p.string());
		return p;
	}
	if (check_exists(std::filesystem::path("../../") / path)) {
		auto p = std::filesystem::path("../../") / path;
		std::println("[IO] Resolved path via fallback: {}", p.string());
		return p;
	}
	return std::nullopt;
}

// 通用二进制读取 (用于 Shader (SPV), Buffer 等)
std::optional<std::vector<char>> FileSystem::read_binary(const std::filesystem::path& path) {
	auto resolved_path = resolve_path(path);
	if (!resolved_path) {
		std::println(stderr, "[IO] Failed to find file: {}", path.string());
		std::println(stderr, "[IO] CWD: {}", std::filesystem::current_path().string());
		return std::nullopt;
	}

	std::ifstream file(*resolved_path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) return std::nullopt;

	size_t file_size = (size_t)file.tellg();
	std::vector<char> buffer(file_size);
	file.seekg(0);
	file.read(buffer.data(), file_size);
	return buffer;
}



Image::Image(Image&& other) noexcept {
	move_from(std::move(other));
}

Image& Image::operator=(Image&& other) noexcept {
	if (this != &other) {
		cleanup();
		move_from(std::move(other));
	}
	return *this;
}

Image::~Image() {
	cleanup();
}

bool Image::is_valid() const {
	return pixels != nullptr;
}

void Image::cleanup() {
	if (pixels) {
		stbi_image_free(pixels);
		pixels = nullptr;
	}
}


void Image::move_from(Image&& other) {
	width = other.width;
	height = other.height;
	channels = other.channels;
	pixels = other.pixels;
	other.pixels = nullptr; // 接管所有权
}


std::optional<Image> ImageLoader::load(const std::filesystem::path& path) {
	Image img;

	// [FIX] Use resolve_path
	auto resolved_opt = FileSystem::resolve_path(path);
	if (!resolved_opt) {
		std::println(stderr, "[IO] Image not found: {}", path.string());
		return std::nullopt;
	}
	std::string path_str = resolved_opt->string();

	// 强制加载为 RGBA (4通道)，Vulkan 友好
	img.pixels = stbi_load(path_str.c_str(), &img.width, &img.height, &img.channels, STBI_rgb_alpha);

	if (!img.pixels) {
		std::println(stderr, "[IO] Failed to load image: {}", path_str);
		return std::nullopt;
	}

	img.channels = 4;
	return std::move(img);
}


// ==========================================
// 3. Model Loader (网格模型)
// ==========================================

std::optional<MeshData> ModelLoader::load_obj(const std::filesystem::path& path) {
	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string warn, err;

	auto base_dir = path.parent_path().string() + "/";
	bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, 
	                            path.string().c_str(), base_dir.c_str());

	if (!warn.empty() && warn.find("Both") == std::string::npos)
		std::println("[TinyOBJ Warn]: {}", warn);
	if (!err.empty())
		std::println("[TinyOBJ Error]: {}", err);
	if (!ret)
		return std::nullopt;

	MeshData mesh_data;

	const std::string default_texture = "data/textures/default.png";

	std::println("[OBJ Parser] Loading {} materials...", materials.size());
	for (size_t mat_idx = 0; mat_idx < materials.size(); mat_idx++) {
		auto tex_name = materials[mat_idx].diffuse_texname;
		std::replace(tex_name.begin(), tex_name.end(), '\\', '/');

		if (!tex_name.empty()) {
			if (std::filesystem::path(tex_name).is_relative()) {
				tex_name = base_dir + tex_name;
			}
			mesh_data.texture_paths.push_back(tex_name);
			//std::println("  Material[{}] -> {}", mat_idx, tex_name);
		}
		else {
			mesh_data.texture_paths.push_back(default_texture);
			std::println("  Material[{}] -> (empty/fallback)", mat_idx);
		}
	}

	std::unordered_map<MeshData::Vertex, uint32_t> unique_vertices{};

	int32_t current_material_id = -1;
	uint32_t current_index_start = 0;
	uint32_t current_index_count = 0;

	auto flush_subset = [&](int32_t next_mat_id) {
		if (current_index_count > 0) {
			uint32_t safe_mat_idx = (current_material_id < 0) ? 0 : static_cast<uint32_t>(current_material_id);

			if (safe_mat_idx >= mesh_data.texture_paths.size()) {
				//std::println(stderr, 
				//	"[OBJ Parser ERROR] Material {} out of range! "
				//	"(Max: {})", 
				//	safe_mat_idx, mesh_data.texture_paths.size());
				safe_mat_idx = 0;
			}

			MeshSubset subset;
			subset.index_start = current_index_start;
			subset.index_count = current_index_count;
			subset.material_index = safe_mat_idx;

			mesh_data.subsets.push_back(subset);

			current_index_start += current_index_count;
			current_index_count = 0;
		}
		current_material_id = next_mat_id;
	};


	for (const auto& shape : shapes) {
		size_t index_offset = 0;
		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
			int32_t mat_id = shape.mesh.material_ids[f];

			if (mat_id != current_material_id) {
				//if (f % 1000 == 0) {  // 每1000个面打印一次，避免过多日志
				//	std::println("[OBJ Parser] Face {}: Material {} -> {}", 
				//		f, current_material_id, mat_id);
				//}
				flush_subset(mat_id);
			}

			int fv = shape.mesh.num_face_vertices[f];
			for (size_t v = 0; v < fv; v++) {
				tinyobj::index_t idx = shape.mesh.indices[index_offset + v];

				MeshData::Vertex vertex{};

				vertex.pos = {
					attrib.vertices[3 * idx.vertex_index + 0],
					attrib.vertices[3 * idx.vertex_index + 1],
					attrib.vertices[3 * idx.vertex_index + 2]
				};

				if (!attrib.colors.empty()) {
					vertex.color = {
						attrib.colors[3 * idx.vertex_index + 0],
						attrib.colors[3 * idx.vertex_index + 1],
						attrib.colors[3 * idx.vertex_index + 2]
					};
				}
				else {
					vertex.color = { 1.0f, 1.0f, 1.0f };
				}

				if (idx.normal_index >= 0) {
					vertex.normal = {
						attrib.normals[3 * idx.normal_index + 0],
						attrib.normals[3 * idx.normal_index + 1],
						attrib.normals[3 * idx.normal_index + 2]
					};
				}

				if (idx.texcoord_index >= 0) {
					vertex.texture_uv = {
						attrib.texcoords[2 * idx.texcoord_index + 0],
						1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]
					};
				}

				if (unique_vertices.count(vertex) == 0) {
					unique_vertices[vertex] = static_cast<uint32_t>(mesh_data.vertices.size());
					mesh_data.vertices.push_back(vertex);
				}

				mesh_data.indices.push_back(unique_vertices[vertex]);
				current_index_count++;
			}

			index_offset += fv;
		}
	}

	// 提交最后一个子网格
	flush_subset(current_material_id);

	//std::println("\n[OBJ Loader Summary]");
	//std::println("  File: {}", path.string());
	//std::println("  Vertices: {}", mesh_data.vertices.size());
	//std::println("  Indices: {}", mesh_data.indices.size());
	//std::println("  Materials (textures): {}", mesh_data.texture_paths.size());
	//std::println("  Subsets: {}", mesh_data.subsets.size());
	
	if (mesh_data.subsets.empty() && !mesh_data.indices.empty()) {
		std::println(stderr, "  ⚠️ WARNING: No subsets found! Creating fallback...");
		MeshSubset fallback;
		fallback.index_start = 0;
		fallback.index_count = (uint32_t)mesh_data.indices.size();
		fallback.material_index = 0;
		mesh_data.subsets.push_back(fallback);
	}


	return mesh_data;
}

std::optional<MeshData> ModelLoader::load_gltf(const std::filesystem::path& path) {
	tinygltf::Model model;
	tinygltf::TinyGLTF loader;
	std::string err;
	std::string warn;
	auto path_str = path.string();
	auto ret = false;

	if (path.extension() == ".glb")
		ret = loader.LoadBinaryFromFile(&model, &err, &warn, path_str);
	else
		ret = loader.LoadASCIIFromFile(&model, &err, &warn, path_str);

	if (!warn.empty())
		std::println("[glTF Warn]: {}", warn);

	if (!err.empty())
		std::println("[glTF Error]: {}", err);

	if (!ret)
		return std::nullopt;

	return convert_to_mesh_data(model);
}


MeshData ModelLoader::convert_to_mesh_data(const tinygltf::Model& model) {
	MeshData meshData;

	if (model.meshes.empty())
		return meshData;

	const auto& gltfMesh = model.meshes[0];

	if (gltfMesh.primitives.empty())
		return meshData;

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
		if (positionBuffer)
			vertex.pos = glm::vec3(positionBuffer[i * 3 + 0], positionBuffer[i * 3 + 1], positionBuffer[i * 3 + 2]);

		vertex.color = glm::vec3(1.0f, 1.0f, 1.0f);

		if (texCoordBuffer)
			vertex.texture_uv = glm::vec2(texCoordBuffer[i * 2 + 0], texCoordBuffer[i * 2 + 1]);

		meshData.vertices.push_back(vertex);
	}

	if (primitive.indices >= 0) {
		const auto& accessor = model.accessors[primitive.indices];
		const auto& bufferView = model.bufferViews[accessor.bufferView];
		const auto& buffer = model.buffers[bufferView.buffer];
		const void* dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

		if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
			const uint32_t* buf = static_cast<const uint32_t*>(dataPtr);
			for (size_t i = 0; i < accessor.count; i++)
				meshData.indices.push_back(buf[i]);
		}
		else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
			const uint16_t* buf = static_cast<const uint16_t*>(dataPtr);
			for (size_t i = 0; i < accessor.count; i++)
				meshData.indices.push_back(buf[i]);
		}
		else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
			const uint8_t* buf = static_cast<const uint8_t*>(dataPtr);
			for (size_t i = 0; i < accessor.count; i++)
				meshData.indices.push_back(buf[i]);
		}
	}

	return meshData;
}


AssetManager::AssetManager(bud::threading::TaskScheduler* scheduler) : task_scheduler(scheduler) {}

void AssetManager::load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded) {

	task_scheduler->spawn("AsyncMeshLoad", [this, path, on_loaded]() {
		auto mesh_opt = ModelLoader::load_obj(path);

		if (mesh_opt) {
			//std::println("[Asset] Mesh parsed: {} (v:{}, i:{})", path, mesh_opt->vertices.size(), mesh_opt->indices.size());
			task_scheduler->submit_main_thread_task([on_loaded, mesh = std::move(*mesh_opt)]() mutable {
				on_loaded(std::move(mesh));
				});
		}
		else {
			std::println(stderr, "[Asset] Failed to load mesh: {}", path);
		}
	});
}


void AssetManager::load_image_async(const std::string& path, std::function<void(Image)> on_loaded) {
	task_scheduler->spawn("AsyncImageLoad", [this, path, on_loaded]() {
		// Fiber Thread 操作：IO + STB 解码
		auto img_opt = ImageLoader::load(path);

		if (img_opt) {
			//std::println("[Asset] Image decoded: {} ({}x{})", path, img_opt->width, img_opt->height);

			task_scheduler->submit_main_thread_task([on_loaded, img = std::move(*img_opt)]() mutable {
				on_loaded(std::move(img));
				});
		}
		else {
			std::println(stderr, "[Asset] Failed to load image: {}", path);
		}
	});
}


void AssetManager::load_file_async(const std::string& path, std::function<void(std::vector<char>)> on_loaded) {
	task_scheduler->spawn("AsyncFileLoad", [this, path, on_loaded]() {
		auto data_opt = FileSystem::read_binary(path);
		if (data_opt) {
			task_scheduler->submit_main_thread_task([on_loaded, data = std::move(*data_opt)]() mutable {
				on_loaded(std::move(data));
				});
		}
		else {
			std::println(stderr, "[Asset] Failed to read file: {}", path);
		}
	});
}


