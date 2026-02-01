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

	// [FIX] Resolve path first
	auto resolved_opt = FileSystem::resolve_path(path);
	if (!resolved_opt) {
		std::println(stderr, "[Asset] OBJ file not found: {}", path.string());
		return std::nullopt;
	}

	std::string path_str = resolved_opt->string();
	std::string base_dir = resolved_opt->parent_path().string() + "/"; // load .mtl logic needs correct base dir

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

			materialToTextureIndex[i] = static_cast<float>(meshData.texture_paths.size());
			std::println("[Asset] Material '{}' uses texture: {}", materials[i].name, texName);
		}
		else {
			materialToTextureIndex[i] = 0.0f; // 没有贴图的材质用默认图
			std::println("[Asset] Material '{}' has NO diffuse texture.", materials[i].name);
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


AssetManager::AssetManager(bud::threading::TaskScheduler* scheduler) : task_scheduler(scheduler) {}

void AssetManager::load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded) {

	task_scheduler->spawn("AsyncMeshLoad", [this, path, on_loaded]() {
		auto mesh_opt = ModelLoader::load_obj(path);

		if (mesh_opt) {
			std::println("[Asset] Mesh parsed: {} (v:{}, i:{})", path, mesh_opt->vertices.size(), mesh_opt->indices.size());

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
			std::println("[Asset] Image decoded: {} ({}x{})", path, img_opt->width, img_opt->height);

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


