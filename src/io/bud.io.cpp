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
#include "src/tools/asset_pipeline/core/raw_mesh.hpp"

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
		std::error_code ec;
		root_path = std::filesystem::current_path(ec);

		if (!root_path.empty() && !std::filesystem::exists(root_path / "Content", ec) && !std::filesystem::exists(root_path / "data", ec)) {
#if defined(_WIN32)
			wchar_t buf[MAX_PATH];
			DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
			if (len != 0) {
				root_path = std::filesystem::path(std::wstring(buf, buf + len)).parent_path();
			}
#elif defined(__APPLE__)
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			if (size != 0) {
				std::string buf(size, '\0');
				if (_NSGetExecutablePath(buf.data(), &size) == 0) {
					root_path = std::filesystem::weakly_canonical(std::filesystem::path(buf), ec).parent_path();
				}
			}
#else
			std::string buf(4096, '\0');
			ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size());
			if (len > 0) {
				buf.resize(static_cast<size_t>(len));
				root_path = std::filesystem::path(buf).parent_path();
			}
#endif
		}

		bud::print("[IO] VirtualFileSystem initialized. root={}", root_path.string());
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

		// Prefer resolving relative paths against the configured root directory
		auto candidate = root_path / path;
		if (check_exists(candidate))
			return normalize(candidate);

		// Fallback check against Content directory
		auto content_cand = root_path / "Content" / path;
		if (check_exists(content_cand))
			return normalize(content_cand);

		auto content_tex_cand = root_path / "Content/Textures" / path.filename();
		if (check_exists(content_tex_cand))
			return normalize(content_tex_cand);

		bud::eprint("[IO] {} doesn't exist", candidate.string());
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
		std::ifstream file(resolved_path.string(), std::ios::binary | std::ios::ate);
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
		// Mirror read_binary: resolve relative paths against root_path so writes always
		// land in the project tree regardless of process CWD.
		std::filesystem::path abs_path = path.is_absolute() ? path : (root_path / path);

		// Ensure the parent directory exists
		std::error_code ec;
		auto parent = abs_path.parent_path();
		if (!parent.empty()) {
			std::filesystem::create_directories(parent, ec);
			if (ec) {
				bud::eprint("[IO] write_binary: failed to create directories '{}': {}",
					parent.string(), ec.message());
				return false;
			}
		}

		// Write to a temporary file first (atomic pattern)
		std::filesystem::path temp_path = abs_path;
		temp_path.replace_extension(abs_path.extension().string() + ".tmp");

		{
			std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
			if (!file.is_open()) {
				bud::eprint("[IO] write_binary: failed to open temp file for writing: {}",
					temp_path.string());
				return false;
			}
			file.write(data.data(), static_cast<std::streamsize>(data.size()));
			file.flush();
			if (!file) {
				bud::eprint("[IO] write_binary: write failed for: {}", temp_path.string());
				return false;
			}
		}

		// Atomic rename
		std::filesystem::rename(temp_path, abs_path, ec);
		if (ec) {
			bud::eprint("[IO] write_binary: rename failed '{}' -> '{}': {}",
				temp_path.string(), abs_path.string(), ec.message());
			// Fallback: leave the .tmp so data isn't lost
			return false;
		}

		bud::print("[IO] write_binary: wrote {} bytes to '{}'.",
			data.size(), abs_path.string());
		return true;
	}


	std::optional<nlohmann::json> VirtualFileSystem::read_json(const std::filesystem::path& path) {
		auto data_opt = read_binary(path);
		if (!data_opt) {
			bud::eprint("[IO] VFS::read_json: failed to read '{}'.", path.string());
			return std::nullopt;
		}
		try {
			auto j = nlohmann::json::parse(data_opt->begin(), data_opt->end());
			bud::print("[IO] VFS::read_json: parsed '{}'.", path.string());
			return j;
		}
		catch (const std::exception& e) {
			bud::eprint("[IO] VFS::read_json: parse error in '{}': {}", path.string(), e.what());
			return std::nullopt;
		}
	}

	bool VirtualFileSystem::write_json(const std::filesystem::path& path, const nlohmann::json& json) {
		std::string dump = json.dump(4);
		std::vector<char> data(dump.begin(), dump.end());
		return write_binary(path, data);
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
			std::string fn = path.filename().string();
			if (fn == "default.png" || fn == "default.budasset" || fn == "default") {
				// Fallback programmatic default texture (neutral 4x4 RGBA)
				img.width = 4;
				img.height = 4;
				img.channels = 4;
				img.pixels = static_cast<unsigned char*>(std::malloc(4 * 4 * 4));
				if (img.pixels) {
					std::memset(img.pixels, 220, 4 * 4 * 4);
					return std::move(img);
				}
			}
			bud::eprint("[IO] Image not found: {} (could not resolve)", path.string());
			return std::nullopt;
		}
		std::string path_str = resolved_opt->string();

		// Check if the image file is a .budasset texture container
		if (path_str.ends_with(".budasset")) {
			auto data_opt = virtual_file_system->read_binary(path);
			if (data_opt && data_opt->size() >= sizeof(bud::asset::BudAssetHeader)) {
				const auto* h = reinterpret_cast<const bud::asset::BudAssetHeader*>(data_opt->data());
				if (h->magic == bud::asset::BUD_ASSET_MAGIC) {
					if (h->chunk_table_offset + h->chunk_count * sizeof(bud::asset::AssetChunkEntry) <= data_opt->size()) {
						const auto* chunks = reinterpret_cast<const bud::asset::AssetChunkEntry*>(data_opt->data() + h->chunk_table_offset);
						const bud::asset::AssetChunkEntry* tex_chunk = nullptr;
						const bud::asset::AssetChunkEntry* payload_chunk = nullptr;
						for (uint32_t c = 0; c < h->chunk_count; ++c) {
							if (chunks[c].chunk_type == static_cast<uint32_t>(bud::asset::AssetChunkType::Texture))
								tex_chunk = &chunks[c];
							if (chunks[c].chunk_type == static_cast<uint32_t>(bud::asset::AssetChunkType::AssetManifest))
								payload_chunk = &chunks[c];
						}

						if (tex_chunk && tex_chunk->size >= sizeof(bud::asset::TextureHeader)) {
							const auto* th = reinterpret_cast<const bud::asset::TextureHeader*>(data_opt->data() + tex_chunk->offset);
							const auto* mips = reinterpret_cast<const bud::asset::TextureMipEntry*>(data_opt->data() + tex_chunk->offset + sizeof(bud::asset::TextureHeader));
							
							if (th->mip_levels > 0) {
								const auto& mip = mips[0];
								img.width = mip.width;
								img.height = mip.height;
								img.channels = 4;
								img.pixels = static_cast<unsigned char*>(std::malloc(mip.size_in_bytes));
								
								if (payload_chunk && mip.offset_in_payload + mip.size_in_bytes <= payload_chunk->size) {
									std::memcpy(img.pixels, data_opt->data() + payload_chunk->offset + mip.offset_in_payload, mip.size_in_bytes);
									return std::move(img);
								} else {
									std::filesystem::path bulk_path = std::filesystem::path(path_str).replace_extension(".budbulk");
									auto bulk_data = virtual_file_system->read_binary(bulk_path);
									if (bulk_data && mip.size_in_bytes <= bulk_data->size()) {
										std::memcpy(img.pixels, bulk_data->data(), mip.size_in_bytes);
										return std::move(img);
									}
								}
								std::free(img.pixels);
								img.pixels = nullptr;
							}
						}
					}
				}
			}
		}

		img.pixels = stbi_load(path_str.c_str(), &img.width, &img.height, &img.channels, STBI_rgb_alpha);
		if (!img.pixels) {
			const char* reason = stbi_failure_reason();
			bud::eprint("[IO] Failed to load image: {} (stb reason: {})", path_str, reason ? reason : "unknown");
			return std::nullopt;
		}
		img.channels = 4;
		return std::move(img);
	}


	ModelLoader::ModelLoader(VirtualFileSystem* virtual_file_system)
		: virtual_file_system(virtual_file_system) {
	}

	std::optional<MeshData> ModelLoader::load_bud_asset(const std::filesystem::path& path) {
		auto resolved_opt = virtual_file_system->resolve_path(path);
		std::string display_path = path.string();
		if (!resolved_opt) {
			bud::eprint("[IO] .budasset file not found: {}", path.string());
			return std::nullopt;
		}
		display_path = resolved_opt->string();

		auto data_opt = virtual_file_system->read_binary(*resolved_opt);
		if (!data_opt || data_opt->size() < sizeof(asset::BudAssetHeader)) {
			bud::eprint("[IO] Failed to read .budasset binary data: {}", display_path);
			return std::nullopt;
		}

		const char* ptr = data_opt->data();
		size_t data_size = data_opt->size();
		const auto* header = reinterpret_cast<const asset::BudAssetHeader*>(ptr);

		if (header->magic != asset::BUD_ASSET_MAGIC) {
			bud::eprint("[IO] Invalid .budasset magic: {}", display_path);
			return std::nullopt;
		}

		if (header->chunk_table_offset + header->chunk_count * sizeof(asset::AssetChunkEntry) > data_size) {
			bud::eprint("[IO] Chunk table out of bounds: {}", display_path);
			return std::nullopt;
		}

		const auto* chunks = reinterpret_cast<const asset::AssetChunkEntry*>(ptr + header->chunk_table_offset);
		const asset::AssetChunkEntry* raw_chunk = nullptr;
		bool has_vg = false;
		for (uint32_t c = 0; c < header->chunk_count; ++c) {
			if (chunks[c].chunk_type == static_cast<uint32_t>(asset::AssetChunkType::VirtualGeometry)) {
				has_vg = true;
			}
			if (chunks[c].chunk_type == static_cast<uint32_t>(asset::AssetChunkType::RawMesh)) {
				raw_chunk = &chunks[c];
			}
		}

		if (!raw_chunk || raw_chunk->offset + raw_chunk->size > data_size) {
			bud::eprint("[IO] RawMesh chunk not found in .budasset: {}", display_path);
			return std::nullopt;
		}

		auto raw_mesh_opt = bud::asset_pipeline::RawMesh::deserialize_binary(
			reinterpret_cast<const uint8_t*>(ptr + raw_chunk->offset), raw_chunk->size);
		if (!raw_mesh_opt) {
			bud::eprint("[IO] Failed to deserialize RawMesh chunk: {}", display_path);
			return std::nullopt;
		}

		const auto& raw = *raw_mesh_opt;
		MeshData mesh;
		mesh.has_virtual_geometry = has_vg;
		mesh.source_path = path.generic_string();
		mesh.vertices.resize(raw.vertices.size());
		for (size_t i = 0; i < raw.vertices.size(); ++i) {
			mesh.vertices[i].pos = glm::vec3(raw.vertices[i].position[0], raw.vertices[i].position[1], raw.vertices[i].position[2]);
			mesh.vertices[i].normal = glm::vec3(raw.vertices[i].normal[0], raw.vertices[i].normal[1], raw.vertices[i].normal[2]);
			mesh.vertices[i].texture_uv = glm::vec2(raw.vertices[i].uv[0], raw.vertices[i].uv[1]);
			mesh.vertices[i].color = glm::vec3(1.0f);
			mesh.vertices[i].texture_index = 0.0f;
		}
		mesh.indices = raw.indices;

		mesh.subsets.resize(raw.submeshes.size());
		for (size_t i = 0; i < raw.submeshes.size(); ++i) {
			mesh.subsets[i].index_start = raw.submeshes[i].index_offset;
			mesh.subsets[i].index_count = raw.submeshes[i].index_count;
			mesh.subsets[i].material_index = raw.submeshes[i].material_index;
			mesh.subsets[i].aabb = bud::math::AABB(
				bud::math::vec3(raw.aabb_min[0], raw.aabb_min[1], raw.aabb_min[2]),
				bud::math::vec3(raw.aabb_max[0], raw.aabb_max[1], raw.aabb_max[2])
			);
		}

		mesh.materials.resize(raw.materials.size());
		for (size_t i = 0; i < raw.materials.size(); ++i) {
			mesh.materials[i].alpha_mode = static_cast<uint8_t>(raw.materials[i].alpha_mode);
			mesh.materials[i].alpha_cutoff = raw.materials[i].alpha_cutoff;
			mesh.materials[i].double_sided = raw.materials[i].double_sided ? 1 : 0;
			mesh.materials[i].metallic_factor = raw.materials[i].metallic_factor;
			mesh.materials[i].roughness_factor = raw.materials[i].roughness_factor;
			mesh.materials[i].base_color_factor = glm::vec4(
				raw.materials[i].base_color_factor[0],
				raw.materials[i].base_color_factor[1],
				raw.materials[i].base_color_factor[2],
				raw.materials[i].base_color_factor[3]
			);
			mesh.materials[i].name = raw.materials[i].name;

			auto find_tex_idx = [&](const std::string& tex_path) -> uint32_t {
				if (tex_path.empty()) return 0xFFFFFFFF;
				for (size_t t = 0; t < raw.textures.size(); ++t) {
					if (raw.textures[t] == tex_path) return static_cast<uint32_t>(t);
				}
				return 0xFFFFFFFF;
			};

			uint32_t base_tex = find_tex_idx(raw.materials[i].base_color_texture_path);
			mesh.materials[i].base_color_texture = (base_tex != 0xFFFFFFFF) ? base_tex : 0;
			mesh.materials[i].normal_texture = find_tex_idx(raw.materials[i].normal_texture_path);
			mesh.materials[i].metallic_roughness_texture = find_tex_idx(raw.materials[i].metallic_roughness_texture_path);
			mesh.materials[i].emissive_texture = find_tex_idx(raw.materials[i].emissive_texture_path);
		}

		mesh.texture_paths = raw.textures;
		return mesh;
	}


	AssetManager::AssetManager(VirtualFileSystem* virtual_file_system, bud::threading::TaskScheduler* scheduler)
		: virtual_file_system(virtual_file_system), task_scheduler(scheduler), image_loader(virtual_file_system), model_loader(virtual_file_system) {
	}

	void AssetManager::load_mesh_async(const std::string& path, std::function<void(MeshData)> on_loaded) {
		task_scheduler->spawn("AsyncMeshLoad", [this, path, on_loaded]() {
			std::optional<MeshData> mesh_opt = this->model_loader.load_bud_asset(path);

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

	void AssetManager::load_file_chunk_async(const std::string& path, uint64_t offset, uint64_t size,
		std::function<void(std::vector<char>)> on_loaded)
	{
		task_scheduler->spawn("AsyncChunkLoad", [this, path, offset, size, on_loaded]() {
			auto resolved = this->virtual_file_system->resolve_path(path);
			std::vector<char> data;
			if (resolved) {
				std::ifstream file(*resolved, std::ios::binary);
				if (file.is_open()) {
					file.seekg(static_cast<std::streamoff>(offset));
					data.resize(size);
					file.read(data.data(), static_cast<std::streamsize>(size));
					data.resize(static_cast<size_t>(file.gcount()));
				}
				else {
					bud::eprint("[IO] Failed to open chunk file: {}", resolved->string());
				}
			}
			task_scheduler->submit_main_thread_task([on_loaded, data = std::move(data)]() mutable {
				on_loaded(std::move(data));
			});
		});
	}

	void AssetManager::load_budasset_async(const std::string& path,
		std::function<void(std::shared_ptr<BudAssetPackage>)> on_loaded)
	{
		task_scheduler->spawn("AsyncBudAssetLoad", [this, path, on_loaded]() {
			auto package = std::make_shared<BudAssetPackage>();
			package->path = path;
			auto resolved = this->virtual_file_system->resolve_path(path);
			if (resolved) {
				std::ifstream file(*resolved, std::ios::binary);
				if (file.is_open()) {
					file.read(reinterpret_cast<char*>(&package->header), sizeof(bud::asset::BudAssetHeader));
					if (file.gcount() == sizeof(bud::asset::BudAssetHeader) &&
						package->header.magic == bud::asset::BUD_ASSET_MAGIC)
					{
						if (package->header.chunk_count > 0 && package->header.chunk_table_offset > 0) {
							package->chunks.resize(package->header.chunk_count);
							file.seekg(static_cast<std::streamoff>(package->header.chunk_table_offset));
							file.read(reinterpret_cast<char*>(package->chunks.data()),
								static_cast<std::streamsize>(package->header.chunk_count * sizeof(bud::asset::AssetChunkEntry)));
						}
					} else {
						bud::eprint("[IO] load_budasset_async: invalid header or magic in '{}'", path);
						package = nullptr;
					}
				} else {
					bud::eprint("[IO] load_budasset_async: failed to open '{}'", resolved->string());
					package = nullptr;
				}
			} else {
				bud::eprint("[IO] load_budasset_async: could not resolve path '{}'", path);
				package = nullptr;
			}

			task_scheduler->submit_main_thread_task([on_loaded, package = std::move(package)]() mutable {
				on_loaded(std::move(package));
			});
		});
	}

	void AssetManager::load_budasset_chunk_async(std::shared_ptr<BudAssetPackage> package,
		bud::asset::AssetChunkType type, std::function<void(std::vector<char>)> on_loaded)
	{
		if (!package) {
			if (on_loaded) {
				task_scheduler->submit_main_thread_task([on_loaded]() {
					on_loaded(std::vector<char>{});
				});
			}
			return;
		}

		const auto* chunk = package->find_chunk(type);
		if (!chunk) {
			bud::eprint("[IO] load_budasset_chunk_async: chunk type {} not found in '{}'",
				static_cast<uint32_t>(type), package->path);
			if (on_loaded) {
				task_scheduler->submit_main_thread_task([on_loaded]() {
					on_loaded(std::vector<char>{});
				});
			}
			return;
		}

		load_file_chunk_async(package->path, chunk->offset, chunk->size, on_loaded);
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



	bool AssetManager::save_json_sync(const std::string& path, const nlohmann::json& json) {
		std::string dump = json.dump(4);
		std::vector<char> data(dump.begin(), dump.end());
		bool ok = virtual_file_system->write_binary(path, data);
		if (ok)
			bud::print("[IO] save_json_sync: wrote '{}'.", path);
		else
			bud::eprint("[IO] save_json_sync: failed to write '{}'.", path);
		return ok;
	}

	std::optional<nlohmann::json> AssetManager::load_json_sync(const std::string& path) {
		auto data_opt = virtual_file_system->read_binary(path);
		if (!data_opt) {
			bud::eprint("[IO] load_json_sync: failed to read '{}'.", path);
			return std::nullopt;
		}
		try {
			auto j = nlohmann::json::parse(data_opt->begin(), data_opt->end());
			bud::print("[IO] load_json_sync: parsed '{}'.", path);
			return j;
		}
		catch (const std::exception& e) {
			bud::eprint("[IO] load_json_sync: JSON parse error in '{}': {}", path, e.what());
			return std::nullopt;
		}
	}

} // namespace bud::io
