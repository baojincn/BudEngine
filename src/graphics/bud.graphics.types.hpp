#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <cmath>

#include "src/core/bud.core.hpp"
#include "src/core/bud.math.hpp"

namespace math = bud::math;


namespace bud::graphics {

	// Backend-agnostic descriptor type constants (matching VkDescriptorType)
	inline constexpr uint32_t DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1;
	inline constexpr uint32_t DESCRIPTOR_TYPE_STORAGE_IMAGE = 3;
	inline constexpr uint32_t DESCRIPTOR_TYPE_UNIFORM_BUFFER = 6;
	inline constexpr uint32_t DESCRIPTOR_TYPE_STORAGE_BUFFER = 7;

	// Backend-agnostic shader stage constants (matching VkShaderStageFlagBits for EXT mesh shader)
	inline constexpr uint32_t SHADER_STAGE_TASK_BIT = 0x00000040;  // VK_SHADER_STAGE_TASK_BIT_EXT
	inline constexpr uint32_t SHADER_STAGE_MESH_BIT = 0x00000080;  // VK_SHADER_STAGE_MESH_BIT_EXT
	inline constexpr uint32_t SHADER_STAGE_VERTEX_BIT = 0x00000001;
	inline constexpr uint32_t SHADER_STAGE_FRAGMENT_BIT = 0x00000010;
	inline constexpr uint32_t SHADER_STAGE_COMPUTE_BIT = 0x00000020;

	constexpr uint32_t ALL_MIPS = 0xFFFFFFFF;

	// Screen-space-error LOD selection for page-backed meshes (Nanite-style
	// single threshold). A LOD level's object-space error projects to
	// `error * focal_pixels / distance` pixels; we pick the COARSEST level
	// whose projected error is still below the threshold. This keeps small
	// nearby pages (flowers, statues) at full detail and coarsens large far
	// pages, which a naive distance/radius ratio gets wrong.
	inline uint32_t select_page_lod(float distance, float radius, float focal_pixels,
		float error_lod1, float error_lod2, float threshold_px) {
		(void)radius;
		float dist = std::max(distance, 1e-3f);
		// Vulkan's clip-space Y is flipped, so proj[1][1] (and thus focal_pixels
		// = proj[1][1] * viewport_height * 0.5) is NEGATIVE. Using a negative
		// focal would make every projected error negative, so e <= threshold is
		// always true and the coarsest LOD is always selected (broken, stretched
		// meshes). Use the absolute focal length.
		const float f = std::abs(focal_pixels);
		// Prefer LOD2 while its projected error is acceptable.
		float e2 = error_lod2 * f / dist;
		if (e2 <= threshold_px) return 2;
		float e1 = error_lod1 * f / dist;
		if (e1 <= threshold_px) return 1;
		return 0;
	}

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

	enum class QueueType : uint32_t {
		Graphics = 0,
		AsyncCompute = 1,
		Transfer = 2
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
		R8_UNORM,
		RGBA8_UNORM,
		RGBA8_SRGB,
		BGRA8_UNORM,
		BGRA8_SRGB,
		BC7_UNORM,
		BC5_UNORM,
		RGBA16_FLOAT,
		R32G32B32_FLOAT,
		R32G32_UINT,
		RGBA32_UINT,
		D32_FLOAT,
		D24_UNORM_S8_UINT,
		R32_FLOAT,
	};

	struct GPUMaterialData {
		glm::vec4 base_color_factor{ 1.0f, 1.0f, 1.0f, 1.0f }; // 16 bytes
		uint32_t albedo_texture_id = 0;                         // 4 bytes (Bindless slot)
		uint32_t normal_texture_id = 0;                         // 4 bytes (Bindless slot)
		uint32_t metallic_roughness_id = 0;                     // 4 bytes (Bindless slot)
		uint32_t emissive_texture_id = 0;                       // 4 bytes (Bindless slot)
		float metallic_factor = 0.0f;                           // 4 bytes
		float roughness_factor = 0.5f;                          // 4 bytes
		float alpha_cutoff = 0.5f;                              // 4 bytes
		uint32_t alpha_mode = 0;                                // 4 bytes (0=Opaque, 1=Mask, 2=Blend)
	};
	static_assert(sizeof(GPUMaterialData) == 48, "GPUMaterialData must be 48 bytes (std430 aligned)");

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
		TextureFormat format = TextureFormat::RGBA8_SRGB;
		TextureType type = TextureType::Texture2D;
		bool is_storage = false;
		bool is_transfer_src = false;
		ResourceState initial_state = ResourceState::Undefined;
	};

	struct BufferDesc {
		uint64_t size = 0;
		ResourceState usage = ResourceState::Common;
		MemoryUsage memory_usage = MemoryUsage::GpuOnly;
		std::string name;
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

		// Standard World Spatial & Physical Units (1.0f == 1.0 cm)
		float world_unit_scale_cm = bud::core::units::cm;
		float default_gravity = -bud::core::units::gravity; // -980.665 cm/s^2
	};

	enum class AOMode : uint32_t {
		Disabled = 0,
		SSAO = 1,
		GTAO = 2
	};

	enum class SkyTimeMode {
		Day = 0,      // 正午白天预设
		Sunset = 1,   // 黄昏日落预设
		Night = 2,    // 深邃暗夜预设
		Manual = 3,   // 手动滑块调节 (Elevation / Azimuth)
		Cycle24H = 4  // 24小时昼夜连续交替流逝
	};

	struct SkyConfig {
		bool enable_sky = true;
		bool enable_clouds = true;
		SkyTimeMode time_mode = SkyTimeMode::Manual;
		float time_of_day = 12.0f; // 0.0 ~ 24.0 hours (12.0 = Noon, 18.0 = Sunset, 0.0 = Midnight)
		float time_speed = 0.5f;   // Hours per second during 24H cycle
		float sun_elevation = 55.0f; // Degrees (-90.0 ~ 90.0)
		float sun_azimuth = 25.0f;   // Degrees (0.0 ~ 360.0)
		float sun_intensity = 1.3f;
		bud::math::vec3 sun_color = bud::math::vec3(1.0f, 0.98f, 0.95f);
		float rayleigh_density = 1.0f;
		float mie_density = 1.0f;
		float ozone_density = 1.0f;
		float cloud_coverage = 0.45f;
		float cloud_speed = 0.08f;
	};

	struct RenderConfig {
		float fixed_logic_timestep = 1.0f / 60.0f;
		float time_scale = 1.0f;
		static constexpr bool reversed_z = false;

		uint32_t shadow_map_size = 2048;

		// -----------------------------------------------------------------
		// Shadow bias: TWO independent stages with TWO different units.
		// Never share values between them - that was the source of the
		// "light leaking / shadows detached by metres" artefact.
		//
		// (1) Raster stage bias, applied while rendering INTO the shadow map.
		//     Units = vkCmdSetDepthBias factors (device depth units, NOT
		//     normalized [0,1] fractions). Only used by CSMShadowPass.
		// -----------------------------------------------------------------
		float shadow_bias_constant = 2.0f;  // constantFactor (integer part is what counts)
		float shadow_bias_slope = 1.75f;    // slopeFactor
		float shadow_bias_clamp = 0.0f;     // 0 == no clamping (Vulkan convention)

		// (2) Receiver stage bias, applied when sampling the shadow map in
		//     lighting.glsl. Expressed in SHADOW TEXELS, so it is invariant to
		//     cascade size / map resolution / camera pitch:
		//         world_offset = cascade_texel_size * <these factors>
		float shadow_normal_offset_texels = 1.0f; // world-space offset along N
		float shadow_receiver_bias_texels = 1.5f; // residual light-depth offset

		float shadow_ortho_size = 35.0f * bud::core::units::m;
		// ^ Legacy: only a fallback for the shadow caster LOD metric when a cascade has
		//   no derived extent yet. update_cascades() now sizes every cascade from its own
		//   frustum slice, so this no longer controls the shadow-map coverage.
		float shadow_near_plane = 0.1f * bud::core::units::m; // Unused: cascades get a per-cascade slab from update_cascades().
		float shadow_far_plane = 500.0f * bud::core::units::m;
		// Cascade boxes never need to be bigger than the scene they enclose.
		// update_cascades() clamps shadow_far_plane to
		// scene_bounds_radius * this factor (prevents every cascade covering
		// the whole world => identical shadow maps + unusable resolution).
		float shadow_far_scene_factor = 2.5f;

		uint32_t cascade_count = 4;
		float cascade_split_lambda = 0.75f; // Practical Split Scheme

		bool enable_soft_shadows = true;
		bool debug_cascades = false;
		bool debug_physics = false;
		// Feed the CSM cascade traversals the FULL scene instance list instead of only
		// the main-camera visible ones. This is the correct CSM model: an object that is
		// outside the primary frustum but inside a cascade's light box still has to be
		// rasterized into that cascade, otherwise rotating the camera changes which
		// objects exist in the shadow map and the shadows visibly break when you look up
		// or down (the caster count tracked the visible set: 20..102 of 393).
		// The reason this used to be off is that it also drags in authored backdrop
		// geometry - the Sponza asset carries a full-footprint lid (sponza_380: a 37x23m
		// slab at y=13.30..14.22, 100% of the scene footprint, 0.92m thick) which then
		// blocks the sun from the entire courtyard. That is now handled per entity with
		// "is_cast_shadow": false in the scene file, and Renderer reports the candidates
		// by asset path (see BudEngine::extract_scene) so any scene can be cleaned up.
		bool shadow_full_scene_casters = true;
		// Opt-in: let alpha-blended translucent meshes write shadow depth too. Off by
		// default because the shadow passes have no opacity-aware depth stage - a glass
		// pane would then drop a fully solid shadow. Translucent objects are drawn by the
		// forward translucent pass and are excluded from every shadow-caster list here.
		bool shadow_translucent_casters = false;

		bool enable_virtual_geometry = true;
		bool enable_mesh_shader = true;
		bool enable_hiz_culling = true;
		bool debug_hiz = false;
		uint32_t debug_hiz_mip = 0;
		bool enable_cluster_visualization = false;
		bool enable_wireframe = false;

		// Page LOD selection by screen-space error (Nanite-style single threshold):
		// A LOD level L is used while its accumulated object-space error projects
		// to <= lod_error_threshold_px pixels on screen.
		float lod_error_lod1 = 2.0f * bud::core::units::mm; // 0.002 m
		float lod_error_lod2 = 10.0f * bud::core::units::mm; // 0.010 m
		float lod_error_threshold_px = 2.0f; // in screen pixels

		// Ambient Occlusion
		AOMode ao_mode = AOMode::GTAO;
		float ao_radius = 1.2f * bud::core::units::m; // 1.2 m (architectural scale)
		float ao_intensity = 0.8f;
		// 32 samples = 8 steps per slice direction at half-res. Good balance of
		// quality and cost now that the temporal reprojection is fixed.
		uint32_t ao_sample_count = 32;
		bool ao_blur_enable = true;
		bool ao_half_res = true;         // Evaluate AO at half resolution and upsample
		bool ao_temporal_enable = true;  // Temporal accumulation over the previous frame

        // Heuristic Occluder selection (CPU heuristic prototype)
        bool heuristic_occluder_enable = true; // enable heuristic occluder selection by default
        float heuristic_occluder_fraction = 0.3f; // select top 30% as occluders by default
        uint32_t heuristic_occluder_min_count = 1;
        uint32_t heuristic_occluder_max_count = 500;
        float heuristic_occluder_tri_weight = 1e-4f; // multiplier for triangle count in score

		// Screen-Space Reflections (SSR)
		bool enable_ssr = true;
		float ssr_max_distance = 30.0f * bud::core::units::m;
		float ssr_thickness = 0.35f * bud::core::units::m;
		uint32_t ssr_max_steps = 48;
		uint32_t ssr_binary_steps = 8;
		float ssr_intensity = 1.0f;

		// Screen-Space Global Illumination (SSGI)
		bool enable_ssgi = true;
		float ssgi_radius = 8.0f * bud::core::units::m;
		float ssgi_thickness = 0.5f * bud::core::units::m;
		uint32_t ssgi_ray_count = 8;
		uint32_t ssgi_max_steps = 24;
		float ssgi_intensity = 1.5f;
		float ssgi_temporal_blend = 0.05f;

		// Sky & Physical Atmosphere
		SkyConfig sky_config;
	};

	struct SceneView {
		bud::math::mat4 model_matrix = bud::math::mat4(1.0f);
		bud::math::mat4 view_matrix;
		bud::math::mat4 proj_matrix;
		bud::math::mat4 view_proj_matrix;
		bud::math::mat4 prev_view_proj_matrix = bud::math::mat4(1.0f);

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
		// Per-cascade shadow-map metrics filled by Renderer::update_cascades().
		// cascade_texel_size  = world metres covered by one shadow-map texel.
		// cascade_depth_range = light-space depth slab thickness (metres) of the
		//                       ortho projection used for that cascade.
		// The receiver shader converts its "in texels" bias into normalized depth
		// with texel_size / depth_range, so bias stays correct for every cascade.
		float cascade_texel_size[MAX_CASCADES] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float cascade_depth_range[MAX_CASCADES] = { 1.0f, 1.0f, 1.0f, 1.0f };

		bud::math::vec3 light_dir = { 0.5f, 1.0f, 0.3f };
		bud::math::vec3 light_color = { 1.0f, 1.0f, 1.0f };
		float light_intensity = 5.0f;
		float ambient_strength = 0.25f;

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
		ImGui,        // Special ImGui layout (0,1,2)
		DebugLine     // Pos(0), Color(1) — for line debug rendering
	};

	struct DescriptorBinding {
		uint32_t binding;
		uint32_t descriptor_type; // VkDescriptorType cast to uint32_t
		uint32_t count = 1;
		uint32_t stage_flags = 0; // VkShaderStageFlags cast to uint32_t
		uint32_t binding_flags = 0; // VkDescriptorBindingFlags cast to uint32_t
	};

	enum class BlendMode {
		Disabled,
		Alpha,              // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
		PremultipliedAlpha, // ONE, ONE_MINUS_SRC_ALPHA
		Additive            // ONE, ONE
	};

	enum class PrimitiveTopology {
		TriangleList,
		LineList,
	};

	struct GraphicsPipelineDesc {
		ShaderStage vs;
		ShaderStage fs;
		ShaderStage ts; // Task shader (mesh shader pipeline)
		ShaderStage ms; // Mesh shader (mesh shader pipeline)
		bool depth_test = true;
		bool depth_write = true;
		CompareOp depth_compare_op = CompareOp::Less;
		CullMode cull_mode = CullMode::Back;
		TextureFormat color_attachment_format = TextureFormat::BGRA8_SRGB;
		TextureFormat depth_attachment_format = TextureFormat::D32_FLOAT;
		bool enable_depth_bias = false;
		bool blending_enable = false;
		BlendMode blend_mode = BlendMode::Disabled;
		VertexLayoutType vertex_layout = VertexLayoutType::Default;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		bool wireframe = false;
		// Backend-specific descriptor set layouts to use instead of the global set.
		// These are VkDescriptorSetLayout handles cast to uint64_t for portability.
		// When non-empty, the pipeline layout will use these sets instead of the global set.
		std::vector<uint64_t> custom_set_layouts;
	};

	struct ComputePipelineDesc {
		enum class LayoutKind {
			HiZCulling,
			HiZMip,
			MlIdentity,
			AmbientOcclusion,
			AOBlur,
			AOTemporal,
			HierarchyTraversal,
			PageEmit,
			ClusterCull,
			ClearStats,
			CSMCulling,
			ScreenSpaceReflections,
			ScreenSpaceGlobalIllumination,
			SSGIDenoise,
			SSGITemporal
		};

		ShaderStage cs;
		LayoutKind layout_kind = LayoutKind::HiZCulling;
	};
	// POD, end

	// Runtime GPU Resource Handles (Direct Slot ID)
	struct BufferHandle {
		uint32_t id = ~0u;

		constexpr bool is_valid() const noexcept { return id != ~0u; }
		constexpr void reset() noexcept { id = ~0u; }
		constexpr bool operator==(const BufferHandle& other) const noexcept = default;
		constexpr auto operator<=>(const BufferHandle& other) const noexcept = default;
	};

	struct BufferSlice {
		BufferHandle buffer;
		uint64_t offset = 0;
		uint64_t size = 0;
		void* mapped_ptr = nullptr;

		constexpr bool is_valid() const noexcept { return buffer.is_valid(); }
		constexpr void reset() noexcept {
			buffer.reset();
			offset = 0;
			size = 0;
			mapped_ptr = nullptr;
		}
	};

	struct TextureHandle {
		uint32_t id = ~0u;

		constexpr bool is_valid() const noexcept { return id != ~0u; }
		constexpr void reset() noexcept { id = ~0u; }
		constexpr bool operator==(const TextureHandle& other) const noexcept = default;
		constexpr auto operator<=>(const TextureHandle& other) const noexcept = default;
	};

	struct PipelineHandle {
		uint32_t id = ~0u;

		constexpr bool is_valid() const noexcept { return id != ~0u; }
		constexpr void reset() noexcept { id = ~0u; }
		constexpr bool operator==(const PipelineHandle& other) const noexcept = default;
		constexpr auto operator<=>(const PipelineHandle& other) const noexcept = default;
	};

	struct MaterialHandle {
		uint32_t id = ~0u;

		constexpr bool is_valid() const noexcept { return id != ~0u; }
		constexpr void reset() noexcept { id = ~0u; }
		constexpr bool operator==(const MaterialHandle& other) const noexcept = default;
		constexpr auto operator<=>(const MaterialHandle& other) const noexcept = default;
	};

	class Texture;
	class Buffer;

	using CommandHandle = void*;

	// Aliases for clarity
	using ImageHandle = TextureHandle;

	class Texture {
	public:
		virtual ~Texture() = default;

		uint32_t width = 0;
		uint32_t height = 0;
		TextureFormat format = TextureFormat::RGBA8_SRGB;
		uint32_t mips = 1;
		uint32_t array_layers = 1;
		TextureType type = TextureType::Texture2D;

		size_t desc_hash = 0;
	};

	class Buffer {
	public:
		virtual ~Buffer() = default;

		uint64_t size = 0;
		ResourceState usage = ResourceState::Common;
		MemoryUsage memory_usage = MemoryUsage::GpuOnly;
		void* mapped_ptr = nullptr;

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
		uint32_t material_id;

		uint32_t lod_level = 0;
		uint32_t page_index = ~0u;
		bool double_sided = false;
		bool is_alpha_tested = false;
		bool is_translucent = false;

		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
	};


	struct PageSubMesh {
		uint32_t index_start = 0;
		uint32_t index_count = 0;
		uint32_t material_id = 0; // bindless texture slot (resolved at runtime)
		uint32_t lod_level = 0;
		uint32_t cluster_start = 0;
		uint32_t cluster_count = 0;
		uint32_t page_index = ~0u;
		bud::math::AABB aabb{};
	};

	struct RenderMesh {
		uint32_t index_count = 0;


		// Virtual geometry page residency
		bool is_page_based = false;
		uint32_t page_index = ~0u;
		uint32_t page_vertex_data_offset = 0;
		uint32_t page_index_data_offset = 0;

		uint32_t lod_index_start[3] = {};
		uint32_t lod_index_count[3] = {};

		float lod_error[3] = {};

		bud::math::AABB aabb;
		bud::math::BoundingSphere sphere;
		bud::math::BoundingSphere global_sphere;
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
		uint32_t heuristicTotalCount = 0;
		uint32_t heuristicCutoffBucket = 0;
		uint32_t heuristicRemaining = 0;
		uint32_t heuristicVisibleInstances = 0;
	};

	enum class VisibilityPath {
		Instance,
		Cluster,
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
		VisibilityPath active_visibility_path = VisibilityPath::Instance;

		// 剔除指标 (GPU Occlusion Culling)
		uint32_t gpu_total_objects = 0;
		uint32_t gpu_visible_objects = 0;
		uint32_t gpu_total_instances = 0;
		uint32_t gpu_visible_instances = 0;
		uint32_t gpu_total_triangles = 0;
		uint32_t gpu_visible_triangles = 0;

		// 剔除指标 (CPU Frustum Culling)
		uint32_t cpu_total_objects = 0;
		uint32_t cpu_visible_objects = 0;
		uint32_t cpu_total_instances = 0;
		uint32_t cpu_visible_instances = 0;
		uint32_t cpu_total_triangles = 0;
		uint32_t cpu_visible_triangles = 0;

		// Virtual Geometry Cluster Stats
		uint32_t vg_total_clusters = 0;
		uint32_t vg_visible_clusters = 0;
		uint32_t vg_resident_pages = 0;
		uint32_t vg_streaming_requests = 0;

		// Neural/Heuristic Occluder Stats
		uint32_t occluder_count = 0;
		uint32_t occluder_triangles = 0;
		uint32_t heuristic_total_count = 0;
		uint32_t heuristic_cutoff_bucket = 0;
		uint32_t heuristic_remaining = 0;
		uint32_t gpu_occluder_instances = 0;

		uint32_t shadow_casters = 0;
		uint32_t shadow_caster_submeshes = 0;

		// Debug overlay attribution. The physics wireframe is drawn by its own pass with a
		// single LINE_LIST draw call, which is invisible inside draw_calls; exposing it
		// separately lets the HUD show what the overlay actually costs (and it proves the
		// overlay is really being submitted when debug_physics is on).
		uint32_t physics_debug_draw_calls = 0;
		uint32_t physics_debug_line_vertices = 0;
		uint32_t physics_debug_boxes = 0;

		void reset() {
			draw_calls = 0;
			drawn_triangles = 0;
			pipeline_binds = 0;
			physics_debug_draw_calls = 0;
			physics_debug_line_vertices = 0;
			physics_debug_boxes = 0;
			active_visibility_path = VisibilityPath::Instance;
			gpu_total_objects = 0;
			gpu_visible_objects = 0;
			gpu_total_instances = 0;
			gpu_visible_instances = 0;
			gpu_total_triangles = 0;
			gpu_visible_triangles = 0;
			cpu_total_objects = 0;
			cpu_visible_objects = 0;
			cpu_total_instances = 0;
			cpu_visible_instances = 0;
			cpu_total_triangles = 0;
			cpu_visible_triangles = 0;
			vg_total_clusters = 0;
			vg_visible_clusters = 0;
			vg_resident_pages = 0;
			vg_streaming_requests = 0;
			occluder_count = 0;
			occluder_triangles = 0;
			heuristic_total_count = 0;
			heuristic_cutoff_bucket = 0;
			heuristic_remaining = 0;
			gpu_occluder_instances = 0;
			shadow_casters = 0;
			shadow_caster_submeshes = 0;
		}
	};
}
