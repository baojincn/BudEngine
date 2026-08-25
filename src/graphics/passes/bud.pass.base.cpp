#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/io/bud.io.hpp"
#include <atomic>

namespace bud::graphics {

	void RenderPass::load_shaders_async(bud::io::AssetManager* asset_manager,
		const std::vector<std::string>& paths,
		std::function<void(std::vector<std::vector<char>>)> on_loaded) {
		if (paths.empty()) {
			on_loaded({});
			return;
		}

		struct Context {
			std::vector<std::vector<char>> results;
			std::atomic<size_t> loaded_count{ 0 };
			std::function<void(std::vector<std::vector<char>>)> on_loaded;
		};
		auto ctx = std::make_shared<Context>();
		ctx->results.resize(paths.size());
		ctx->on_loaded = std::move(on_loaded);

		for (size_t i = 0; i < paths.size(); ++i) {
			asset_manager->load_file_async(paths[i], [ctx, i, count = paths.size()](std::vector<char> code) {
				ctx->results[i] = std::move(code);
				if (++ctx->loaded_count == count) {
					ctx->on_loaded(ctx->results);
				}
				});
		}
	}

	void RenderPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(pipeline);
			pipeline.reset();
		}
	}

}
