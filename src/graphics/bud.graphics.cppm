// src/bud.graphics.cppm

#include <vulkan/vulkan.h>

export module bud.graphics;

import bud.core;

export namespace bud::graphics {
    class VulkanContext {
    public:
        VulkanContext() = default;
        ~VulkanContext() = default;
    };
}