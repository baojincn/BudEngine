#pragma once
#include <vector>
#include <string_view>
#include <variant>
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::ml {

	enum class DataType {
		Float32,
		Float16,
		Int8,
		Uint32
	};

	// 抽象 GPU Tensor：它是对引擎 Buffer 或 Image 的轻量级 View
	class GpuTensor {
	public:
		GpuTensor(std::vector<uint32_t> shape, DataType type, graphics::BufferHandle buffer, size_t offset = 0)
			: m_shape(std::move(shape)), m_type(type), m_resource(buffer), m_offset(offset) {
		}

		GpuTensor(std::vector<uint32_t> shape, DataType type, graphics::ImageHandle image)
			: m_shape(std::move(shape)), m_type(type), m_resource(image), m_offset(0) {
		}

		const std::vector<uint32_t>& shape() const { return m_shape; }
		DataType data_type() const { return m_type; }
		size_t offset() const { return m_offset; }

		bool is_buffer() const { return std::holds_alternative<graphics::BufferHandle>(m_resource); }
		bool is_image() const { return std::holds_alternative<graphics::ImageHandle>(m_resource); }

		graphics::BufferHandle as_buffer() const {
			assert(is_buffer());
			return std::get<graphics::BufferHandle>(m_resource);
		}

		graphics::ImageHandle as_image() const {
			assert(is_image());
			return std::get<graphics::ImageHandle>(m_resource);
		}

		size_t element_count() const {
			size_t count = 1;
			for (auto d : m_shape) count *= d;
			return count;
		}

	private:
		std::vector<uint32_t> m_shape;
		DataType m_type;
		std::variant<graphics::BufferHandle, graphics::ImageHandle> m_resource;
		size_t m_offset;
	};
}
