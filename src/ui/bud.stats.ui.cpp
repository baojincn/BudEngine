#include "bud.stats.ui.hpp"
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <cmath>
#include <cfloat>
#include <string_view>
#include <cstring>
#include <string>
#include <format>
#include "src/runtime/bud.camera_sequencer.hpp"
#include <functional>

namespace bud::ui {

    void StatsUI::render(const bud::graphics::RenderStats& stats, float delta_time,
        bud::scene::SequencerState sequencer_state,
        size_t keyframe_count,
        size_t playback_index,
        bool is_paused,
        bool is_looping,
        bool show_stats,
        std::function<void(float)> set_occluder,
        float current_occluder,
        std::function<void(bool)> set_occluder_enable,
		bool current_occluder_enable,
		std::function<void(bool)> set_meshlet_rendering_enable,
		bool current_meshlet_rendering_enable) {

		if (show_stats) {
			ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.35f);
            // Prevent the stats window from auto-shrinking too small when internal
            // controls toggle glyphs. Reserve a sensible minimum width so text
            // doesn't reflow on control state changes.
            ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

            if (ImGui::Begin("Engine Stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
				static float update_timer = 0.0f;
				static float display_fps = 0.0f;
				static float display_ms = 0.0f;
				static constexpr float fps_ema_tau_seconds = 0.8f;
				static uint32_t display_draw_calls = 0;
				static uint32_t display_drawn_tris = 0;
				static uint32_t display_pipeline_binds = 0;

				static uint32_t cpu_display_visible_tris = 0;
				static uint32_t cpu_display_total_tris = 0;
				static uint32_t cpu_display_total_objs = 0;
				static uint32_t cpu_display_visible_objs = 0;
				static uint32_t cpu_display_total_instances = 0;
				static uint32_t cpu_display_visible_instances = 0;
				static uint32_t cpu_display_total_meshlets = 0;
				static uint32_t cpu_display_visible_meshlets = 0;

				static uint32_t gpu_display_visible_tris = 0;
				static uint32_t gpu_display_total_tris = 0;
				static uint32_t gpu_display_total_objs = 0;
				static uint32_t gpu_display_visible_objs = 0;
				static uint32_t gpu_display_total_instances = 0;
				static uint32_t gpu_display_visible_instances = 0;
				static uint32_t gpu_display_total_meshlets = 0;
				static uint32_t gpu_display_visible_meshlets = 0;

				static uint32_t display_shadow_casters = 0;
				static uint32_t display_occluder_count = 0;
				static uint32_t display_occluder_tris = 0;
				static uint32_t display_shadow_caster_submeshes = 0;

				float current_ms = delta_time * 1000.0f;
				float ema_alpha = (delta_time > 0.0f)
					? (1.0f - std::exp(-delta_time / fps_ema_tau_seconds))
					: 1.0f;
				if (display_ms <= 0.0f) {
					display_ms = current_ms;
				}
				else {
					display_ms = ema_alpha * current_ms + (1.0f - ema_alpha) * display_ms;
				}
				display_fps = (display_ms > 0.0f) ? (1000.0f / display_ms) : 0.0f;

				update_timer += delta_time;
				if (update_timer >= 0.5f) {
					display_draw_calls = stats.draw_calls;
					display_drawn_tris = stats.drawn_triangles;
					display_pipeline_binds = stats.pipeline_binds;

					cpu_display_total_tris = stats.cpu_total_triangles;
					cpu_display_visible_tris = stats.cpu_visible_triangles;
					cpu_display_total_objs = stats.cpu_total_objects;
					cpu_display_visible_objs = stats.cpu_visible_objects;
					cpu_display_total_instances = stats.cpu_total_instances;
					cpu_display_visible_instances = stats.cpu_visible_instances;
					cpu_display_total_meshlets = stats.cpu_total_meshlets;
					cpu_display_visible_meshlets = stats.cpu_visible_meshlets;

					gpu_display_total_tris = stats.gpu_total_triangles;
					gpu_display_visible_tris = stats.gpu_visible_triangles;
					gpu_display_total_objs = stats.gpu_total_objects;
					gpu_display_visible_objs = stats.gpu_visible_objects;
					gpu_display_total_instances = stats.gpu_total_instances;
					gpu_display_visible_instances = stats.gpu_visible_instances;
					gpu_display_total_meshlets = stats.gpu_total_meshlets;
					gpu_display_visible_meshlets = stats.gpu_visible_meshlets;

					display_shadow_casters = stats.shadow_casters;
					display_occluder_count = stats.occluder_count;
					display_occluder_tris = stats.occluder_triangles;
					display_shadow_caster_submeshes = stats.shadow_caster_submeshes;
					update_timer = 0.0f;
				}


				ImGui::SetWindowFontScale(1.5f);

				ImVec4 color_good(0.4f, 1.0f, 0.4f, 1.0f);
				ImVec4 color_warn(1.0f, 1.0f, 0.4f, 1.0f);
				ImVec4 color_bad(1.0f, 0.4f, 0.4f, 1.0f);
				ImVec4 color_neutral(0.9f, 0.9f, 0.9f, 1.0f);

				ImVec4 fps_color = display_fps >= 60.0f ? color_good : (display_fps >= 30.0f ? color_warn : color_bad);
				ImVec4 dc_color = display_draw_calls <= 5000 ? color_good : (display_draw_calls <= 10000 ? color_warn : color_bad);
				ImVec4 drawn_tri_color = display_drawn_tris <= 2000000 ? color_good : (display_drawn_tris <= 5000000 ? color_warn : color_bad);
				ImVec4 pipe_color = display_pipeline_binds <= 100 ? color_good : (display_pipeline_binds <= 500 ? color_warn : color_bad);
				const char* visibility_path_text = stats.active_visibility_path == bud::graphics::VisibilityPath::Meshlet
					? "Meshlet"
					: "Instance/Submesh Fallback";

				ImGui::TextColored(fps_color, "FPS: %.1f (%.2f ms)", display_fps, display_ms);
				//ImGui::TextColored(color_neutral, "Visibility Path: %s", visibility_path_text);

				// Build and show sequencer status using the supplied values to avoid cross-thread calls.
				std::string seq_status;
				if (sequencer_state == bud::scene::SequencerState::RECORDING) {
					seq_status = std::format("| REC [{} kf]", keyframe_count);
				}
				else if (sequencer_state == bud::scene::SequencerState::PLAYING) {
					if (is_paused) {
						seq_status = std::format("| PAUS [{}/{}]", playback_index, keyframe_count);
					}
					else {
						const char* mode = is_looping ? "LOOP" : "PLAY";
						seq_status = std::format("| {} [{}/{}]", mode, playback_index, keyframe_count);
					}
				}
				else if (keyframe_count > 0) {
					seq_status = std::format("| READY [{} kf]", keyframe_count);
				}

				if (!seq_status.empty()) {
					ImGui::SameLine();
					bool is_rec = seq_status.find("REC") != std::string::npos;
					bool is_play = seq_status.find("PLAY") != std::string::npos;
					ImVec4 seq_color = is_rec ? color_bad
						: is_play ? color_good
						: color_neutral;
					ImGui::TextColored(seq_color, " %.*s",
						static_cast<int>(seq_status.size()), seq_status.c_str());
				}
				ImGui::Separator();
				ImGui::TextColored(color_neutral, "Draw Stats");
				ImGui::TextColored(dc_color, "Draw Calls: %u", display_draw_calls);
				ImGui::TextColored(drawn_tri_color, "Rasterized Tris: %u", display_drawn_tris);
				ImGui::TextColored(pipe_color, "Pipeline Binds: %u", display_pipeline_binds);

				// CPU CULLING
				ImGui::Separator();
				ImGui::TextColored(color_neutral, "CPU Frustum Culling");
				ImGui::TextColored(color_neutral, "Total Objects/Entities: %u", cpu_display_total_objs);
				ImGui::TextColored(color_neutral, "Visible Objects/Entities: %u", cpu_display_visible_objs);
				float cpu_obj_cull_rate = cpu_display_total_objs > 0 ? (1.0f - (float)cpu_display_visible_objs / cpu_display_total_objs) * 100.0f : 0.0f;
				ImGui::TextColored(color_neutral, "Obj Cull Ratio: %.1f%%", cpu_obj_cull_rate);
				ImGui::TextColored(color_neutral, "Total Submesh Instances: %u", cpu_display_total_instances);
				ImGui::TextColored(color_neutral, "Visible Submesh Instances: %u", cpu_display_visible_instances);
				float cpu_instance_cull_rate = cpu_display_total_instances > 0 ? (1.0f - (float)cpu_display_visible_instances / cpu_display_total_instances) * 100.0f : 0.0f;
				ImGui::TextColored(color_neutral, "Instance Cull Ratio: %.1f%%", cpu_instance_cull_rate);

				ImGui::Separator();
				ImGui::TextColored(color_neutral, "Meshlet Rendering");
				if (set_meshlet_rendering_enable) {
					ImGui::SameLine();
					ImGui::PushID("meshlet_rendering_enable");
					ImGui::PushItemWidth(24.0f);
					bool tmp = current_meshlet_rendering_enable;
					if (ImGui::Checkbox("##meshlet_rendering_enable", &tmp)) {
						set_meshlet_rendering_enable(tmp);
					}
					ImGui::PopItemWidth();
					ImGui::PopID();
				}

				const auto draw_heuristic_occluder_controls = [&]() {
					if (set_occluder && current_occluder >= 0.0f) {
						ImGui::TextColored(color_neutral, "Heuristic Occluder Frac: %.1f%%", current_occluder * 100.0f);
						ImGui::SameLine();
						if (ImGui::SmallButton("-")) {
							float v = std::clamp(current_occluder - 0.01f, 0.0f, 1.0f);
							set_occluder(v);
						}
						ImGui::SameLine();
						if (ImGui::SmallButton("+")) {
							float v = std::clamp(current_occluder + 0.01f, 0.0f, 1.0f);
							set_occluder(v);
						}
						if (set_occluder_enable) {
							ImGui::SameLine();
							ImGui::PushID("heuristic_occluder_enable");
							ImGui::PushItemWidth(24.0f);
							bool tmp = current_occluder_enable;
							if (ImGui::Checkbox("##heuristic_occluder_enable", &tmp)) {
								set_occluder_enable(tmp);
							}
							ImGui::PopItemWidth();
							ImGui::PopID();
						}
					}
				};

				if (current_meshlet_rendering_enable) {
					ImGui::TextColored(color_neutral, "Occluders: %u", display_occluder_count);
					draw_heuristic_occluder_controls();

					ImGui::Separator();
					ImGui::TextColored(color_neutral, "Meshlet Frustum Culling");
					ImGui::TextColored(color_neutral, "Total Meshlets: %u", stats.meshlet_frustum_total_meshlets);
					ImGui::TextColored(color_neutral, "Visible Meshlets: %u", stats.meshlet_frustum_visible_meshlets);
					float meshlet_frustum_cull_rate = stats.meshlet_frustum_total_meshlets > 0 ? (1.0f - (float)stats.meshlet_frustum_visible_meshlets / stats.meshlet_frustum_total_meshlets) * 100.0f : 0.0f;
					ImGui::TextColored(color_neutral, "Meshlet Cull Ratio: %.1f%%", meshlet_frustum_cull_rate);

					ImGui::TextColored(color_neutral, "Selected Occluders: %u", display_occluder_count);
					ImGui::TextColored(color_neutral, "Occluder Tris: %u", display_occluder_tris);
					ImGui::TextColored(color_neutral, "Depth Only: %u occluders (visible %u)", stats.occluder_count, stats.cpu_visible_instances);

					ImGui::Separator();
					ImGui::TextColored(color_neutral, "Meshlet HiZ Culling");
					ImGui::TextColored(color_neutral, "Total Meshlets: %u", stats.meshlet_hiz_total_meshlets);
					ImGui::TextColored(color_neutral, "Visible Meshlets: %u", stats.meshlet_hiz_visible_meshlets);
					float meshlet_hiz_cull_rate = stats.meshlet_hiz_total_meshlets > 0 ? (1.0f - (float)stats.meshlet_hiz_visible_meshlets / stats.meshlet_hiz_total_meshlets) * 100.0f : 0.0f;
					ImGui::TextColored(color_neutral, "Meshlet Cull Ratio: %.1f%%", meshlet_hiz_cull_rate);
				}
				else {
					ImGui::TextColored(color_neutral, "Meshlet Rendering [Off]");
					draw_heuristic_occluder_controls();

					ImGui::Separator();
					ImGui::TextColored(color_neutral, "GPU Instance HiZ Group");
					ImGui::TextColored(color_neutral, "Total Objects/Entities: %u", gpu_display_total_objs);
					ImGui::TextColored(color_neutral, "Visible Objects/Entities: %u", gpu_display_visible_objs);
					float gpu_obj_cull_rate = gpu_display_total_objs > 0 ? (1.0f - (float)gpu_display_visible_objs / gpu_display_total_objs) * 100.0f : 0.0f;
					ImGui::TextColored(color_neutral, "Obj Cull Ratio: %.1f%%", gpu_obj_cull_rate);
					ImGui::TextColored(color_neutral, "Total Submesh Instances: %u", gpu_display_total_instances);
					ImGui::TextColored(color_neutral, "Visible Submesh Instances: %u", gpu_display_visible_instances);
					float gpu_instance_cull_rate = gpu_display_total_instances > 0 ? (1.0f - (float)gpu_display_visible_instances / gpu_display_total_instances) * 100.0f : 0.0f;
					ImGui::TextColored(color_neutral, "Instance Cull Ratio: %.1f%%", gpu_instance_cull_rate);
					ImGui::TextColored(color_neutral, "Total Triangles: %u", gpu_display_total_tris);
					ImVec4 gpu_tri_color = gpu_display_visible_tris <= 2000000 ? color_good : (gpu_display_visible_tris <= 5000000 ? color_warn : color_bad);
					ImGui::TextColored(gpu_tri_color, "Visible Triangles: %u", gpu_display_visible_tris);
					float gpu_tri_cull_rate = gpu_display_total_tris > 0 ? (1.0f - (float)gpu_display_visible_tris / gpu_display_total_tris) * 100.0f : 0.0f;
					ImGui::TextColored(color_neutral, "Tri Cull Ratio: %.1f%%", gpu_tri_cull_rate);
				}

				ImGui::Separator();
				ImGui::TextColored(color_neutral, "Shadow Casters: %u", display_shadow_casters);
				ImGui::TextColored(color_neutral, "Shadow Casters (Submeshes): %u", display_shadow_caster_submeshes);

				// Ensure a tiny bottom padding so auto-resize windows don't clip the last lines
				ImGui::Spacing();
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}
	}

} // namespace bud::ui
