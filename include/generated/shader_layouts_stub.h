// Auto-generated stub for shader layouts
// This file is intended as a development-time placeholder.
// It should be replaced by a real generated header produced from shader reflection
// (SPIRV-Reflect) in the build/CI pipeline once the codegen is available.

#pragma once

namespace generated::shaders {

// Common descriptor set constants (stub)
inline constexpr uint32_t DESCRIPTOR_SET_0 = 0;

// ===== shader: cluster_debug.frag =====
namespace ClusterDebugFrag {
    // no descriptor bindings discovered in stub
    // inputs (varyings)
    inline constexpr uint32_t IN_FRAG_WORLD_POS = 0; // frag_world_pos (from vertex)
    inline constexpr uint32_t IN_FRAG_NORMAL = 1; // frag_normal
    inline constexpr uint32_t IN_INSTANCE_SEED = 2; // instance_seed
    inline constexpr uint32_t IN_CLUSTER_SEED = 3; // cluster_seed
}

// ===== shader: cluster_debug.vert =====
namespace ClusterDebugVert {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_UBO = 0; // ubo
    inline constexpr uint32_t BINDING_INSTANCE_BUFFER = 3; // instance_buffer

    // vertex inputs
    inline constexpr uint32_t ATTR_POSITION = 0; // in_position
    inline constexpr uint32_t ATTR_NORMAL = 2; // in_normal
}

// ===== shader: debug_ui.frag =====
namespace DebugUiFrag {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_BINDLESS_TEXTURES = 1; // bindless_textures

    // inputs
    inline constexpr uint32_t IN_FRAGMENT_COLOR = 0;
    inline constexpr uint32_t IN_FRAGMENT_UV = 1;

    // push constants
    inline constexpr size_t PUSHCONST_PUSH_DATA_SIZE = 20;
}

// ===== shader: debug_ui.vert =====
namespace DebugUiVert {
    // vertex attributes
    inline constexpr uint32_t ATTR_POSITION = 0; // vertex_position
    inline constexpr uint32_t ATTR_UV = 1; // vertex_uv
    inline constexpr uint32_t ATTR_COLOR = 2; // vertex_color
}

// ===== shader: fullscreen.vert =====
namespace FullscreenVert {
    inline constexpr uint32_t ATTR_UNUSED_VERTEX_INDEX = 0; // built-in gl_VertexIndex
    inline constexpr uint32_t OUT_UV = 0; // out_uv
}

// ===== shader: hiz_cull.comp =====
namespace HizCullComp {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_STATS = 2; // stats
    inline constexpr uint32_t BINDING_HIZ_PYRAMID = 3; // hizPyramid
    inline constexpr uint32_t BINDING_UBO = 4; // ubo

    // push constants
    inline constexpr size_t PUSHCONST_PC_SIZE = 4;
}

// ===== shader: hiz_debug.frag =====
namespace HizDebugFrag {
    inline constexpr uint32_t IN_UV = 0;
    inline constexpr uint32_t OUT_COLOR = 0;
    inline constexpr size_t PUSHCONST_PC_SIZE = 4;
}

// ===== shader: hiz_mip.comp =====
namespace HizMipComp {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_IN_MIP = 3;
    inline constexpr uint32_t BINDING_OUT_MIP = 5;
    inline constexpr size_t PUSHCONST_PC_SIZE = 16;
}

// ===== shader: main.frag =====
namespace MainFrag {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_UBO = 0; // ubo
    inline constexpr uint32_t BINDING_TEX_SAMPLERS = 1; // tex_samplers
    inline constexpr uint32_t BINDING_SHADOW_MAP = 2; // shadow_map

    // inputs (from vertex)
    inline constexpr uint32_t IN_FRAG_WORLD_POS = 0;
    inline constexpr uint32_t IN_FRAG_NORMAL = 1;
    inline constexpr uint32_t IN_FRAG_TEX_COORD = 2;
    inline constexpr uint32_t IN_FRAG_COLOR = 3;
    inline constexpr uint32_t IN_FRAG_MATERIAL_ID = 4;
}

// ===== shader: main.vert =====
namespace MainVert {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_UBO = 0;
    inline constexpr uint32_t BINDING_INSTANCE_BUFFER = 3;

    inline constexpr uint32_t ATTR_POSITION = 0; // in_position
    inline constexpr uint32_t ATTR_COLOR = 1; // in_color
    inline constexpr uint32_t ATTR_TEX_COORD = 3; // in_tex_coord
    inline constexpr uint32_t ATTR_TEX_INDEX = 4; // in_tex_index
}

// ===== shader: ml_depth_downsample.comp =====
namespace MLDDepthDownsampleComp {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_IN_DEPTH = 0;
    inline constexpr uint32_t BINDING_OUT_DEPTH = 1;
}

// ===== shader: ml_identity.comp =====
namespace MLIdentityComp {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_0 = 0;
    inline constexpr uint32_t BINDING_1 = 1;
}

// ===== shader: shadow.frag / shadow.vert =====
namespace ShadowFrag {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_TEX_SAMPLERS = 1;
    inline constexpr size_t PUSHCONST_PUSH_SIZE = 160;
}
namespace ShadowVert {
    inline constexpr uint32_t ATTR_POSITION = 0;
    inline constexpr uint32_t ATTR_TEX_COORD = 3;
    inline constexpr size_t PUSHCONST_SIZE = 148;
}

// ===== shader: zprepass.frag / zprepass.vert =====
namespace ZPrepassFrag {
    inline constexpr uint32_t SET_0 = 0;
    inline constexpr uint32_t BINDING_TEX_SAMPLERS = 1;
    inline constexpr size_t PUSHCONST_SIZE = 80;
}
namespace ZPrepassVert {
    inline constexpr uint32_t ATTR_POSITION = 0;
    inline constexpr uint32_t ATTR_TEX_COORD = 3;
}

// Add more stubs as needed. Keep names stable and semantic.

} // namespace generated::shaders
