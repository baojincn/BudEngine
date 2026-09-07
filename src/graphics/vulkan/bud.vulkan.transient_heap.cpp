#include "src/graphics/vulkan/bud.vulkan.transient_heap.hpp"
#include "src/graphics/vulkan/bud.vulkan.utils.hpp"
#include "src/core/bud.logger.hpp"

#include <algorithm>
#include <format>

namespace bud::graphics::vulkan {

	VulkanTransientHeap::VulkanTransientHeap(VkDevice device, VkPhysicalDevice physical_device, VmaAllocator allocator, VulkanResourcePool* pool)
		: device(device), physical_device(physical_device), vma_allocator(allocator), pool(pool) {
	}

	VulkanTransientHeap::~VulkanTransientHeap() {
		clear_cache();
		for (auto& chunk : chunks) {
			if (chunk.virtual_block)
				vmaDestroyVirtualBlock(chunk.virtual_block);
			if (chunk.allocation)
				vmaFreeMemory(vma_allocator, chunk.allocation);
		}
		chunks.clear();
	}

	size_t VulkanTransientHeap::hash_desc(const TextureDesc& desc) const {
		size_t h = desc.width ^ (desc.height << 1) ^ (static_cast<uint32_t>(desc.format) << 2)
			^ (desc.mips << 3) ^ (desc.array_layers << 4) ^ (static_cast<uint32_t>(desc.type) << 5)
			^ (desc.is_storage ? (1u << 6) : 0) ^ (desc.is_transfer_src ? (1u << 7) : 0);
		return h;
	}

	VkMemoryRequirements VulkanTransientHeap::get_image_memory_requirements(const TextureDesc& desc, size_t desc_hash) {
		if (auto it = mem_reqs_cache.find(desc_hash); it != mem_reqs_cache.end())
			return it->second;

		auto vk_format = to_vk_format(desc.format);
		auto usage = get_image_usage(vk_format, desc.is_storage);
		if (desc.is_transfer_src)
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		VkImageCreateInfo image_info{};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.flags = VK_IMAGE_CREATE_ALIAS_BIT;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.extent = { desc.width, desc.height, 1 };
		image_info.mipLevels = desc.mips;
		image_info.arrayLayers = desc.array_layers;
		image_info.format = vk_format;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		image_info.usage = usage;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkImage temp_img = VK_NULL_HANDLE;
		if (vkCreateImage(device, &image_info, nullptr, &temp_img) != VK_SUCCESS) {
			bud::eprint("VulkanTransientHeap: Failed to create prototype image for requirements query: {}x{} fmt={}", desc.width, desc.height, static_cast<int>(desc.format));
			return VkMemoryRequirements{};
		}

		VkMemoryRequirements reqs{};
		vkGetImageMemoryRequirements(device, temp_img, &reqs);
		vkDestroyImage(device, temp_img, nullptr);

		mem_reqs_cache[desc_hash] = reqs;
		return reqs;
	}

	uint32_t VulkanTransientHeap::allocate_chunk(VkDeviceSize min_size, uint32_t memory_type_bits) {
		constexpr VkDeviceSize DEFAULT_CHUNK_SIZE = 256ULL * 1024 * 1024; // 256 MB
		VkDeviceSize chunk_size = std::max(DEFAULT_CHUNK_SIZE, min_size);

		VkMemoryRequirements chunk_reqs{};
		chunk_reqs.size = chunk_size;
		chunk_reqs.alignment = 65536; // 64 KB alignment
		chunk_reqs.memoryTypeBits = memory_type_bits;

		VmaAllocationCreateInfo alloc_create_info{};
		alloc_create_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		alloc_create_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		alloc_create_info.memoryTypeBits = memory_type_bits;

		VmaAllocation chunk_alloc = VK_NULL_HANDLE;
		VmaAllocationInfo alloc_info{};
		VkResult res = vmaAllocateMemory(vma_allocator, &chunk_reqs, &alloc_create_info, &chunk_alloc, &alloc_info);
		if (res != VK_SUCCESS) {
			bud::eprint("VulkanTransientHeap: Failed to allocate physical chunk of {} bytes, error = {}", chunk_size, static_cast<int>(res));
			return UINT32_MAX;
		}

		VmaVirtualBlockCreateInfo vb_info{};
		vb_info.size = chunk_size;
		VmaVirtualBlock vb = VK_NULL_HANDLE;
		res = vmaCreateVirtualBlock(&vb_info, &vb);
		if (res != VK_SUCCESS) {
			bud::eprint("VulkanTransientHeap: Failed to create virtual block, error = {}", static_cast<int>(res));
			vmaFreeMemory(vma_allocator, chunk_alloc);
			return UINT32_MAX;
		}

		uint32_t chunk_idx = static_cast<uint32_t>(chunks.size());
		auto& chunk = chunks.emplace_back();
		chunk.memory = alloc_info.deviceMemory;
		chunk.allocation = chunk_alloc;
		chunk.virtual_block = vb;
		chunk.size = chunk_size;
		chunk.memory_type_index = alloc_info.memoryType;

		bud::print("[VulkanTransientHeap] Created physical VRAM chunk #{} (size = {:.1f} MB, memType = {})",
			chunk_idx, static_cast<double>(chunk_size) / (1024.0 * 1024.0), alloc_info.memoryType);

		return chunk_idx;
	}

	TextureHandle VulkanTransientHeap::allocate_texture(const TextureDesc& desc, TransientAllocation& out_alloc) {
		std::lock_guard lock(mutex);
		out_alloc = TransientAllocation{};

		size_t desc_hash = hash_desc(desc);
		VkMemoryRequirements reqs = get_image_memory_requirements(desc, desc_hash);
		if (reqs.size == 0)
			return TextureHandle{};

		VmaVirtualAllocationCreateInfo valloc_info{};
		valloc_info.size = reqs.size;
		valloc_info.alignment = reqs.alignment;
		valloc_info.flags = VMA_VIRTUAL_ALLOCATION_CREATE_STRATEGY_MIN_OFFSET_BIT;

		uint32_t found_chunk = UINT32_MAX;
		VmaVirtualAllocation valloc = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;

		for (uint32_t c = 0; c < chunks.size(); ++c) {
			if ((reqs.memoryTypeBits & (1u << chunks[c].memory_type_index)) == 0)
				continue;

			if (vmaVirtualAllocate(chunks[c].virtual_block, &valloc_info, &valloc, &offset) == VK_SUCCESS) {
				found_chunk = c;
				break;
			}
		}

		if (found_chunk == UINT32_MAX) {
			found_chunk = allocate_chunk(reqs.size, reqs.memoryTypeBits);
			if (found_chunk == UINT32_MAX)
				return TextureHandle{};

			if (vmaVirtualAllocate(chunks[found_chunk].virtual_block, &valloc_info, &valloc, &offset) != VK_SUCCESS) {
				bud::eprint("VulkanTransientHeap: Virtual allocate failed on freshly created chunk!");
				return TextureHandle{};
			}
		}

		out_alloc.chunk_index = found_chunk;
		out_alloc.offset = offset;
		out_alloc.size = reqs.size;
		out_alloc.alignment = reqs.alignment;
		out_alloc.virtual_alloc_handle = valloc;
		out_alloc.is_valid = true;

		current_allocated_bytes += reqs.size;
		peak_allocated_bytes = std::max(peak_allocated_bytes, current_allocated_bytes);

		CacheKey key{
			.chunk_index = found_chunk,
			.offset = offset,
			.desc_hash = desc_hash
		};

		if (auto it = image_cache.find(key); it != image_cache.end()) {
			auto& cached = it->second;
			cached.last_used_frame = current_frame;
			if (cached.texture)
				cached.texture->reset_subresource_states(desc.initial_state);
			return cached.handle;
		}

		// Cache miss: create new placed VkImage with VK_IMAGE_CREATE_ALIASING_BIT
		auto vk_format = to_vk_format(desc.format);
		auto usage = get_image_usage(vk_format, desc.is_storage);
		if (desc.is_transfer_src)
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		VkImageCreateInfo img_info{};
		img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		img_info.flags = VK_IMAGE_CREATE_ALIAS_BIT;
		img_info.imageType = VK_IMAGE_TYPE_2D;
		img_info.extent = { desc.width, desc.height, 1 };
		img_info.mipLevels = desc.mips;
		img_info.arrayLayers = desc.array_layers;
		img_info.format = vk_format;
		img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		img_info.usage = usage;
		img_info.samples = VK_SAMPLE_COUNT_1_BIT;
		img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VkImage image = VK_NULL_HANDLE;
		VkResult res = vmaCreateAliasingImage2(vma_allocator, chunks[found_chunk].allocation, offset, &img_info, &image);
		if (res != VK_SUCCESS) {
			bud::eprint("VulkanTransientHeap: vmaCreateAliasingImage2 failed, error = {}", static_cast<int>(res));
			vmaVirtualFree(chunks[found_chunk].virtual_block, valloc);
			return TextureHandle{};
		}

		VkImageViewCreateInfo view_info{};
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = image;
		view_info.viewType = (desc.array_layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = vk_format;
		view_info.subresourceRange.aspectMask = get_aspect_flags(vk_format);
		view_info.subresourceRange.baseMipLevel = 0;
		view_info.subresourceRange.levelCount = desc.mips;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount = desc.array_layers;
		view_info.components = {
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY
		};

		VkImageView view = VK_NULL_HANDLE;
		if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
			bud::eprint("VulkanTransientHeap: Failed to create base image view for aliased image");
			vkDestroyImage(device, image, nullptr);
			vmaVirtualFree(chunks[found_chunk].virtual_block, valloc);
			return TextureHandle{};
		}

		auto tex = std::make_shared<VulkanTexture>();
		tex->image = image;
		tex->view = view;
		tex->width = desc.width;
		tex->height = desc.height;
		tex->format = desc.format;
		tex->mips = desc.mips;
		tex->array_layers = desc.array_layers;
		tex->desc_hash = desc_hash;
		tex->current_state = desc.initial_state;
		tex->is_aliased = true;
		tex->heap_offset = offset;

		if (desc.mips > 1) {
			for (uint32_t m = 0; m < desc.mips; ++m) {
				VkImageViewCreateInfo mip_info = view_info;
				mip_info.subresourceRange.baseMipLevel = m;
				mip_info.subresourceRange.levelCount = 1;
				VkImageView mv = VK_NULL_HANDLE;
				if (vkCreateImageView(device, &mip_info, nullptr, &mv) == VK_SUCCESS)
					tex->mip_views.push_back(mv);
				else
					tex->mip_views.push_back(VK_NULL_HANDLE);
			}
		}

		if (desc.array_layers > 1) {
			for (uint32_t l = 0; l < desc.array_layers; ++l) {
				VkImageViewCreateInfo layer_info = view_info;
				layer_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
				layer_info.subresourceRange.baseArrayLayer = l;
				layer_info.subresourceRange.layerCount = 1;
				VkImageView lv = VK_NULL_HANDLE;
				if (vkCreateImageView(device, &layer_info, nullptr, &lv) == VK_SUCCESS)
					tex->layer_views.push_back(lv);
				else
					tex->layer_views.push_back(VK_NULL_HANDLE);
			}
		}

		TextureHandle handle = pool->register_texture(tex);

		image_cache[key] = CachedPlacedImage{
			.handle = handle,
			.texture = tex,
			.chunk_index = found_chunk,
			.offset = offset,
			.size = reqs.size,
			.desc_hash = desc_hash,
			.last_used_frame = current_frame
		};

		return handle;
	}

	void VulkanTransientHeap::virtual_free(const TransientAllocation& alloc) {
		std::lock_guard lock(mutex);
		if (!alloc.is_valid || alloc.chunk_index >= chunks.size() || !alloc.virtual_alloc_handle)
			return;

		vmaVirtualFree(chunks[alloc.chunk_index].virtual_block, static_cast<VmaVirtualAllocation>(alloc.virtual_alloc_handle));
		if (current_allocated_bytes >= alloc.size)
			current_allocated_bytes -= alloc.size;
		else
			current_allocated_bytes = 0;
	}

	void VulkanTransientHeap::reset_frame() {
		std::lock_guard lock(mutex);
		for (auto& chunk : chunks) {
			if (chunk.virtual_block)
				vmaClearVirtualBlock(chunk.virtual_block);
		}
		current_allocated_bytes = 0;
		current_frame++;
	}

	TransientHeapStats VulkanTransientHeap::get_stats() const {
		std::lock_guard lock(mutex);
		TransientHeapStats s{};
		s.total_chunks = chunks.size();
		s.total_allocated_bytes = current_allocated_bytes;
		s.peak_allocated_bytes = peak_allocated_bytes;
		for (const auto& chunk : chunks)
			s.total_heap_capacity_bytes += chunk.size;
		return s;
	}

	void VulkanTransientHeap::clear_cache() {
		std::lock_guard lock(mutex);
		for (auto& [key, entry] : image_cache) {
			if (entry.texture) {
				for (auto v : entry.texture->mip_views) {
					if (v)
						vkDestroyImageView(device, v, nullptr);
				}
				entry.texture->mip_views.clear();

				for (auto v : entry.texture->layer_views) {
					if (v)
						vkDestroyImageView(device, v, nullptr);
				}
				entry.texture->layer_views.clear();

				if (entry.texture->view) {
					vkDestroyImageView(device, entry.texture->view, nullptr);
					entry.texture->view = VK_NULL_HANDLE;
				}

				if (entry.texture->image) {
					vkDestroyImage(device, entry.texture->image, nullptr);
					entry.texture->image = VK_NULL_HANDLE;
				}

				if (pool && entry.handle.is_valid())
					pool->unregister_texture(entry.handle);
			}
		}
		image_cache.clear();
	}

}
