#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <cstring>
#include <algorithm>

namespace bud::graphics {

	namespace {
		constexpr uint32_t imgui_font_bindless_slot = 999;
		constexpr uint32_t buffer_growth_padding = 4096;
	}

	UIPass::~UIPass() {}

	void UIPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (font_texture.is_valid() && rhi) {
			rhi->destroy_texture(font_texture);
			font_texture.reset();
		}
		if (rhi) {
			for (auto& buf : vertex_buffers) {
				if (buf.is_valid()) rhi->destroy_buffer(buf);
				buf.reset();
			}
			for (auto& buf : index_buffers) {
				if (buf.is_valid()) rhi->destroy_buffer(buf);
				buf.reset();
			}
		}
		vertex_buffers.clear();
		index_buffers.clear();
		current_vertex_buffer_sizes.clear();
		current_index_buffer_sizes.clear();
	}

	void UIPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		// Build font texture FIRST
		ImGuiIO& imgui_io = ImGui::GetIO();
		unsigned char* pixels;
		int width, height;
		imgui_io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		TextureDesc tex_desc;
		tex_desc.width = width;
		tex_desc.height = height;
		tex_desc.format = TextureFormat::RGBA8_SRGB;
		tex_desc.mips = 1;

		font_texture = rhi->create_texture(tex_desc, pixels, width * height * 4);
		rhi->set_debug_name(font_texture, ObjectType::Texture, "ImGui_Font_Atlas");

		font_bindless_index = imgui_font_bindless_slot;
		rhi->update_bindless_texture(font_bindless_index, font_texture);
		imgui_io.Fonts->SetTexID((ImTextureID)(intptr_t)font_bindless_index);

		load_shaders_async(asset_manager, { "src/shaders/debug_ui.vert.spv", "src/shaders/debug_ui.frag.spv" }, [this, rhi](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = false;
			desc.depth_write = false;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.depth_attachment_format = bud::graphics::TextureFormat::Undefined;
			desc.depth_compare_op = CompareOp::Always;
			desc.enable_depth_bias = false;
			desc.blending_enable = true;
			desc.vertex_layout = VertexLayoutType::ImGui;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline.is_valid()) {
				bud::print("[UIPass] Shaders loaded and pipeline created.");
			}
			});
	}

	void UIPass::update_draw_data(ImDrawData* draw_data) {
		UIDrawDataSnapshot ui_draw_data_snapshot;

		if (draw_data && draw_data->CmdListsCount > 0) {
			ui_draw_data_snapshot.display_pos = draw_data->DisplayPos;
			ui_draw_data_snapshot.display_size = draw_data->DisplaySize;
			ui_draw_data_snapshot.framebuffer_scale = draw_data->FramebufferScale;
			ui_draw_data_snapshot.lists.reserve(draw_data->CmdListsCount);

			for (int n = 0; n < draw_data->CmdListsCount; ++n) {
				const ImDrawList* src_list = draw_data->CmdLists[n];
				UIDrawListSnapshot dst_list;
				dst_list.vertices.assign(src_list->VtxBuffer.Data, src_list->VtxBuffer.Data + src_list->VtxBuffer.Size);
				dst_list.indices.reserve(src_list->IdxBuffer.Size);
				dst_list.commands.reserve(src_list->CmdBuffer.Size);

				for (int i = 0; i < src_list->IdxBuffer.Size; ++i) {
					dst_list.indices.push_back(static_cast<uint32_t>(src_list->IdxBuffer.Data[i]));
				}

				for (int cmd_i = 0; cmd_i < src_list->CmdBuffer.Size; ++cmd_i) {
					const ImDrawCmd& src_cmd = src_list->CmdBuffer[cmd_i];
					dst_list.commands.push_back({
						.clip_rect = src_cmd.ClipRect,
						.elem_count = src_cmd.ElemCount,
						.idx_offset = src_cmd.IdxOffset,
						.vtx_offset = src_cmd.VtxOffset,
						.texture_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>((void*)src_cmd.GetTexID()))
						});
				}

				ui_draw_data_snapshot.lists.push_back(std::move(dst_list));
			}
		}

		std::lock_guard lock(draw_data_mutex);
		cached_draw_data = std::move(ui_draw_data_snapshot);
	}

	void UIPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer) {
		rg.add_pass("UIPass",
			[&](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
			},
			[=, this](RHI* rhi, CommandHandle cmd) {
				UIDrawDataSnapshot draw_data;
				{
					std::lock_guard lock(draw_data_mutex);
					draw_data = cached_draw_data;
				}

				if (!draw_data.has_data())
					return;

				if (!pipeline.is_valid())
					return;

				uint32_t needed_vb_size = draw_data.total_vtx_count() * sizeof(ImDrawVert);
				uint32_t needed_ib_size = draw_data.total_idx_count() * sizeof(uint32_t);

				uint32_t frame_index = rhi->get_current_frame_index();
				uint32_t inflight = rhi->get_inflight_frame_count();
				if (vertex_buffers.size() < inflight) {
					vertex_buffers.resize(inflight);
					index_buffers.resize(inflight);
					current_vertex_buffer_sizes.resize(inflight, 0);
					current_index_buffer_sizes.resize(inflight, 0);
				}

				if (needed_vb_size > current_vertex_buffer_sizes[frame_index]) {
					if (vertex_buffers[frame_index].is_valid()) {
						rhi->destroy_buffer(vertex_buffers[frame_index]);
					}
					current_vertex_buffer_sizes[frame_index] = needed_vb_size + buffer_growth_padding;
					vertex_buffers[frame_index] = rhi->create_upload_buffer(current_vertex_buffer_sizes[frame_index]);
				}

				if (needed_ib_size > current_index_buffer_sizes[frame_index]) {
					if (index_buffers[frame_index].is_valid()) {
						rhi->destroy_buffer(index_buffers[frame_index]);
					}
					current_index_buffer_sizes[frame_index] = needed_ib_size + buffer_growth_padding;
					index_buffers[frame_index] = rhi->create_upload_buffer(current_index_buffer_sizes[frame_index]);
				}

				auto* vk_vb = rhi->get_buffer(vertex_buffers[frame_index]);
				auto* vk_ib = rhi->get_buffer(index_buffers[frame_index]);
				if (!vk_vb || !vk_vb->mapped_ptr || !vk_ib || !vk_ib->mapped_ptr) {
					return;
				}

				auto* vtx_dst = (ImDrawVert*)vk_vb->mapped_ptr;
				auto* idx_dst = (uint32_t*)vk_ib->mapped_ptr;

				for (const auto& cmd_list : draw_data.lists) {
					std::memcpy(vtx_dst, cmd_list.vertices.data(), cmd_list.vertices.size() * sizeof(ImDrawVert));
					vtx_dst += cmd_list.vertices.size();
					std::memcpy(idx_dst, cmd_list.indices.data(), cmd_list.indices.size() * sizeof(uint32_t));
					idx_dst += cmd_list.indices.size();
				}

				RenderPassBeginInfo ui_pass_info;
				ui_pass_info.color_attachments.push_back(rg.get_texture(backbuffer));
				ui_pass_info.clear_color = false;
				ui_pass_info.clear_depth = false;

				rhi->cmd_begin_render_pass(cmd, ui_pass_info);
				rhi->cmd_bind_pipeline(cmd, pipeline);

				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				rhi->cmd_bind_vertex_buffer(cmd, vertex_buffers[frame_index]);
				rhi->cmd_bind_index_buffer(cmd, index_buffers[frame_index]);

				float fb_width = draw_data.display_size.x * draw_data.framebuffer_scale.x;
				float fb_height = draw_data.display_size.y * draw_data.framebuffer_scale.y;

				rhi->cmd_set_viewport(cmd, fb_width, fb_height);

				struct PushConst {
					bud::math::vec2 scale;
					bud::math::vec2 translate;
					uint32_t texture_id;
					uint32_t padding[3];
				} push_const;

				push_const.scale[0] = 2.0f / draw_data.display_size.x;
				push_const.scale[1] = 2.0f / draw_data.display_size.y;
				push_const.translate[0] = -1.0f - draw_data.display_pos.x * push_const.scale[0];
				push_const.translate[1] = -1.0f - draw_data.display_pos.y * push_const.scale[1];
				push_const.texture_id = font_bindless_index;

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConst), &push_const);

				int global_vtx_offset = 0;
				int global_idx_offset = 0;
				ImVec2 clip_off = draw_data.display_pos;
				ImVec2 clip_scale = draw_data.framebuffer_scale;

				for (const auto& cmd_list : draw_data.lists) {
					for (const auto& pcmd : cmd_list.commands) {

						// Setup clip rectangle
						ImVec2 clip_min((pcmd.clip_rect.x - clip_off.x) * clip_scale.x, (pcmd.clip_rect.y - clip_off.y) * clip_scale.y);
						ImVec2 clip_max((pcmd.clip_rect.z - clip_off.x) * clip_scale.x, (pcmd.clip_rect.w - clip_off.y) * clip_scale.y);

						if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
						if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
						if (clip_max.x > fb_width) { clip_max.x = fb_width; }
						if (clip_max.y > fb_height) { clip_max.y = fb_height; }
						if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
							continue;

						// Scissor setup
						rhi->cmd_set_scissor(cmd, (int32_t)clip_min.x, (int32_t)clip_min.y, (uint32_t)(clip_max.x - clip_min.x), (uint32_t)(clip_max.y - clip_min.y));

						push_const.texture_id = pcmd.texture_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConst), &push_const);

						// Draw
						rhi->cmd_draw_indexed(cmd, pcmd.elem_count, 1, pcmd.idx_offset + global_idx_offset, pcmd.vtx_offset + global_vtx_offset, 0);
					}
					global_idx_offset += static_cast<int>(cmd_list.indices.size());
					global_vtx_offset += static_cast<int>(cmd_list.vertices.size());
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

}
