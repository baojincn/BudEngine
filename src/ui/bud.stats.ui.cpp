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
		std::function<void(bud::graphics::AOMode)> set_ao_mode,
		bud::graphics::AOMode current_ao_mode,
		std::function<void(bool)> set_ssr_enable,
		bool current_ssr_enable,
		std::function<void(bool)> set_ssgi_enable,
		bool current_ssgi_enable,
		std::function<void(float)> set_ssgi_intensity,
		float current_ssgi_intensity,
		std::function<void(float)> set_ssgi_blend,
		float current_ssgi_blend,
		std::function<void(float)> set_light_elevation,
		float current_light_elevation,
		std::function<void(float)> set_light_azimuth,
		float current_light_azimuth,
		std::function<void(bud::math::vec3)> set_light_color,
		bud::math::vec3 current_light_color,
		std::function<void(float)> set_light_intensity,
		float current_light_intensity,
		std::function<void(float)> set_ambient_strength,
		float current_ambient_strength) {

		if (show_stats) {
			ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

            if (ImGui::Begin("Engine Stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
				static float update_timer = 0.0f;
				static float display_fps = 0.0f;
				static float display_ms = 0.0f;
				static float display_gpu_ms = 0.0f;
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

				static uint32_t gpu_display_visible_tris = 0;
				static uint32_t gpu_display_total_tris = 0;
				static uint32_t gpu_display_total_objs = 0;
				static uint32_t gpu_display_visible_objs = 0;
				static uint32_t gpu_display_total_instances = 0;
				static uint32_t gpu_display_visible_instances = 0;

				static uint32_t display_heuristic_total_count = 0;
				static uint32_t display_heuristic_cutoff_bucket = 0;
				static uint32_t display_heuristic_remaining = 0;

				static uint32_t display_shadow_casters = 0;
				static uint32_t display_occluder_count = 0;
				static uint32_t display_occluder_tris = 0;
				static uint32_t display_shadow_caster_submeshes = 0;

				static uint32_t display_physics_debug_calls = 0;
				static uint32_t display_physics_debug_boxes = 0;
				static uint32_t display_physics_debug_verts = 0;

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

				// GPU frame time (Vulkan timestamp query read back 1-2 frames
				// late). Smoothed with the same EMA as the CPU frame time.
				if (stats.gpu_render_time > 0.0f) {
					if (display_gpu_ms <= 0.0f) {
						display_gpu_ms = stats.gpu_render_time;
					}
					else {
						display_gpu_ms = ema_alpha * stats.gpu_render_time + (1.0f - ema_alpha) * display_gpu_ms;
					}
				}

				update_timer += delta_time;
				if (update_timer >= 0.5f) {
					display_draw_calls = stats.draw_calls;
					display_drawn_tris = stats.gpu_visible_triangles;
					display_pipeline_binds = stats.pipeline_binds;
					display_physics_debug_calls = stats.physics_debug_draw_calls;
					display_physics_debug_boxes = stats.physics_debug_boxes;
					display_physics_debug_verts = stats.physics_debug_line_vertices;

					cpu_display_total_tris = stats.cpu_total_triangles;
					cpu_display_visible_tris = stats.cpu_visible_triangles;
					cpu_display_total_objs = stats.cpu_total_objects;
					cpu_display_visible_objs = stats.cpu_visible_objects;
					cpu_display_total_instances = stats.cpu_total_instances;
					cpu_display_visible_instances = stats.cpu_visible_instances;

					gpu_display_total_tris = stats.gpu_total_triangles;
					gpu_display_visible_tris = stats.gpu_visible_triangles;
					gpu_display_total_objs = stats.gpu_total_objects;
					gpu_display_visible_objs = stats.gpu_visible_objects;
					gpu_display_total_instances = stats.gpu_total_instances;
					gpu_display_visible_instances = stats.gpu_visible_instances;

					display_heuristic_total_count = stats.heuristic_total_count;
					display_heuristic_cutoff_bucket = stats.heuristic_cutoff_bucket;
					display_heuristic_remaining = stats.heuristic_remaining;

					display_shadow_casters = stats.shadow_casters;
					display_occluder_count = current_occluder_enable ? stats.gpu_occluder_instances : stats.occluder_count;
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
				const char* visibility_path_text = stats.active_visibility_path == bud::graphics::VisibilityPath::Cluster
					? "Cluster"
					: "Instance/Submesh";

				ImGui::TextColored(fps_color, "FPS: %.1f (%.2f ms CPU / %.2f ms GPU)", display_fps, display_ms, display_gpu_ms);
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
				// Physics wireframe overlay: attributed separately because it is a single
				// LINE_LIST draw call that otherwise vanishes inside draw_calls. Non-zero
				// here == the overlay really is being submitted (F3 hotkey is working).
				ImGui::TextColored(display_physics_debug_calls ? color_warn : color_neutral,
				                   "Physics Debug: %u calls, %u boxes, %u line verts",
				                   display_physics_debug_calls, display_physics_debug_boxes, display_physics_debug_verts);


				if (cpu_display_total_objs > 0) {
					ImGui::Separator();
					ImGui::TextColored(color_neutral, "Non-VG Frustum Culling (CPU Phase)");
					ImGui::TextColored(color_neutral, "Total Non-VG Objects: %u", cpu_display_total_objs);
					ImGui::TextColored(color_neutral, "Visible Non-VG Objects: %u", cpu_display_visible_objs);
					float cpu_obj_cull_rate = (1.0f - (float)cpu_display_visible_objs / cpu_display_total_objs) * 100.0f;
					ImGui::TextColored(color_neutral, "Non-VG Cull Ratio: %.1f%%", cpu_obj_cull_rate);
				}

				ImGui::Separator();
				ImGui::TextColored(color_neutral, "Cluster Rendering");

				if (set_ao_mode) {
					ImGui::SameLine();
					ImGui::TextColored(color_neutral, " | AO:");
					ImGui::SameLine();
					ImGui::PushID("ao_mode_combo");
					ImGui::PushItemWidth(70.0f);
					int current_idx = static_cast<int>(current_ao_mode);
					const char* items[] = { "Off", "SSAO", "GTAO" };
					if (ImGui::Combo("##ao_mode", &current_idx, items, IM_ARRAYSIZE(items))) {
						set_ao_mode(static_cast<bud::graphics::AOMode>(current_idx));
					}
					ImGui::PopItemWidth();
					ImGui::PopID();
				}

				if (set_ssr_enable) {
					ImGui::SameLine();
					ImGui::TextColored(color_neutral, " | SSR");
					ImGui::SameLine();
					ImGui::PushID("ssr_enable_checkbox");
					bool tmp_ssr = current_ssr_enable;
					if (ImGui::Checkbox("##ssr_enable", &tmp_ssr)) {
						set_ssr_enable(tmp_ssr);
					}
					ImGui::PopID();
				}

				if (set_ssgi_enable) {
					ImGui::SameLine();
					ImGui::TextColored(color_neutral, " | SSGI");
					ImGui::SameLine();
					ImGui::PushID("ssgi_enable_checkbox");
					bool tmp_ssgi = current_ssgi_enable;
					if (ImGui::Checkbox("##ssgi_enable", &tmp_ssgi)) {
						set_ssgi_enable(tmp_ssgi);
					}
					ImGui::PopID();
				}

				if (current_ssgi_enable && (set_ssgi_intensity || set_ssgi_blend)) {
					if (set_ssgi_intensity) {
						ImGui::TextColored(color_neutral, "SSGI Int: %.1f", current_ssgi_intensity);
						ImGui::SameLine();
						ImGui::PushID("ssgi_intensity_slider");
						ImGui::PushItemWidth(60.0f);
						float tmp = current_ssgi_intensity;
						if (ImGui::SliderFloat("##ssgi_intensity", &tmp, 0.0f, 4.0f, "%.1f")) {
							set_ssgi_intensity(tmp);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
					if (set_ssgi_blend) {
						ImGui::SameLine();
						ImGui::TextColored(color_neutral, " | Blend: %.2f", current_ssgi_blend);
						ImGui::SameLine();
						ImGui::PushID("ssgi_blend_slider");
						ImGui::PushItemWidth(60.0f);
						float tmp = current_ssgi_blend;
						if (ImGui::SliderFloat("##ssgi_blend", &tmp, 0.01f, 0.20f, "%.2f")) {
							set_ssgi_blend(tmp);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
				}

				const auto draw_heuristic_occluder_controls = [&]() {
					if (set_occluder && current_occluder >= 0.0f) {
						ImGui::TextColored(color_neutral, "Heuristic Occluder Frac: %.1f%%", current_occluder * 100.0f);
						ImGui::SameLine();
						ImGui::PushID("occluder_slider");
						ImGui::PushItemWidth(60.0f);
						float tmp = current_occluder;
						if (ImGui::SliderFloat("##occluder", &tmp, 0.0f, 1.0f, "%.2f")) {
							set_occluder(tmp);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
					if (set_occluder_enable) {
						ImGui::SameLine();
						ImGui::TextColored(color_neutral, " | Heuristic Occluder");
						ImGui::SameLine();
						ImGui::PushID("occluder_enable");
						ImGui::PushItemWidth(24.0f);
						bool tmp = current_occluder_enable;
						if (ImGui::Checkbox("##occluder_enable", &tmp)) {
							set_occluder_enable(tmp);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
				};

				// --- Directional light editing -----------------------------------------------
				// Direction is edited as elevation / azimuth (degrees). Angle-driven editing can
				// never produce a zero vector, so the per-frame normalize() in the engine and the
				// CSM light-space matrices stay safe no matter what the user drags.
				if (set_light_elevation || set_light_azimuth || set_light_color ||
					set_light_intensity || set_ambient_strength) {
					ImGui::Separator();
					ImGui::TextColored(color_neutral, "Directional Light");
				}

				// Elev / Azim 放在同一行,两个滑块平分扣除标签后的整行宽度。
				if (set_light_elevation || set_light_azimuth) {
					const float row_w = ImGui::GetContentRegionAvail().x;
					const float spacing = ImGui::GetStyle().ItemSpacing.x;
					const std::string elev_label = std::format("Elev: {:.0f}\xC2\xB0", current_light_elevation);
					const std::string azim_label = std::format(" | Azim: {:.0f}\xC2\xB0", current_light_azimuth);
					const float elev_label_w = ImGui::CalcTextSize(elev_label.c_str()).x;
					const float azim_label_w = ImGui::CalcTextSize(azim_label.c_str()).x;
					// [elev_label][slider_elev][azim_label][slider_azim], 3 个 ItemSpacing
					float slider_total = row_w - elev_label_w - azim_label_w - spacing * 3.0f;
					if (slider_total < 20.0f) slider_total = 20.0f;
					const float elev_w = slider_total * 0.5f;
					const float azim_w = slider_total - elev_w;

					if (set_light_elevation) {
						ImGui::TextColored(color_neutral, "%s", elev_label.c_str());
						ImGui::SameLine();
						ImGui::PushID("light_elev_slider");
						ImGui::PushItemWidth(elev_w);
						float tmp_elev = current_light_elevation;
						if (ImGui::SliderFloat("##light_elev", &tmp_elev, -90.0f, 90.0f, "%.0f")) {
							set_light_elevation(tmp_elev);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
					if (set_light_azimuth) {
						ImGui::SameLine();
						ImGui::TextColored(color_neutral, "%s", azim_label.c_str());
						ImGui::SameLine();
						ImGui::PushID("light_azim_slider");
						ImGui::PushItemWidth(azim_w);
						float tmp_azim = current_light_azimuth;
						if (ImGui::SliderFloat("##light_azim", &tmp_azim, 0.0f, 359.0f, "%.0f")) {
							set_light_azimuth(tmp_azim);
						}
						ImGui::PopItemWidth();
						ImGui::PopID();
					}
				}

				if (set_light_color) {
					ImGui::TextColored(color_neutral, "Color:");
					ImGui::SameLine();
					ImGui::PushID("light_color_picker");
					float light_color_tmp[3] = { current_light_color.r, current_light_color.g, current_light_color.b };
					if (ImGui::ColorEdit3("##light_color", light_color_tmp, ImGuiColorEditFlags_NoInputs)) {
						set_light_color(bud::math::vec3(light_color_tmp[0], light_color_tmp[1], light_color_tmp[2]));
					}
					ImGui::PopID();
				}

				if (set_light_intensity) {
					ImGui::SameLine();
					ImGui::TextColored(color_neutral, " | Int: %.1f", current_light_intensity);
					ImGui::SameLine();
					ImGui::PushID("light_intensity_slider");
					ImGui::PushItemWidth(60.0f);
					float tmp_int = current_light_intensity;
					if (ImGui::SliderFloat("##light_int", &tmp_int, 0.0f, 20.0f, "%.1f")) {
						set_light_intensity(tmp_int);
					}
					ImGui::PopItemWidth();
					ImGui::PopID();
				}
				if (set_ambient_strength) {
					ImGui::SameLine();
					ImGui::TextColored(color_neutral, " | Ambient: %.2f", current_ambient_strength);
					ImGui::SameLine();
					ImGui::PushID("light_ambient_slider");
					ImGui::PushItemWidth(60.0f);
					float tmp_amb = current_ambient_strength;
					if (ImGui::SliderFloat("##light_ambient", &tmp_amb, 0.0f, 1.0f, "%.2f")) {
						set_ambient_strength(tmp_amb);
					}
					ImGui::PopItemWidth();
					ImGui::PopID();
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
