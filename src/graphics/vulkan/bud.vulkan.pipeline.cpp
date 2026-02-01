module;
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <stdexcept>

module bud.vulkan.pipeline;

namespace bud::graphics::vulkan {

    void VulkanPipelineCache::init(VkDevice device) {
        this->device = device;
    }

    void VulkanPipelineCache::cleanup() {
        for (auto& [key, pipe] : cache) {
            vkDestroyPipeline(device, pipe, nullptr);
        }
        cache.clear();
    }

    VkPipeline VulkanPipelineCache::get_pipeline(const PipelineKey& key, VkPipelineLayout layout, bool is_depth_only) {
        if (cache.contains(key)) return cache[key];

        // 缓存未命中，执行昂贵的创建操作
        VkPipeline new_pipe = create_pipeline_internal(key, layout, is_depth_only);
        cache[key] = new_pipe;
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
        bindingDescription.stride = sizeof(float) * (3+3+3+2+1); // Vertex struct size
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> attributeDescriptions = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},     // Pos
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},    // Color
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24},    // Normal
            {3, 0, VK_FORMAT_R32G32_SFLOAT,    36},    // UV
            {4, 0, VK_FORMAT_R32_SFLOAT,       44}     // TexIndex
        };

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
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = key.cull_mode;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_TRUE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = key.depth_test;
        depthStencil.depthWriteEnable = key.depth_write;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE; // No Blending for Opaque

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = is_depth_only ? 0 : 1;
        colorBlending.pAttachments = is_depth_only ? nullptr : &colorBlendAttachment;

        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // Dynamic Rendering Info
        VkFormat colorFormat = key.color_format;
        if (colorFormat == VK_FORMAT_UNDEFINED && !is_depth_only) {
            colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
        }
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR; // Use KHR for compatibility
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

        VkPipeline graphicsPipeline;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline!");
        }
        return graphicsPipeline;
    }

}
