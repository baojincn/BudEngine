#pragma once
#include <memory>
#include <string>
#include <string_view>
#include "bud.ml.tensor.hpp"

namespace bud::graphics {
	class RHI;
	class CommandHandle;
}

namespace bud::ml {

	class InferBackendBase {
	public:
		virtual ~InferBackendBase() = default;

		virtual bool load_model(const std::string& path) = 0;

		virtual void bind_input(std::string_view name, const GpuTensor& tensor) = 0;
		virtual void bind_output(std::string_view name, const GpuTensor& tensor) = 0;

		virtual void record_dispatch(graphics::RHI* rhi, graphics::CommandHandle cmd) = 0;

		virtual const std::string& backend_name() const = 0;
	};
}
