// Only for pure enum and POD structure

module;

#include <cstdint>
#include <vector>
#include <string>

export module bud.graphics.defs;

import bud.math;

export namespace bud::graphics {
	// Enum, begin
	export enum class Backend {
		Vulkan,
		D3D12,
		Metal
	};

	export enum class ResourceState {
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


	export enum class ObjectType {
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

	export enum class TextureFormat {
		Undefined,          // [FIX] For depth-only pipelines (no color attachment)
		RGBA8_UNORM,
		BGRA8_UNORM,
		BGRA8_SRGB, // [FIX] Added for Swapchain Match
		R32G32B32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT,
	};

	export enum class TextureType {
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube
	};

	export enum class MemoryUsage {
		GpuOnly,    // Device Local
		CpuToGpu,   // Host Visible (Upload)
		GpuToCpu,   // Host Visible (Readback)
	};

	export enum class CullMode {
		None,
		Front,
		Back
	};
	
	export constexpr uint32_t MAX_CASCADES = 4;
	// Enum, end

	// POD, begin
	export struct TextureDesc {
		uint32_t width = 1;
		uint32_t height = 1;
		uint32_t depth = 1;
		uint32_t array_layers = 1;
		uint32_t mips = 1;
		TextureFormat format = TextureFormat::RGBA8_UNORM;
		TextureType type = TextureType::Texture2D;
		ResourceState initial_state = ResourceState::Undefined;
	};

	export struct RenderConfig {
		uint32_t shadow_map_size = 2048; // [CSM] Reduced per-cascade size
		float shadow_bias_constant = 1.25f;
		float shadow_bias_slope = 1.75f;
		float shadow_ortho_size = 35.0f;
		float shadow_near_plane = 0.1f;
		float shadow_far_plane = 100.0f;
		
		uint32_t cascade_count = 4;
		float cascade_split_lambda = 0.95f; // Practical Split Scheme

		bud::math::vec3 directional_light_position = { 5.0f, 15.0f, 5.0f };
		bud::math::vec3 directional_light_color = { 1.0f, 1.0f, 1.0f };
		float directional_light_intensity = 5.0f;
		float ambient_strength = 0.05f;

		bool enable_soft_shadows = true;
		bool debug_cascades = false;
		bool cache_shadows = true;
	};

	export struct SceneView {
		bud::math::mat4 model = bud::math::mat4(1.0f); // Model/World transform
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

		void update_matrices() {
			view_proj_matrix = proj_matrix * view_matrix;
		}
	};

	export struct VertexAttribute {
		uint32_t location;
		uint32_t binding = 0;
		TextureFormat format;
		uint32_t offset;
	};

	export struct VertexInputLayout {
		std::vector<VertexAttribute> attributes;
		uint32_t stride = 0;
	};

	export struct ShaderStage {
		std::vector<char> code;
		std::string entry_point = "main";
	};

	export struct GraphicsPipelineDesc {
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
}
