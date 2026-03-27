#include "bud.io.hpp"
#include "src/core/bud.core.hpp"
#include "src/core/bud.asset.types.hpp"
#include <fstream>
#include <filesystem>
#include <optional>
#include <iostream>
#include <algorithm>
#include <format>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <nlohmann/json.hpp>

#include <tiny_obj_loader.h>
#include <stb_image.h>
#include "src/core/bud.logger.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace bud::io {

	bool MeshData::Vertex::operator==(const Vertex& other) const {
		return pos == other.pos &&
			color == other.color &&
			normal == other.normal &&
			texture_uv == other.texture_uv &&
			texture_index == other.texture_index;
	}

	VirtualFileSystem::VirtualFileSystem() {
		// Determine a sensible root directory for the VFS at startup.
		std::error_code ec;
#if defined(_WIN32)
		wchar_t buf[MAX_PATH];
		DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
		if (len != 0) {
			root_directory = std::filesystem::path(std::wstring(buf, buf + len)).parent_path();
		}
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		if (size != 0) {
			std::string buf(size, '\0');
			if (_NSGetExecutablePath(buf.data(), &size) == 0) {
				root_directory = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec).parent_path();
			}
		}
#else
		std::string buf(4096, '\0');
		ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size());
		if (len > 0) {
			buf.resize(static_cast<size_t>(len));
			root_directory = std::filesystem::path(buf).parent_path();
		}
#endif

		if (root_directory.empty()) {
			// Fallback to current working directory
			root_directory = std::filesystem::current_path(ec);
		}

		bud::print("[IO] VirtualFileSystem initialized. root={}", root_directory.string());
	}

	void VirtualFileSystem::append_text_async(const std::filesystem::path& path, std::string text, bud::threading::Counter* counter, bud::threading::TaskScheduler* scheduler) {
		auto parent = path.parent_path();
		if (!parent.empty())
			std::filesystem::create_directories(parent);


		bud::threading::TaskScheduler* use_scheduler = scheduler ? scheduler : bud::threading::t_scheduler;

		if (use_scheduler) {
			use_scheduler->spawn("IO.Append", [p = path.string(), text = std::move(text)]() mutable {
				std::ofstream f(p, std::ios::app);
				if (f) {
					f << text << '\n';
					f.flush();
				}
				},
				counter);
		}

		std::ofstream f(path, std::ios::app);
		if (f) {
			f << text << '\n';
			f.flush();
		}

	}

	std::optional<std::filesystem::path> VirtualFileSystem::resolve_path(const std::filesystem::path& path) {
		auto check_exists = [](const std::filesystem::path& p) {
			std::error_code ec;
			return std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec);
			};

		// Helper to normalize returned paths to an absolute/weakly canonical form
		auto normalize = [](const std::filesystem::path& p) {
			std::error_code ec;
			try {
				auto r = std::filesystem::weakly_canonical(p, ec);
				if (!ec) return r;
			}
			catch (...) {}
			std::error_code ec2;
			return std::filesystem::absolute(p, ec2);
			};

		// If the provided path is absolute and exists, return it normalized
		if (path.is_absolute() && check_exists(path))
			return normalize(path);

		// Prefer resolving relative paths against the configured root_directory
		if (!root_directory.empty()) {
			auto candidate = root_directory / path;
			if (check_exists(candidate))
				return normalize(candidate);
			// Not found under root_directory: report and fail fast
			bud::eprint("[IO] {} doesn't exist", candidate.string());
			return std::nullopt;
		}

		// As a last resort, try current working directory
		auto cwd_candidate = std::filesystem::current_path() / path;
		if (check_exists(cwd_candidate))
			return normalize(cwd_candidate);

		bud::eprint("[IO] Failed to resolve {}: no root_directory configured and not found in CWD", path.string());
		return std::nullopt;
	}

	// 通用二进制读取 (用于 Shader (SPV), Buffer 等)
	std::optional<std::vector<char>> VirtualFileSystem::read_binary(const std::filesystem::path& path) {
		auto resolved_path_opt = resolve_path(path);
		if (!resolved_path_opt) {
			bud::eprint("[IO] read_binary: failed to resolve path: {}", path.string());
			return std::nullopt;
		}

		std::filesystem::path resolved_path = *resolved_path_opt;
		bud::print("[IO] Open binary file: {}", resolved_path.string());

		std::ifstream file;
		file.open(resolved_path, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			// Check if path is a regular file and report errno for more info
			std::error_code ec;
			bool exists = std::filesystem::exists(resolved_path, ec);
			bool is_reg = std::filesystem::is_regular_file(resolved_path, ec);
			int saved_errno = errno;
			bud::eprint("[IO] Failed to open file after resolution: {} (exists={}, is_regular={}, err={})",
				resolved_path.string(), exists, is_reg, saved_errno ? std::strerror(saved_errno) : "0");
			return std::nullopt;
		}

		size_t file_size = (size_t)file.tellg();

		// Treat zero-sized files as an error for resource loading: log and return nullopt
		if (file_size == 0) {
			bud::eprint("[IO] File is empty: {}", resolved_path.string());
			return std::nullopt;
		}

		std::vector<char> buffer(file_size);
		file.seekg(0);
		file.read(buffer.data(), file_size);
		if (!file) {
			bud::eprint("[IO] Failed to read file contents: {}", resolved_path.string());
			return std::nullopt;
		}

		return buffer;
	}

	bool VirtualFileSystem::write_binary(const std::filesystem::path& path, const std::vector<char>& data) {
		// Ensure the parent directory exists
		auto parent_path = path.parent_path();
		if (!parent_path.empty() && !std::filesystem::exists(parent_path)) {
			std::filesystem::create_directories(parent_path);
		}

		// Write to a temporary file first
		std::filesystem::path temp_path = path;
		temp_path.replace_extension(path.extension().string() + ".tmp");

		{
			std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				bud::eprint("[IO] Failed to open temp file for writing: {}", temp_path.string());
				return false;
			}
			file.write(data.data(), data.size());
			file.flush();
		}

		// Atomic rename (on most filesystems)
		std::filesystem::rename(temp_path, path);
		return true;
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


	ImageLoader::ImageLoader(VirtualFileSystem* virtual_file_system)
		: virtual_file_system(virtual_file_system) {
	}


	std::optional<Image> ImageLoader::load(const std::filesystem::path& path) {
		Image img;
		auto resolved_opt = virtual_file_system->resolve_path(path);
		if (!resolved_opt) {
			bud::eprint("[IO] Image not found: {} (could not resolve)", path.string());
			return std::nullopt;
		}
		std::string path_str = resolved_opt->string();
		img.pixels = stbi_load(path_str.c_str(), &img.width, &img.height, &img.channels, STBI_rgb_alpha);
		if (!img.pixels) {
			const char* reason = stbi_failure_reason();
			bud::eprint("[IO] Failed to load image: {} (stb reason: {})", path_str, reason ? reason : "unknown");
			return std::nullopt;
		}
		img.channels = 4;
		return std::move(img);
	}


	// ==========================================
	// 3. Model Loader (网格模型)
	// ==========================================

	ModelLoader::ModelLoader(VirtualFileSystem* virtual_file_system)
		: virtual_file_system(virtual_file_system) {
	}


	std::optional<MeshData> ModelLoader::load_obj(const std::filesystem::path& path) {
		auto resolved_opt = virtual_file_system->resolve_path(path);
		if (!resolved_opt) {
			bud::eprint("[IO] OBJ file not found: {}", path.string());
			return std::nullopt;
		}
		std::filesystem::path resolved_path = *resolved_opt;

		tinyobj::attrib_t attrib;
		std::vector<tinyobj::shape_t> shapes;
		std::vector<tinyobj::material_t> materials;
		std::string warn, err;

		auto base_dir = resolved_path.parent_path().string() + "/";
		bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
			resolved_path.string().c_str(), base_dir.c_str());

		if (!warn.empty() && warn.find("Both") == std::string::npos)
			bud::print("[IO] [TinyOBJ Warn]: {}", warn);
		if (!err.empty())
			bud::print("[IO] [TinyOBJ Error]: {}", err);
		if (!ret)
		{
			bud::eprint("[IO] TinyOBJ failed to load OBJ: {}", resolved_path.string());
			return std::nullopt;
		}

		MeshData mesh_data;

		const std::string default_texture = "data/textures/default.png";

		bud::print("[IO] [OBJ Parser] Loading {} materials...", materials.size());
		for (size_t mat_idx = 0; mat_idx < materials.size(); mat_idx++) {
			auto tex_name = materials[mat_idx].diffuse_texname;
			std::replace(tex_name.begin(), tex_name.end(), '\\', '/');

			if (!tex_name.empty()) {
				if (std::filesystem::path(tex_name).is_relative()) {
					tex_name = base_dir + tex_name;
				}
				mesh_data.texture_paths.push_back(tex_name);
			}
			else {
				mesh_data.texture_paths.push_back(default_texture);
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

		flush_subset(current_material_id);

		if (mesh_data.subsets.empty() && !mesh_data.indices.empty()) {
			bud::eprint("[IO] WARNING: No subsets found! Creating fallback...");
			MeshSubset fallback;
			fallback.index_start = 0;
			fallback.index_count = (uint32_t)mesh_data.indices.size();
			fallback.material_index = 0;
			mesh_data.subsets.push_back(fallback);
		}


		return mesh_data;
	}

	std::optional<MeshData> ModelLoader::load_gltf(const std::filesystem::path& path) {
		auto resolved_opt = virtual_file_system->resolve_path(path);
		if (!resolved_opt) {
			bud::eprint("[IO] glTF file not found: {}", path.string());
			return std::nullopt;
		}
		auto path_str = resolved_opt->string();

		tinygltf::Model model;
		tinygltf::TinyGLTF loader;
		std::string err;
		std::string warn;
		auto ret = false;

		if (resolved_opt->extension() == ".glb")
			ret = loader.LoadBinaryFromFile(&model, &err, &warn, path_str);
		else
			ret = loader.LoadASCIIFromFile(&model, &err, &warn, path_str);

		if (!warn.empty())
			bud::print("[IO] [glTF Warn]: {}", warn);

		if (!err.empty())
			bud::print("[IO] [glTF Error]: {}", err);

		if (!ret)
		{
			bud::eprint("[IO] glTF loader failed for: {} (err: {})", path_str, err);
			return std::nullopt;
		}

		return convert_to_mesh_data(model);
	}


	MeshData ModelLoader::convert_to_mesh_data(const tinygltf::Model& model) {
		MeshData meshData;

		if (model.meshes.empty()) {
			std::string err = "ModelLoader::convert_to_mesh_data called with empty glTF model.meshes";
			bud::eprint("[IO] {}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return meshData;
#endif
		}

		const auto& gltfMesh = model.meshes[0];

		if (gltfMesh.primitives.empty()) {
			std::string err = "ModelLoader::convert_to_mesh_data gltf mesh has no primitives";
			bud::eprint("[IO] {}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return meshData;
#endif
		}

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

	std::optional<MeshData> ModelLoader::load_bud_mesh(const std::filesystem::path& path) {
		// Resolve path first so all diagnostics use the fully resolved path
		auto resolved_opt = virtual_file_system->resolve_path(path);
		std::string display_path = path.string();
		if (!resolved_opt) {
			bud::eprint("[IO] .budmesh file not found: {}", path.string());
			return std::nullopt;
		}
		else {
			display_path = resolved_opt->string();
		}

		// Use unified logging helpers so output gets the global prefix/backend handling.
		bud::print("[IO] load_bud_mesh: {}", display_path);

		auto data_opt = virtual_file_system->read_binary(*resolved_opt);
		if (!data_opt) {
			bud::eprint("[IO] Failed to read .budmesh binary data: {}", display_path);
#if defined(_DEBUG)
			throw std::runtime_error("Failed to read .budmesh binary data");
#else
			return std::nullopt;
#endif
		}

		const char* ptr = data_opt->data();
		size_t data_size = data_opt->size();
		const asset::BudMeshHeader* header = reinterpret_cast<const asset::BudMeshHeader*>(ptr);

		if (header->magic != asset::MESH_MAGIC) {
			bud::eprint("[IO] Invalid .budmesh magic: {}", display_path);
			return std::nullopt;
		}

		if (header->version < 2) {
			bud::eprint("[IO] Version too old: {}, expected at least 2, got {}", display_path, header->version);
			return std::nullopt;
		}

		static_assert(sizeof(asset::BudMeshHeader) == asset::MESH_HEADER_SIZE, "BudMeshHeader size mismatch!");
		static_assert(offsetof(asset::BudMeshHeader, vertex_offset) == asset::MESH_HEADER_VERTEX_OFFSET, "BudMeshHeader alignment mismatch!");
		static_assert(offsetof(asset::BudMeshHeader, submesh_count) == asset::MESH_HEADER_SUBMESH_COUNT_OFFSET, "BudMeshHeader submesh_count offset mismatch!");

		bud::print("[IO] sizeof(Header)={}, sizeof(Vertex)={}", sizeof(asset::BudMeshHeader), sizeof(asset::Vertex));
		bud::print("[IO] .budmesh: {}, size={}, v_count={}, i_count={}, m_count={}, s_count=",
			display_path, data_size, header->total_vertices, header->total_indices, header->meshlet_count, header->submesh_count);

		auto check_offset = [&](uint64_t offset, size_t section_size, const char* name) {
			if (offset + section_size > data_size) {
				bud::eprint("[IO] Offset out of bounds: {} (offset={}, section_size={}, total={})", name, offset, section_size, data_size);
				return false;
			}
			return true;
			};

		if (!check_offset(header->vertex_offset, header->total_vertices * sizeof(asset::Vertex), "Vertices") ||
			!check_offset(header->index_offset, header->total_indices * sizeof(uint32_t), "Indices") ||
			!check_offset(header->meshlet_offset, header->meshlet_count * sizeof(asset::MeshletDescriptor), "Meshlets") ||
			!check_offset(header->vertex_index_offset, header->meshlet_index_offset - header->vertex_index_offset, "MeshletVertices") ||
			!check_offset(header->meshlet_index_offset, header->cull_data_offset - header->meshlet_index_offset, "MeshletTriangles") ||
			!check_offset(header->cull_data_offset, header->meshlet_count * sizeof(asset::MeshletCullData), "CullData") ||
			(header->version >= 2 && !check_offset(header->submesh_offset, header->submesh_count * sizeof(asset::SubMeshDescriptor), "Submeshes")) ||
			(header->version >= 3 && !check_offset(header->texture_offset, 0, "Textures"))) { // check_offset 0 just for existence of start
			bud::eprint("[IO] .budmesh validation failed for: {}", display_path);
			return std::nullopt;
		}

		MeshData mesh;

		// 1. Map Source Vertices
		const asset::Vertex* src_vertices = reinterpret_cast<const asset::Vertex*>(ptr + header->vertex_offset);

		// 2. Map Meshlet and Submesh Data
		bud::print("[IO] Offsets: v={}, i={}, m={}, s={}", header->vertex_offset, header->index_offset, header->meshlet_offset, header->submesh_offset);
		const asset::MeshletDescriptor* descriptors = reinterpret_cast<const asset::MeshletDescriptor*>(ptr + header->meshlet_offset);
		const uint32_t* meshlet_vertices = reinterpret_cast<const uint32_t*>(ptr + header->vertex_index_offset);
		const uint32_t* meshlet_triangles = reinterpret_cast<const uint32_t*>(ptr + header->meshlet_index_offset);
		const asset::SubMeshDescriptor* submesh_descs = reinterpret_cast<const asset::SubMeshDescriptor*>(ptr + header->submesh_offset);

		// 3. Fast load buffers (Convert back to engine-friendly Vertex structure)
		mesh.vertices.resize(header->total_vertices);
		for (uint32_t i = 0; i < header->total_vertices; ++i) {
			MeshData::Vertex v;
			v.pos = { src_vertices[i].position[0], src_vertices[i].position[1], src_vertices[i].position[2] };
			v.normal = { src_vertices[i].normal[0], src_vertices[i].normal[1], src_vertices[i].normal[2] };
			v.texture_uv = { src_vertices[i].uv[0], src_vertices[i].uv[1] };
			v.color = { 1.0f, 1.0f, 1.0f };
			v.texture_index = 0.0f;
			mesh.vertices[i] = v;
		}

		const uint32_t* src_indices = reinterpret_cast<const uint32_t*>(ptr + header->index_offset);
		mesh.indices.assign(src_indices, src_indices + header->total_indices);

		// Map Meshlet and Submesh Data
		const asset::MeshletDescriptor* meshlet_descs = reinterpret_cast<const asset::MeshletDescriptor*>(ptr + header->meshlet_offset);
		const uint32_t* mv_ptr = reinterpret_cast<const uint32_t*>(ptr + header->vertex_index_offset);
		const uint32_t* mt_ptr = reinterpret_cast<const uint32_t*>(ptr + header->meshlet_index_offset);
		const asset::MeshletCullData* mc_ptr = reinterpret_cast<const asset::MeshletCullData*>(ptr + header->cull_data_offset);
		const asset::SubMeshDescriptor* sm_ptr = reinterpret_cast<const asset::SubMeshDescriptor*>(ptr + header->submesh_offset);

		mesh.meshlets.assign(meshlet_descs, meshlet_descs + header->meshlet_count);
		mesh.meshlet_cull_data.assign(mc_ptr, mc_ptr + header->meshlet_count);

		uint32_t mv_count = (uint32_t)((header->meshlet_index_offset - header->vertex_index_offset) / sizeof(uint32_t));
		uint32_t mt_count = (uint32_t)((header->cull_data_offset - header->meshlet_index_offset) / sizeof(uint32_t));
		mesh.meshlet_vertices.assign(mv_ptr, mv_ptr + mv_count);
		mesh.meshlet_triangles.assign(mt_ptr, mt_ptr + mt_count);

		for (uint32_t s = 0; s < header->submesh_count; ++s) {
			const auto& sub = sm_ptr[s];
			MeshSubset subset;
			subset.index_start = sub.index_start;
			subset.index_count = sub.index_count;
			subset.meshlet_start = sub.meshlet_start;
			subset.meshlet_count = sub.meshlet_count;
			subset.material_index = sub.material_id;
			subset.aabb = bud::math::AABB(
				bud::math::vec3(sub.aabb_min[0], sub.aabb_min[1], sub.aabb_min[2]),
				bud::math::vec3(sub.aabb_max[0], sub.aabb_max[1], sub.aabb_max[2])
			);
			mesh.subsets.push_back(subset);
		}

		bud::print("[IO] Loaded mesh: {} (v={}, i={}, m={}, s={})", display_path, (uint32_t)mesh.vertices.size(), (uint32_t)mesh.indices.size(), (uint32_t)mesh.meshlets.size(), (uint32_t)mesh.subsets.size());

		// Texture paths
		if (header->version >= 3 && header->texture_count > 0) {
			mesh.texture_paths.clear();
			const char* tex_ptr = ptr + header->texture_offset;
			for (uint32_t t = 0; t < header->texture_count; ++t) {
				std::string tex_path(tex_ptr);
				// Fix backslashes
				std::replace(tex_path.begin(), tex_path.end(), '\\', '/');

				// If it's just a filename, we might need a base path?
				// But for Sponza, we should have exported them correctly in Tool.
				mesh.texture_paths.push_back(tex_path);
				tex_ptr += tex_path.length() + 1;
			}
		}
		else {
			mesh.texture_paths.push_back("data/textures/default.png");
		}

		return mesh;
	}


	AssetManager::AssetManager(VirtualFileSystem* virtual_file_system, bud::threading::TaskScheduler* scheduler)
		: virtual_file_system(virtual_file_system), task_scheduler(scheduler), image_loader(virtual_file_system), model_loader(virtual_file_system) {
	}

	void AssetManager::load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded) {
		task_scheduler->spawn("AsyncMeshLoad", [this, path, on_loaded]() {
			std::optional<MeshData> mesh_opt;

			std::string path_lower = path;
			std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);

			if (path_lower.ends_with(".budmesh")) {
				mesh_opt = this->model_loader.load_bud_mesh(path);
			}
			else if (path_lower.ends_with(".gltf") || path_lower.ends_with(".glb")) {
				mesh_opt = this->model_loader.load_gltf(path);
			}
			else {
				mesh_opt = this->model_loader.load_obj(path);
			}


			if (mesh_opt) {
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::print("[IO] Loaded mesh (resolved): {}", resolved->string());
				}
				else {
					bud::print("[IO] Loaded mesh: {}", path);
				}

				task_scheduler->submit_main_thread_task([on_loaded, mesh = std::move(*mesh_opt)]() mutable {
					on_loaded(std::move(mesh));
					});
			}
			else {
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::eprint("[Asset] Failed to load mesh: {} (resolved: {})", path, resolved->string());
				}
				else {
					bud::eprint("[Asset] Failed to load mesh: {} (could not resolve)", path);
				}
			}
			});
	}


	void AssetManager::load_image_async(const std::string& path, std::function<void(Image)> on_loaded) {
		task_scheduler->spawn("AsyncImageLoad", [this, path, on_loaded]() {
			auto img_opt = this->image_loader.load(path);

			if (img_opt) {
				// Log resolved path for successful image loads
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::print("[IO] Loaded image (resolved): {}", resolved->string());
				}
				else {
					bud::print("[IO] Loaded image: {}", path);
				}

				task_scheduler->submit_main_thread_task([on_loaded, img = std::move(*img_opt)]() mutable {
					on_loaded(std::move(img));
					});
			}
			else {
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::eprint("[Asset] Failed to load image: {} (resolved: {})", path, resolved->string());
				}
				else {
					bud::eprint("[Asset] Failed to load image: {} (could not resolve)", path);
				}
			}
			});
	}


	void AssetManager::load_file_async(const std::string& path, std::function<void(std::vector<char>)> on_loaded) {
		task_scheduler->spawn("AsyncFileLoad", [this, path, on_loaded]() {
			auto data_opt = this->virtual_file_system->read_binary(path);
			if (data_opt) {
				// Log resolved path for successful file reads
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::print("[IO] Loaded file (resolved): {}", resolved->string());
				}
				else {
					bud::print("[IO] Loaded file: {}", path);
				}

				task_scheduler->submit_main_thread_task([on_loaded, data = std::move(*data_opt)]() mutable {
					on_loaded(std::move(data));
					});
			}
			else {
				// Attempt to resolve and report the absolute/resolved path for better diagnostics
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::eprint("[Asset] Failed to read file: {} (resolved: {})", path, resolved->string());
				}
				else {
					bud::eprint("[Asset] Failed to read file: {} (could not resolve)", path);
				}

				// Ensure caller always receives a completion callback to avoid callers waiting indefinitely
				// (e.g., shader loader expects all file callbacks to be invoked).
				task_scheduler->submit_main_thread_task([on_loaded]() {
					on_loaded(std::vector<char>{});
					});
			}
			});
	}

	void AssetManager::load_json_async(const std::string& path, std::function<void(nlohmann::json)> on_loaded) {
		task_scheduler->spawn("AsyncJSONLoad", [this, path, on_loaded]() {
			auto data_opt = this->virtual_file_system->read_binary(path);
			if (data_opt) {
				try {
					nlohmann::json j = nlohmann::json::parse(data_opt->begin(), data_opt->end());
					// Log resolved path for successful JSON loads
					auto resolved = this->virtual_file_system->resolve_path(path);
					if (resolved) {
						bud::print("[IO] Loaded JSON (resolved): {}", resolved->string());
					}
					else {
						bud::print("[IO] Loaded JSON: {}", path);
					}

					task_scheduler->submit_main_thread_task([on_loaded, json = std::move(j)]() mutable {
						on_loaded(std::move(json));
						});
				}
				catch (const std::exception& e) {
					bud::eprint("[Asset] Failed to parse JSON: {} - {}", path, e.what());
				}
			}
			else {
				auto resolved = this->virtual_file_system->resolve_path(path);
				if (resolved) {
					bud::eprint("[Asset] Failed to read JSON file: {} (resolved: {})", path, resolved->string());
				}
				else {
					bud::eprint("[Asset] Failed to read JSON file: {} (could not resolve)", path);
				}
			}
			});
	}

	void AssetManager::save_json_async(const std::string& path, const nlohmann::json& json, std::function<void(bool)> on_finished) {
		std::string dump = json.dump(4);
		std::vector<char> data(dump.begin(), dump.end());
		save_file_async(path, std::move(data), on_finished);
	}

	void AssetManager::save_file_async(const std::string& path, std::vector<char> data, std::function<void(bool)> on_finished) {
		task_scheduler->spawn("AsyncFileSave", [path, data = std::move(data), on_finished, this]() mutable {
			bool success = this->virtual_file_system->write_binary(path, data);
			if (on_finished) {
				task_scheduler->submit_main_thread_task([on_finished, success]() {
					on_finished(success);
					});
			}
			});
	}

} // namespace bud::io
