#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>

#include "src/core/bud.core.hpp"
#include "src/core/bud.math.hpp"

namespace math = bud::math;


namespace bud::graphics {
	constexpr uint32_t ALL_MIPS = 0xFFFFFFFF;

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
		IndirectArgument,  // GPU-Driven Draw Indirect Buffer
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
		R32_FLOAT,
	};

	enum class TextureType {
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube
	};

	enum class MemoryUsage {
		GpuOnly,          // Device Local (MegaBuffers, Textures)
		StagingRing,      // Host Visible + Coherent (Dynamic per-frame UI/Uniforms)
		PersistentMapped, // Host Visible + Coherent + Persistently Mapped (MDI Data)
		Readback          // Gpu to Cpu
	};

	enum class CullMode {
		None,
		Front,
		Back
	};

	enum class CompareOp {
		Less,
		LessEqual,
		Greater,
		GreaterEqual,
		Always
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
		bool is_storage = false;
		bool is_transfer_src = false;
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
		bool is_puppet_mode = false;
		bool is_headless = false;
	};

	struct RenderConfig {
		float fixed_logic_timestep = 1.0f / 60.0f;
		float time_scale = 1.0f;
		static constexpr bool reversed_z = false;

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
		bool cache_shadows = false; // Disabled: feature has rendering bugs, enable when fixed

		bool enable_gpu_driven = true;
		bool debug_hiz = false;
		uint32_t debug_hiz_mip = 0;
		bool enable_cluster_visualization = false;

		// Occluder selection (CPU heuristic prototype)
		float occluder_fraction = 0.1f; // select top 10% as occluders by default
		uint32_t occluder_min_count = 1;
		uint32_t occluder_max_count = 4096;
		float occluder_tri_weight = 1e-4f; // multiplier for triangle count in score
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

		bool show_debug_stats = false;

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

	enum class VertexLayoutType {
		Default,      // Pos(0), Color(1), Normal(2), UV(3)
		PositionOnly, // Pos(0) only
		PositionUV,   // Pos(0) and UV(3)
		PositionNormal, // Pos(0) and Normal(2)
		NoVertexInput,// For self-generating vertices (Fullscreen)
		ImGui         // Special ImGui layout (0,1,2)
	};

	struct GraphicsPipelineDesc {
		ShaderStage vs;
		ShaderStage fs;
		bool depth_test = true;
		bool depth_write = true;
		CompareOp depth_compare_op = CompareOp::Less;
		CullMode cull_mode = CullMode::Back;
		TextureFormat color_attachment_format = TextureFormat::BGRA8_SRGB;
		TextureFormat depth_attachment_format = TextureFormat::D32_FLOAT;
		bool enable_depth_bias = false;
		bool blending_enable = false;
		VertexLayoutType vertex_layout = VertexLayoutType::Default;
	};

	struct ComputePipelineDesc {
		ShaderStage cs;
	};
	// POD, end

    // BufferHandle replaces the old 'MemoryBlock' to avoid API leakage
    // Keep a raw pointer for fast access (`internal_state`) and an optional owning
    // shared_ptr (`owner`) that controls lifetime when this handle represents ownership.
    struct BufferHandle {
        void* internal_state = nullptr; // e.g. VulkanBuffer*
        std::shared_ptr<void> owner;    // optional owning reference with custom deleter
        uint64_t offset = 0;
        uint64_t size = 0;
        void* mapped_ptr = nullptr;

        bool is_valid() const { return internal_state != nullptr; }

        void reset() {
            internal_state = nullptr;
            owner.reset();
            offset = 0;
            size = 0;
            mapped_ptr = nullptr;
        }
    };

	class Texture;

	using CommandHandle = void*;

	// Aliases for clarity
	using ImageHandle = Texture*;

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

	struct IndirectCommand {
		uint32_t index_count;
		uint32_t instance_count;
		uint32_t first_index;
		int32_t  vertex_offset;
		uint32_t first_instance;
	};

	struct SubMesh {
		uint32_t index_start;
		uint32_t index_count;
		uint32_t meshlet_start;
		uint32_t meshlet_count;
		uint32_t material_id;
		bool double_sided = false;

		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
	};

	struct RenderMesh {
		// Offsets into the global Geometry Pool Mega-Buffers
		uint32_t first_index = 0;
		int32_t  vertex_offset = 0;
		uint32_t index_count = 0;

		// GPU-Driven Meshlet data
		BufferHandle meshlet_buffer;
		BufferHandle vertex_index_buffer;
		BufferHandle meshlet_index_buffer;
		BufferHandle cull_data_buffer;
		uint32_t meshlet_count = 0;

		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
		std::vector<SubMesh> submeshes;

		bool is_valid() const { return index_count > 0; }
	};

	struct GPUStats {
		uint32_t totalInstances = 0;
		uint32_t visibleInstances = 0;
		uint32_t totalTriangles = 0;
		uint32_t visibleTriangles = 0;
		uint32_t totalMeshlets = 0;
		uint32_t visibleMeshlets = 0;
	};


	struct RenderStats {
		// 耗时 (ms)
		float fps = 0.0f;
		float frame_time = 0.0f;
		float cpu_render_time = 0.0f;
		float gpu_render_time = 0.0f;

		// 绘制指标
		uint32_t draw_calls = 0;
		uint32_t drawn_triangles = 0; // Total accumulated across ALL render passes (Shadows, etc)
		uint32_t pipeline_binds = 0;

		// 剔除指标 (GPU Occlusion Culling)
		uint32_t gpu_total_objects = 0;
		uint32_t gpu_visible_objects = 0;
		uint32_t gpu_total_instances = 0;
		uint32_t gpu_visible_instances = 0;
		uint32_t gpu_total_triangles = 0;
		uint32_t gpu_visible_triangles = 0;
		uint32_t gpu_total_meshlets = 0;
		uint32_t gpu_visible_meshlets = 0;

		// 剔除指标 (CPU Frustum Culling)
		uint32_t cpu_total_objects = 0;
		uint32_t cpu_visible_objects = 0;
		uint32_t cpu_total_instances = 0;
		uint32_t cpu_visible_instances = 0;
		uint32_t cpu_total_triangles = 0;
		uint32_t cpu_visible_triangles = 0;
		uint32_t cpu_total_meshlets = 0;
		uint32_t cpu_visible_meshlets = 0;

		// ML Occluder Stats
		uint32_t occluder_count = 0;
		uint32_t occluder_triangles = 0;

		uint32_t shadow_casters = 0;
		uint32_t shadow_caster_submeshes = 0;

		void reset() {
			draw_calls = 0;
			drawn_triangles = 0;
			pipeline_binds = 0;
			gpu_total_objects = 0;
			gpu_visible_objects = 0;
			gpu_total_instances = 0;
			gpu_visible_instances = 0;
			gpu_total_triangles = 0;
			gpu_visible_triangles = 0;
			gpu_total_meshlets = 0;
			gpu_visible_meshlets = 0;
			cpu_total_objects = 0;
			cpu_visible_objects = 0;
			cpu_total_instances = 0;
			cpu_visible_instances = 0;
			cpu_total_triangles = 0;
			cpu_visible_triangles = 0;
			cpu_total_meshlets = 0;
			cpu_visible_meshlets = 0;
			occluder_count = 0;
			occluder_triangles = 0;
			shadow_casters = 0;
			shadow_caster_submeshes = 0;
		}
	};
}
