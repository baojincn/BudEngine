#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include "src/core/bud.math.hpp"

namespace bud::graphics {
	// Enum, begin
	enum class Backend {
		Vulkan,
		D3D12,
		Metal
	};

	enum class ResourceState {
		Undefined,
		Common,            // D3D12_RESOURCE_STATE_COMMON / VK_IMAGE_LAYOUT_GENERAL
		VertexBuffer,      // Vertex Buffer / Constant Buffer
		IndexBuffer,       // Index Buffer
		RenderTarget,      // Color Attachment Write
		DepthWrite,        // Depth Attachment Write
		DepthRead,         // Depth Attachment Read
		ShaderResource,    // Pixel Shader Read (Texture)
		UnorderedAccess,   // Compute Shader Write (UAV / Storage Image)
		TransferSrc,       // Copy Source
		TransferDst,       // Copy Dest
		Present,           // Swapchain Present
	};


	enum class ObjectType {
		Unknown,
		Texture,        // VkImage / ID3D12Resource / MTLTexture
		ImageView,      // VkImageView / D3D12_CPU_DESCRIPTOR_HANDLE (RTV/DSV)
		Buffer,         // VkBuffer / ID3D12Resource / MTLBuffer
		Shader,         // VkShaderModule / ID3DBlob / MTLLibrary
		Pipeline,       // VkPipeline / ID3D12PipelineState / MTLRenderPipelineState
		CommandBuffer,  // VkCommandBuffer / ID3D12GraphicsCommandList / MTLCommandBuffer
		Queue,          // VkQueue / ID3D12CommandQueue / MTLCommandQueue
		Semaphore,      // VkSemaphore / ID3D12Fence / MTLEvent
		Fence,          // VkFence / ID3D12Fence / MTLSharedEvent
		Sampler,        // VkSampler / D3D12_CPU_DESCRIPTOR_HANDLE / MTLSamplerState
		Instance,       // VkInstance / IDXGIFactory
		Device,         // VkDevice / ID3D12Device / MTLDevice
		RenderPass,     // VkRenderPass (DX12/Metal 无直接对应，通常指代一次渲染过程)
		DescriptorSet,  // VkDescriptorSet / ID3D12DescriptorHeap (部分对应)
	};

	enum class TextureFormat {
		Undefined,
		RGBA8_UNORM,
		BGRA8_UNORM,
		BGRA8_SRGB,
		R32G32B32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT,
	};

	enum class TextureType {
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube
	};

	enum class MemoryUsage {
		GpuOnly,    // Device Local
		CpuToGpu,   // Host Visible (Upload)
		GpuToCpu,   // Host Visible (Readback)
	};

	enum class CullMode {
		None,
		Front,
		Back
	};

	constexpr uint32_t MAX_CASCADES = 4;
	// Enum, end

	// POD, begin
	struct TextureDesc {
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		uint32_t array_layers = 1;
		uint32_t mips = 1;
		TextureFormat format = TextureFormat::RGBA8_UNORM;
		TextureType type = TextureType::Texture2D;
		ResourceState initial_state = ResourceState::Undefined;
	};

	struct EngineConfig {
		std::string name = "Bud Engine";
		int width = 1920;
		int height = 1080;
		Backend backend = Backend::Vulkan;
		uint32_t inflight_frame_count = 3;
		bool enable_validation = true;
		bool vsync = false;
	};

	struct RenderConfig {
		float fixed_logic_timestep = 1.0f / 60.0f;
		float time_scale = 1.0f;

		uint32_t shadow_map_size = 2048;
		float shadow_bias_constant = 1.25f;
		float shadow_bias_slope = 1.75f;
		float shadow_ortho_size = 35.0f;
		float shadow_near_plane = 1.0f;
		float shadow_far_plane = 3000.0f;

		uint32_t cascade_count = 4;
		float cascade_split_lambda = 0.75f; // Practical Split Scheme

		bool enable_soft_shadows = true;
		bool debug_cascades = false;
		bool cache_shadows = true;
	};

	struct SceneView {
		bud::math::mat4 model_matrix = bud::math::mat4(1.0f);
		bud::math::mat4 view_matrix;
		bud::math::mat4 proj_matrix;
		bud::math::mat4 view_proj_matrix;

		bud::math::vec3 camera_position;
		float fov;
		float near_plane;
		float far_plane;

		float viewport_width;
		float viewport_height;

		float time;
		float delta_time;

		bud::math::mat4 cascade_view_proj_matrices[MAX_CASCADES];
		float cascade_split_depths[MAX_CASCADES];

		bud::math::vec3 light_dir = { 0.5f, 1.0f, 0.3f };
		bud::math::vec3 light_color = { 1.0f, 1.0f, 1.0f };
		float light_intensity = 5.0f;
		float ambient_strength = 0.05f;

		void update_matrices() {
			view_proj_matrix = proj_matrix * view_matrix;
		}
	};

	struct VertexAttribute {
		uint32_t location;
		uint32_t binding = 0;
		TextureFormat format;
		uint32_t offset;
	};

	struct VertexInputLayout {
		std::vector<VertexAttribute> attributes;
		uint32_t stride = 0;
	};

	struct ShaderStage {
		std::vector<char> code;
		std::string entry_point = "main";
	};

	struct GraphicsPipelineDesc {
		ShaderStage vs;
		ShaderStage fs;
		VertexInputLayout vertex_layout;
		TextureFormat color_attachment_format = TextureFormat::RGBA8_UNORM;
		TextureFormat depth_attachment_format = TextureFormat::D32_FLOAT;
		bool depth_test = true;
		bool depth_write = true;
		CullMode cull_mode = CullMode::Back;
	};
	// POD, end

	using CommandHandle = void*;

	struct MemoryBlock {
		void* internal_handle = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		void* mapped_ptr = nullptr;
		bool is_valid() const { return internal_handle != nullptr; }
	};

	// 纹理基类
	class Texture {
	public:
		virtual ~Texture() = default;

		uint32_t width = 0;
		uint32_t height = 0;
		TextureFormat format = TextureFormat::RGBA8_UNORM;
		uint32_t mips = 1;
		uint32_t array_layers = 1;
		TextureType type = TextureType::Texture2D;

		size_t desc_hash = 0;
	};


	struct CascadeData {
		bud::math::mat4 view_proj_matrix;
		float split_depth;
	};

	struct SubMesh {
		uint32_t index_start;
		uint32_t index_count;
		uint32_t material_id;
		bool double_sided = false;

		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
	};

	struct RenderMesh {
		MemoryBlock vertex_buffer;
		MemoryBlock index_buffer;
		uint32_t index_count = 0;
		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
		std::vector<SubMesh> submeshes;

		bool is_valid() const { return index_count > 0; }
	};
}
