#version 460 core

layout(location = 0) in vec4 in_curr_clip;
layout(location = 1) in vec4 in_prev_clip;

layout(location = 0) out vec2 out_velocity;

void main() {
	vec2 curr_ndc = in_curr_clip.xy / (abs(in_curr_clip.w) > 1e-6 ? in_curr_clip.w : 1.0);
	vec2 prev_ndc = in_prev_clip.xy / (abs(in_prev_clip.w) > 1e-6 ? in_prev_clip.w : 1.0);

	// Velocity in screen UV coordinates: curr_uv - prev_uv = (curr_ndc - prev_ndc) * 0.5
	out_velocity = (curr_ndc - prev_ndc) * 0.5;
}
