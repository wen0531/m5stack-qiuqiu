// Qiuqiu procedural avatar engine for M5Stack StopWatch (466x466 AMOLED).
// Rendering style/method inspired by KK (Trentct/m5stack-stopwatch-avatar, AGPL):
//   procedural geometry + smoothing + dirty-free simple rasterizer.
// Character geometry/data derived from the qiuqiu DSL design (aora-bot eye rings,
// Emotion Ball Community License, non-commercial).
#pragma once

#include <stdint.h>

namespace qiuqiu {

// Initialize for a WxH RGB565 framebuffer.
void init(int width, int height);

// Same, but with an explicit on-screen ball radius in pixels (lets the app
// render into a smaller off-screen viewport while keeping the ball size).
void init(int width, int height, float ball_radius_px);

// Switch expression (0..kExprCount-1). Blends smoothly from current pose.
void set_expression(int expr, uint32_t now_ms);

/* 0 = Hermes (white), 1 = Codex (blue), 2 = DSH (red) */
void set_agent_color(int idx);
int agent_color_index();
int cycle_agent_color();   /* advance 0->1->2->0, returns the new index */
int expression();
const char* expression_name(int expr);
int expression_count();

// Optional gaze -1..1 (e.g. from IMU later); 0,0 = neutral.
void set_gaze(float h, float v);

// Trigger a blink now.
void request_blink(uint32_t now_ms);

// Jelly impulse (bounce/squash), e.g. on tap.
void poke(uint32_t now_ms);

// Tuning knobs (for preview / device profiling).
//   omega      : spring angular frequency (rad/s), higher = faster bounce
//   zeta       : damping ratio, lower = longer wobble
//   amp_scale  : multiplier for per-expression deformation amplitude
//   idle_scale : multiplier for the idle micro-jiggle
void set_tuning(float omega, float zeta, float amp_scale, float idle_scale);

// Eye outline smoothing: 1 = 48 pts, 2 = 96 pts (default), 3 = 192 pts.
void set_eye_smoothing(int iterations);

// Eye roundness warp: 0 = faithful ring shape, 0.4 = noticeably rounder/plumper.
void set_eye_roundness(float r);

// Render one frame into an RGB565 buffer of W*H pixels (row-major).
void render(uint16_t* fb, uint32_t now_ms);

// Millisecond time helper-free: caller passes now_ms.

}  // namespace qiuqiu
