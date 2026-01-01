// src/bud.graphics.cppm

#include <vulkan/vulkan.h>

export module bud.graphics;

import bud.core;

export namespace bud::graphics {
    class VulkanRHI {
    public:
        VulkanRHI() = default;
        ~VulkanRHI() = default;
    };
}