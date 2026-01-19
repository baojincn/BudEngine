#include <cstdint>

export module bud.graphics.defs;


export namespace bud::graphics {

	// 资源状态
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

	// 纹理格式
	export enum class TextureFormat {
		RGBA8_UNORM,
		BGRA8_UNORM,
		R32G32B32_FLOAT,
		D32_FLOAT,
		D24_UNORM_S8_UINT,
		// ...
	};

	// 纹理类型
	export enum class TextureType {
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube
	};

	// 资源描述符
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

	// 抽象的 Command Buffer Handle
	export using CommandHandle = void*;
}
