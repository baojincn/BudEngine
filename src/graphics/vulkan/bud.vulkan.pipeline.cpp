#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <stdexcept>

#include "src/graphics/vulkan/bud.vulkan.pipeline.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"
#include "src/core/bud.logger.hpp"
#include <imgui.h>
#include <cstddef>

namespace bud::graphics::vulkan {

    void VulkanPipelineCache::init(VkDevice device) {
        this->device = device;
    }

    void VulkanPipelineCache::cleanup() {
        for (auto& [key, entry] : cache) {
            if (device && entry.pipeline) vkDestroyPipeline(device, entry.pipeline, nullptr);
        }
        cache.clear();
        pipeline_to_key.clear();

        for (auto pipe : compute_pipelines) {
            vkDestroyPipeline(device, pipe, nullptr);
        }
        compute_pipelines.clear();
        compute_pipeline_set.clear();
    }

    VkPipeline VulkanPipelineCache::get_pipeline(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only) {
        auto it = cache.find(key);
        if (it != cache.end()) {
            // Increment refcount and return existing pipeline
            it->second.refcount++;
            return it->second.pipeline;
        }

        VkPipeline new_pipe = create_pipeline_internal(key, layout, is_depth_only);
        PipelineEntry entry{ new_pipe, 1 };
        cache[key] = entry;
        // Record reverse mapping for O(1) lookup on release
        pipeline_to_key[new_pipe] = key;
        return new_pipe;
    }

    VkPipeline VulkanPipelineCache::create_pipeline_internal(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only) {
        
        VkPipelineShaderStageCreateInfo shaderStages[] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, key.vert_shader, "main", nullptr },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, key.frag_shader, "main", nullptr }
        };

        // Vertex Input (Hardcoded for Sponza sample based on MeshData::Vertex)
        // in Vulkan 1.3 Dynamic Rendering, we often use generic layouts.
        // But input state is needed.
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attributeDescriptions;

        switch (key.vertex_layout) {
        case VertexLayoutType::Default:
            bindingDescription.stride = sizeof(bud::io::MeshData::Vertex);
            attributeDescriptions = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, color)},
                {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, normal)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(bud::io::MeshData::Vertex, texture_uv)},
                {4, 0, VK_FORMAT_R32_SFLOAT,       offsetof(bud::io::MeshData::Vertex, texture_index)}
            };
            break;
        case VertexLayoutType::PositionOnly:
            bindingDescription.stride = sizeof(bud::io::MeshData::Vertex);
            attributeDescriptions = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, pos)}
            };
            break;
        case VertexLayoutType::PositionUV:
            bindingDescription.stride = sizeof(bud::io::MeshData::Vertex);
            attributeDescriptions = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, pos)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(bud::io::MeshData::Vertex, texture_uv)}
            };
            break;
        case VertexLayoutType::PositionNormal:
            bindingDescription.stride = sizeof(bud::io::MeshData::Vertex);
            attributeDescriptions = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, pos)},
                {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bud::io::MeshData::Vertex, normal)}
            };
            break;
        case VertexLayoutType::NoVertexInput:
            bindingDescription.stride = 0;
            attributeDescriptions = {};
            break;
        case VertexLayoutType::ImGui:
            bindingDescription.stride = sizeof(ImDrawVert);
            attributeDescriptions = {
                {0, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, pos)},
                {1, 0, VK_FORMAT_R32G32_SFLOAT,  offsetof(ImDrawVert, uv)},
                {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)}
            };
            break;
        }


        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = key.cull_mode;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = key.depth_bias_enable;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = key.depth_test;
        depthStencil.depthWriteEnable = key.depth_write;
		depthStencil.depthCompareOp = key.depth_compare_op;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (key.blending_enable) {
            colorBlendAttachment.blendEnable = VK_TRUE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {
            colorBlendAttachment.blendEnable = VK_FALSE;
        }

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = is_depth_only ? 0 : 1;
        colorBlending.pAttachments = is_depth_only ? nullptr : &colorBlendAttachment;

        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        if (key.depth_bias_enable) {
            dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
        }

        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Dynamic Rendering Info
        VkFormat colorFormat = key.color_format;
        if (colorFormat == VK_FORMAT_UNDEFINED && !is_depth_only) {
            colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
        }
		VkFormat depthFormat = key.depth_format;

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
		renderingInfo.pNext = nullptr;
        renderingInfo.viewMask = 0;
        
        // Support depth-only pipelines (shadow maps)
        if (is_depth_only) {
            renderingInfo.colorAttachmentCount = 0;
            renderingInfo.pColorAttachmentFormats = nullptr;
        } else {
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &colorFormat;
        }
        
        renderingInfo.depthAttachmentFormat = depthFormat;
        renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2; 
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        
        if (is_depth_only) {
            colorBlending.attachmentCount = 0;
            colorBlending.pAttachments = nullptr;
        }
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = layout;
        pipelineInfo.renderPass = VK_NULL_HANDLE; 
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
        pipelineInfo.basePipelineIndex = -1;

        VkPipeline graphicsPipeline = VK_NULL_HANDLE;
        // Diagnostic: record that we are about to create a graphics pipeline
        {
            std::string msg = std::format("[Vulkan][Worker] vkCreateGraphicsPipelines: vert_module={} frag_module={} color_fmt={} depth_fmt={}",
                (void*)shaderStages[0].module, (void*)shaderStages[1].module, (int)colorFormat, (int)depthFormat);
            bud::print("{}", msg);
        }

        VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline);
        if (r != VK_SUCCESS) {
            std::string msg = std::format("[Vulkan][Worker] vkCreateGraphicsPipelines failed: {}", (int)r);
            bud::eprint("{}", msg);
#if defined(_DEBUG)
            throw std::runtime_error("failed to create graphics pipeline!");
#else
            return VK_NULL_HANDLE;
#endif
        }

		std::string msg = std::format("[Vulkan][Worker] vkCreateGraphicsPipelines OK: pipeline={}", (void*)graphicsPipeline);
		bud::print("{}", msg);



        return graphicsPipeline;
    }

    VkPipeline VulkanPipelineCache::create_compute_pipeline(VkShaderModule compute_shader, VkPipelineLayout layout) {
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = compute_shader;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout;

        VkPipeline computePipeline;
        VkResult r2 = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline);
        if (r2 != VK_SUCCESS) {
            std::string msg = std::format("[Vulkan][Worker] vkCreateComputePipelines failed: {}", (int)r2);
            bud::eprint("{}", msg);
#if defined(_DEBUG)
            throw std::runtime_error("failed to create compute pipeline!");
#else
            return VK_NULL_HANDLE;
#endif
        }

        compute_pipelines.push_back(computePipeline);
        compute_pipeline_set.insert(computePipeline);
        return computePipeline;
    }

    void VulkanPipelineCache::release_pipeline(VkPipeline pipeline) {
        if (pipeline == VK_NULL_HANDLE) {
            std::string err = std::format("VulkanPipelineCache::release_pipeline called with VK_NULL_HANDLE");
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return;
#endif
        }
        // O(1) lookup using reverse map
        auto rit = pipeline_to_key.find(pipeline);
        if (rit != pipeline_to_key.end()) {
            const PipelineKey& key = rit->second;
            auto cit = cache.find(key);
            if (cit != cache.end()) {
                if (cit->second.refcount > 1) {
                    cit->second.refcount--;
                    bud::print("[VulkanPipelineCache] release_pipeline: decremented refcount for pipeline {} to {}", (void*)pipeline, cit->second.refcount);
                } else {
                    if (device && cit->second.pipeline) {
                        bud::print("[VulkanPipelineCache] release_pipeline: destroying pipeline {}", (void*)pipeline);
                        vkDestroyPipeline(device, cit->second.pipeline, nullptr);
                    }
                    cache.erase(cit);
                    pipeline_to_key.erase(rit);
                }
            } else {
                // Shouldn't happen: reverse map points to missing cache entry
                pipeline_to_key.erase(rit);
                bud::eprint("[VulkanPipelineCache] release_pipeline: reverse map had key but cache missing for pipeline {}", (void*)pipeline);
            }
            return;
        }

        // If not found in reverse map, check compute pipeline set
        auto pit = compute_pipeline_set.find(pipeline);
        if (pit != compute_pipeline_set.end()) {
            if (device && pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            compute_pipeline_set.erase(pit);
            // Also remove from vector
            auto vit = std::find(compute_pipelines.begin(), compute_pipelines.end(), pipeline);
            if (vit != compute_pipelines.end()) compute_pipelines.erase(vit);
            bud::print("[VulkanPipelineCache] release_pipeline: destroyed compute pipeline {}", (void*)pipeline);
            return;
        }

        bud::eprint("[VulkanPipelineCache] release_pipeline: pipeline {} not found in cache or compute set", (void*)pipeline);
    }
}
