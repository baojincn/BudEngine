module;

#include <vector>

export module bud.graphics.passes;

import bud.graphics;
import bud.graphics.defs;
import bud.graphics.graph;
import bud.math;

export namespace bud::graphics {

	class CSMShadowPass {
	public:
		struct ShadowData {
			bud::math::mat4 light_space_matrix;
			bud::math::vec4 light_dir;
			// ... 级联数据 ...
		};

		void add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config) {
			rg.add_pass("CSM Shadow",
				[&](RGBuilder& builder) { /* ... */ },
				[=](RHI* rhi, CommandHandle cmd) {

				}
			);
		}
	};

	// --- 2. 主光照 Pass ---
	class MainPass {
	public:
		void init(RHI* rhi) {}

		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer) {
			rg.add_pass("Main Lighting Pass",
				[&](RGBuilder& builder) {
					// 声明依赖：读 ShadowMap，写 Backbuffer
					// builder.read(shadow_map, ResourceState::ShaderResource);
					builder.write(backbuffer, ResourceState::RenderTarget);
				},
				[=](RHI* rhi, CommandHandle cmd) {
					// rhi->render_main_pass_commands(cmd);
				}
			);
		}
	};
}
