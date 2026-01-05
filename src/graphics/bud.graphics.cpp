module;
#include <memory>
#include <stdexcept>

module bud.graphics;

import bud.graphics.vulkan;
// import bud.graphics.d3d12;

namespace bud::graphics {

	std::unique_ptr<RHI> create_rhi(Backend backend) {
		switch (backend) {
		case Backend::Vulkan:
			return std::make_unique<bud::graphics::vulkan::VulkanRHI>();

		case Backend::D3D12:
			throw std::runtime_error("D3D12 backend not implemented yet.");

		default:
			throw std::runtime_error("Unknown graphics backend.");
		}
	}
}
