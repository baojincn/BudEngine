#pragma once

#include <cstdint>
#include <cstddef>
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics {

	struct TransientAllocation {
		uint32_t chunk_index = 0;
		uint64_t offset = 0;
		uint64_t size = 0;
		uint64_t alignment = 0;
		void* virtual_alloc_handle = nullptr;
		bool is_valid = false;
	};

	struct TransientHeapStats {
		size_t total_chunks = 0;
		size_t total_allocated_bytes = 0;
		size_t total_heap_capacity_bytes = 0;
		size_t peak_allocated_bytes = 0;
	};

	class TransientHeapBase {
	public:
		virtual ~TransientHeapBase() = default;

		// Allocate placed texture on transient heap.
		// If steady-state image wrapper cache hits, returns existing cached TextureHandle.
		// Otherwise creates image with VK_IMAGE_CREATE_ALIASING_BIT, binds to heap chunk, and caches it.
		virtual TextureHandle allocate_texture(const TextureDesc& desc, TransientAllocation& out_alloc) = 0;

		// Free virtual allocation inside the virtual block during graph compile-time simulation.
		virtual void virtual_free(const TransientAllocation& alloc) = 0;

		// Reset virtual allocations at end of frame (O(1)). Physical memory and cached images are preserved.
		virtual void reset_frame() = 0;

		// Query memory statistics
		virtual TransientHeapStats get_stats() const = 0;

		// Clear cached images and release unused resources
		virtual void clear_cache() = 0;
	};

}
