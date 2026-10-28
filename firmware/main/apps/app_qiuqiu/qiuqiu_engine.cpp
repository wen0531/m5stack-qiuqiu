// Qiuqiu procedural avatar engine — implementation.
//
// Rendering: deformed-ball jelly physics + anti-aliased scanline polygon fill.
//   - ball radius modulated by low-order deformation modes (k = 2,3,4); each is
//     a damped spring -> squash/stretch and "duang" bounce.
//   - eye outlines are Catmull-Rom smoothed (24 -> 192 points) and rasterized
//     with sub-pixel coverage, so no polygon facets or jaggies.
#include "qiuqiu_engine.h"

#include <math.h>
#include <string.h>

#include "qiuqiu_data.h"

namespace qiuqiu {
namespace {

int g_w = 466;
int g_h = 466;
float g_cs = 1.0f;      // design px -> screen px
float g_cx = 233.0f;    // screen center
float g_cy = 233.0f;

int g_expr = 0;
uint32_t g_expr_at = 0;
float g_gaze_h = 0.0f;
float g_gaze_v = 0.0f;

// smoothed base eye points (design space, before deformation)
float g_curL[24][2];
float g_curR[24][2];
bool g_have_cur = false;
uint32_t g_last_ms = 0;

// blink state
bool g_blink_active = false;
uint32_t g_blink_start = 0;
uint32_t g_next_blink = 0;

constexpr uint16_t kColorBall = 0xFFFF;  // #ffffff (pure white, per Wen 2026-10-20)

/* Active backend identity: white = Hermes, blue = Codex, red = DSH. */
uint16_t g_ball_color = 0xFFFF;

constexpr uint16_t kColorEye = 0x0000;   // #000000 (pure black eyes)
constexpr int kBallSegs = 96;           // deformed-ball outline resolution
constexpr int kEyeSub = 4;               // Chaikin iterations: 24 -> 96 points
constexpr int kEyePts = 96;              // Chaikin (24 -> 48 -> 96)
constexpr int kSubRows = 2;              // vertical supersampling for AA fill

// --- jelly deformation state ---
struct JellyMode {
    float x, v;      // value + velocity
    float target;    // preferred deformation
};
struct Jelly {
    JellyMode c2, s2;  // cos/sin 2theta (squash / tilt)
    JellyMode c3, s3;  // 3theta
    JellyMode c4;      // 4theta
    float bounce;      // ball-center vertical offset (px, design space)
    float bounce_v;
};
Jelly g_jelly;
float g_phase = 0.0f;  // free-running oscillator for idle jiggle

// runtime tuning
float g_omega = 2.0f * 3.14159265f * 2.1f;
float g_zeta = 0.16f;
float g_amp_scale = 1.0f;
float g_idle_scale = 1.0f;
int g_eye_iters = 1;
int g_eye_npts = 96;
float g_eye_round = 0.0f;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float smoothstep(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float frac(float v)
{
    return v - floorf(v);
}

// Design coordinates are 320x240 with ball center (160,120), radius 80.
inline float sx(float dx)
{
    return (dx - 160.0f) * g_cs + g_cx;
}

inline float sy(float dy)
{
    return (dy - 120.0f) * g_cs + g_cy;
}

// --- waveforms (ported from the DSL semantics) ---
float wave_glance(uint32_t now, uint16_t period, uint16_t phase)
{
    float u = (float)((now + phase) % period) / (float)period;
    if (u < 0.10f) return smoothstep(u / 0.10f);
    if (u < 0.45f) return 1.0f;
    if (u < 0.55f) return 1.0f - 2.0f * smoothstep((u - 0.45f) / 0.10f);
    if (u < 0.90f) return -1.0f;
    return -1.0f + smoothstep((u - 0.90f) / 0.10f);
}

float wave_sine(uint32_t now, uint16_t period, uint16_t phase)
{
    float u = (float)((now + phase) % period) / (float)period;
    float tri = 1.0f - 2.0f * fabsf(u * 2.0f - 1.0f);
    return tri * (3.0f - tri * tri) / 2.0f;
}

float wave_jitter(uint32_t now, uint16_t period, uint16_t phase)
{
    uint32_t seg = (now + phase) / period;
    float u = frac((float)seg * 0.618034f);
    return u * 2.0f - 1.0f;
}

float wave_value(const AnimData& a, uint32_t now)
{
    switch (a.wave) {
    case WAVE_GLANCE: return wave_glance(now, a.period_ms ? a.period_ms : 1000, a.phase_ms);
    case WAVE_SINE: return wave_sine(now, a.period_ms ? a.period_ms : 1000, a.phase_ms);
    case WAVE_JITTER: return wave_jitter(now, a.period_ms ? a.period_ms : 100, a.phase_ms);
    default: return 0.0f;
    }
}

int pick_ring(const ExprData& e, uint32_t now)
{
    if (e.pool_count <= 1) return e.pools[0];
    uint32_t seg = now / (e.pool_ms ? e.pool_ms : 1000);
    float u = (float)seg * 0.618034f;
    int slot = (int)(frac(u) * (float)e.pool_count);
    if (slot < 0) slot = 0;
    if (slot >= e.pool_count) slot = e.pool_count - 1;
    return e.pools[slot];
}

float blink_factor(uint32_t now)
{
    if (g_next_blink == 0) {
        g_next_blink = now + 2500;
    }
    if (!g_blink_active && (int32_t)(now - g_next_blink) >= 0) {
        g_blink_active = true;
        g_blink_start = now;
    }
    if (!g_blink_active) return 1.0f;

    const uint32_t close_ms = 110;
    const uint32_t open_ms = 190;
    uint32_t t = now - g_blink_start;
    float f;
    if (t < close_ms) {
        f = 1.0f - smoothstep((float)t / (float)close_ms);
    } else if (t < close_ms + open_ms) {
        f = smoothstep((float)(t - close_ms) / (float)open_ms);
    } else {
        g_blink_active = false;
        uint32_t h = (uint32_t)(now * 2654435761u);
        g_next_blink = now + 3400 + (h % 2800);
        f = 1.0f;
    }
    return clampf(f, 0.06f, 1.0f);
}

// ---------------------------------------------------------------------------
// anti-aliased scanline polygon fill
// ---------------------------------------------------------------------------
inline float value_cov(float c, int subdiv)
{
    return c / (float)subdiv;
}

inline uint16_t blend565(uint16_t dst, uint16_t src, float a)
{
    int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    int r = (int)(dr + (sr - dr) * a + 0.5f);
    int g = (int)(dg + (sg - dg) * a + 0.5f);
    int b = (int)(db + (sb - db) * a + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Fill a closed polygon: solid interior spans are written with a tight loop,
// only the two fractional end pixels need blending.
void fill_poly_aa(uint16_t* fb, const float* xs, const float* ys, int n, uint16_t color)
{
    float miny = ys[0], maxy = ys[0], minx = xs[0], maxx = xs[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_w - 1) x1 = g_w - 1;
    if (y1 > g_h - 1) y1 = g_h - 1;
    if (y1 < y0 || x1 < x0 || n > 1024) return;

    static float cross[1024];
    static uint16_t rowcache[1024];
    for (int y = y0; y <= y1; y++) {
        uint16_t* row = fb + (size_t)y * g_w;
        for (int sr = 0; sr < kSubRows; sr++) {
        const float sy_ = (float)y + ((float)sr + 0.5f) / (float)kSubRows;
        int cnt = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float ay = ys[j], by = ys[i];
            if ((ay <= sy_ && by > sy_) || (by <= sy_ && ay > sy_)) {
                float ax = xs[j], bx = xs[i];
                float x = ax + (sy_ - ay) * (bx - ax) / (by - ay);
                if (cnt < 1024) cross[cnt++] = x;
            }
        }
        if (cnt < 2) continue;
        for (int i = 1; i < cnt; i++) {
            float v = cross[i];
            int j = i - 1;
            while (j >= 0 && cross[j] > v) {
                cross[j + 1] = cross[j];
                j--;
            }
            cross[j + 1] = v;
        }
        uint16_t* row = fb + (size_t)y * g_w;
        for (int k = 0; k + 1 < cnt; k += 2) {
            float xa = cross[k], xb = cross[k + 1];
            if (xb <= xa) continue;
            int solid_s = (int)ceilf(xa);
            int solid_e = (int)floorf(xb) - 1;
            if (solid_s < x0) solid_s = x0;
            if (solid_e > x1) solid_e = x1;
            int pl = (int)floorf(xa);
            if (pl >= x0 && pl <= x1 && (float)pl < xa) {
                float cov = value_cov((float)(pl + 1) - xa, kSubRows);
                if (cov > 0.002f) row[pl] = blend565(row[pl], color, cov);
            }
            int pr = (int)floorf(xb);
            if (pr >= x0 && pr <= x1 && (float)(pr + 1) > xb && (float)pr >= xa) {
                float cov = value_cov(xb - (float)pr, kSubRows);
                if (cov > 0.002f) row[pr] = blend565(row[pr], color, cov);
            }
            if (solid_e >= solid_s) {
                uint16_t* dst = row + solid_s;
                int count = solid_e - solid_s + 1;
                int i = 0;
                uint32_t two = (uint32_t)color | ((uint32_t)color << 16);
                for (; i + 1 < count; i += 2) memcpy(dst + i, &two, 4);
                if (i < count) dst[i] = color;
            }
        }
        }
    }
}

// ---------------------------------------------------------------------------
// jelly physics
// ---------------------------------------------------------------------------
struct ExprShape {
    float c2, s2;  // k=2
    float c3, s3;  // k=3
    float c4;      // k=4
};

/* Per-expression jelly targets (small values; ball radius is 80 in design space). */
ExprShape shape_for_expr(int expr)
{
    switch (expr) {
    case 1: return {-0.055f, 0.000f, 0.010f, 0.000f, 0.006f};  // HAPPY: bouncy squash
    case 2: return {0.035f, 0.000f, -0.006f, 0.000f, 0.000f};  // SAD: droopy stretch
    case 3: return {-0.030f, 0.015f, 0.012f, 0.000f, 0.000f};  // ANGRY: tense
    case 4: return {0.010f, 0.020f, 0.000f, 0.018f, 0.000f};   // DOUBT: tilted
    case 5: return {0.045f, 0.000f, 0.000f, 0.000f, 0.004f};   // SLEEPY: saggy
    case 6: return {-0.020f, 0.000f, 0.000f, 0.014f, 0.000f};  // LISTENING: alert stretch
    case 7: return {0.000f, 0.030f, 0.000f, 0.000f, 0.000f};   // THINKING: slow tilt
    case 8: return {-0.060f, 0.000f, 0.014f, 0.000f, 0.008f};  // EXCITED
    case 9: return {0.005f, 0.025f, 0.000f, 0.012f, 0.000f};   // CURIOUS
    case 10: return {0.000f, 0.000f, 0.028f, 0.000f, 0.000f};  // CONFUSED
    case 11: return {0.040f, 0.000f, 0.000f, 0.010f, 0.010f};  // SURPRISED
    case 12: return {0.000f, 0.000f, 0.030f, 0.020f, 0.015f};  // DIZZY
    case 13: return {0.030f, 0.000f, 0.000f, 0.008f, 0.000f};  // AFFECTION
    case 14: return {0.020f, 0.000f, 0.000f, 0.000f, 0.000f};  // BORED
    case 0:
    default: return {0.000f, 0.000f, 0.000f, 0.000f, 0.000f};  // NEUTRAL
    }
}

// spring constants come from the runtime tuning (see set_tuning)

inline void spring_step(float& x, float& v, float target, float dt)
{
    float accel = g_omega * g_omega * (target - x) - 2.0f * g_zeta * g_omega * v;
    v += accel * dt;
    x += v * dt;
}

void jelly_update(uint32_t now_ms)
{
    float dt = (float)(now_ms - g_last_ms) * 0.001f;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.033f;

    ExprShape target = shape_for_expr(g_expr);
    target.c2 *= g_amp_scale;
    target.s2 *= g_amp_scale;
    target.c3 *= g_amp_scale;
    target.s3 *= g_amp_scale;
    target.c4 *= g_amp_scale;

    // idle micro-jiggle keeps the surface alive
    g_phase = (float)now_ms * 0.001f;
    float idle2 = g_idle_scale * (0.0045f * sinf(g_phase * 3.1f) + 0.0025f * sinf(g_phase * 5.3f + 1.1f));
    float idle3 = g_idle_scale * 0.0030f * sinf(g_phase * 2.3f + 0.7f);
    target.c2 += idle2;
    target.s2 += 0.6f * idle2;
    target.c3 += idle3;

    spring_step(g_jelly.c2.x, g_jelly.c2.v, target.c2, dt);
    spring_step(g_jelly.s2.x, g_jelly.s2.v, target.s2, dt);
    spring_step(g_jelly.c3.x, g_jelly.c3.v, target.c3, dt);
    spring_step(g_jelly.s3.x, g_jelly.s3.v, target.s3, dt);
    spring_step(g_jelly.c4.x, g_jelly.c4.v, target.c4, dt);

    // happy/excited hop + soft bounce
    float bt = 0.0f;
    if (g_expr == 1 || g_expr == 8) {
        bt = -fabsf(sinf(g_phase * 3.6f)) * 2.2f;
    }
    spring_step(g_jelly.bounce, g_jelly.bounce_v, bt, dt);
}

void jelly_impulse(float strength)
{
    float s = strength * (0.6f + 0.4f * g_amp_scale);
    g_jelly.c2.v -= s;
    g_jelly.s2.v += 0.5f * s;
    g_jelly.c3.v += 0.7f * s;
    g_jelly.bounce_v -= 0.6f * s;
}

// ---------------------------------------------------------------------------
// pose computation
// ---------------------------------------------------------------------------
void compute_target(uint32_t now, float outL[24][2], float outR[24][2])
{
    const ExprData& e = kExprs[g_expr];
    int ridx = pick_ring(e, now);
    const RingData& ring = kRings[ridx];
    const RingPlace& pl = kPlaces[ridx];

    float axl = 0, ayl = 0, axr = 0, ayr = 0;
    for (int i = 0; i < e.anim_count && i < 2; i++) {
        const AnimData& a = e.anims[i];
        float v = wave_value(a, now) * (float)a.amp;
        if (a.axis == AXIS_X) {
            if (a.target == TARGET_BOTH || a.target == TARGET_LEFT) axl += v;
            if (a.target == TARGET_BOTH || a.target == TARGET_RIGHT) axr += v;
        } else {
            if (a.target == TARGET_BOTH || a.target == TARGET_LEFT) ayl += v;
            if (a.target == TARGET_BOTH || a.target == TARGET_RIGHT) ayr += v;
        }
    }

    float gx = g_gaze_h * 16.8f;
    float gy = g_gaze_v * 10.5f;

    float blx = 160.0f + (float)e.dxl + axl + gx;
    float bly = 120.0f + (float)e.dyl + ayl + gy;
    float brx = 160.0f + (float)e.dxr + axr + gx;
    float bry = 120.0f + (float)e.dyr + ayr + gy;

    float open = ((float)e.openness / 100.0f) * blink_factor(now);
    if (open < 0.045f) open = 0.045f;

    for (int i = 0; i < 24; i++) {
        outL[i][0] = blx + (float)pl.lx + (float)ring.x[i];
        outL[i][1] = bly + (float)pl.ly + (float)ring.y[i] * open;
        outR[i][0] = brx + (float)pl.rx + (float)ring.x[i];
        outR[i][1] = bry + (float)pl.ry + (float)ring.y[i] * open;
    }
}

// Chaikin corner-cutting with a runtime iteration count:
// 1 -> 48 points, 2 -> 96 points (default), 3 -> 192 points.
// It never overshoots, so the smoothed outline stays clean.
void smooth_closed(const float in[24][2], float out[192][2], int iters, int* out_n)
{
    static float buf_a[192][2];
    static float buf_b[192][2];
    int n = 24;
    for (int i = 0; i < 24; i++) {
        buf_a[i][0] = in[i][0];
        buf_a[i][1] = in[i][1];
    }
    float (*cur)[2] = buf_a;
    float (*nxt)[2] = buf_b;
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < n; i++) {
            const float* p = cur[i];
            const float* q = cur[(i + 1) % n];
            nxt[2 * i][0] = 0.75f * p[0] + 0.25f * q[0];
            nxt[2 * i][1] = 0.75f * p[1] + 0.25f * q[1];
            nxt[2 * i + 1][0] = 0.25f * p[0] + 0.75f * q[0];
            nxt[2 * i + 1][1] = 0.25f * p[1] + 0.75f * q[1];
        }
        n *= 2;
        float (*tmp)[2] = cur;
        cur = nxt;
        nxt = tmp;
    }
    for (int i = 0; i < n; i++) {
        out[i][0] = cur[i][0];
        out[i][1] = cur[i][1];
    }
    *out_n = n;
}

// Blend an outline toward its best-fit ellipse (roundness warp).
void round_outline(float pts[][2], int n, float r)
{
    if (r <= 0.001f || n < 3) return;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; i++) {
        cx += pts[i][0];
        cy += pts[i][1];
    }
    cx /= (float)n;
    cy /= (float)n;
    float cxx = 0, cyy = 0, cxy = 0;
    for (int i = 0; i < n; i++) {
        float dx = pts[i][0] - cx, dy = pts[i][1] - cy;
        cxx += dx * dx;
        cyy += dy * dy;
        cxy += dx * dy;
    }
    cxx /= (float)n;
    cyy /= (float)n;
    cxy /= (float)n;
    float tr = cxx + cyy;
    float det = cxx * cyy - cxy * cxy;
    float disc = sqrtf(fmaxf(0.0f, tr * tr * 0.25f - det));
    float e1 = tr * 0.5f + disc;
    float e2 = tr * 0.5f - disc;
    if (e1 < 1e-4f) return;
    float a = sqrtf(e1);
    float b = sqrtf(fmaxf(e2, 1e-4f));
    float phi = 0.5f * atan2f(2.0f * cxy, cxx - cyy);
    float ca = cosf(phi), sa = sinf(phi);
    for (int i = 0; i < n; i++) {
        float dx = pts[i][0] - cx, dy = pts[i][1] - cy;
        float u = dx * ca + dy * sa;
        float v = -dx * sa + dy * ca;
        float alpha = atan2f(v / b, u / a);
        float eu = a * cosf(alpha);
        float ev = b * sinf(alpha);
        float ex = cx + eu * ca - ev * sa;
        float ey = cy + eu * sa + ev * ca;
        pts[i][0] = (1.0f - r) * pts[i][0] + r * ex;
        pts[i][1] = (1.0f - r) * pts[i][1] + r * ey;
    }
}

}  // namespace

static int g_agent_color_idx = 0;

int agent_color_index() { return g_agent_color_idx; }

/* Advance locally (used by the B key so the colour changes instantly). */
int cycle_agent_color()
{
    set_agent_color((g_agent_color_idx + 1) % 3);
    return g_agent_color_idx;
}

void set_agent_color(int idx)
{
    if (idx < 0 || idx > 2) idx = 0;
    g_agent_color_idx = idx;
    switch (idx) {
    case 1: g_ball_color = 0x04BF; break;   /* blue = Codex */
    case 2: g_ball_color = 0xF800; break;   /* red  = DSH   */
    default: g_ball_color = 0xFFFF; break;  /* white = Hermes */
    }
}

void init(int width, int height, float ball_radius_px)
{
    g_w = width;
    g_h = height;
    g_cx = (float)width * 0.5f;
    g_cy = (float)height * 0.5f;
    g_cs = ball_radius_px / 80.0f;
    g_have_cur = false;
    g_blink_active = false;
    g_next_blink = 0;
    g_expr = 0;
    g_gaze_h = g_gaze_v = 0.0f;
    memset(&g_jelly, 0, sizeof(g_jelly));
    g_phase = 0.0f;
}

void set_expression(int expr, uint32_t now_ms)
{
    if (expr < 0) expr = 0;
    if (expr >= kExprCount) expr = kExprCount - 1;
    if (expr == g_expr) return;
    g_expr = expr;
    g_expr_at = now_ms;
    jelly_impulse(0.9f);
}

int expression() { return g_expr; }
int expression_count() { return kExprCount; }

const char* expression_name(int expr)
{
    if (expr < 0) expr = 0;
    if (expr >= kExprCount) expr = kExprCount - 1;
    return kExprs[expr].name;
}

void set_gaze(float h, float v)
{
    g_gaze_h = clampf(h, -1.0f, 1.0f);
    g_gaze_v = clampf(v, -1.0f, 1.0f);
}

void request_blink(uint32_t now_ms)
{
    g_blink_active = true;
    g_blink_start = now_ms;
    jelly_impulse(0.25f);
}

void poke(uint32_t now_ms)
{
    (void)now_ms;
    jelly_impulse(1.6f);
}

void set_tuning(float omega, float zeta, float amp_scale, float idle_scale)
{
    if (omega > 0.1f) g_omega = omega;
    if (zeta > 0.0f) g_zeta = zeta;
    if (amp_scale > 0.0f) g_amp_scale = amp_scale;
    if (idle_scale >= 0.0f) g_idle_scale = idle_scale;
}

void set_eye_roundness(float r)
{
    if (r < 0.0f) r = 0.0f;
    if (r > 0.6f) r = 0.6f;
    g_eye_round = r;
}

void set_eye_smoothing(int iterations)
{
    if (iterations < 1) iterations = 1;
    if (iterations > 3) iterations = 3;
    g_eye_iters = iterations;
    g_eye_npts = 24 << iterations;
}

void render(uint16_t* fb, uint32_t now_ms)
{
    if (g_last_ms == 0) {
        g_last_ms = now_ms;
        g_have_cur = false;
    }

    // target pose (design space)
    float tL[24][2], tR[24][2];
    compute_target(now_ms, tL, tR);

    if (!g_have_cur) {
        memcpy(g_curL, tL, sizeof(g_curL));
        memcpy(g_curR, tR, sizeof(g_curR));
        g_have_cur = true;
    } else {
        uint32_t dt = now_ms - g_last_ms;
        if (dt > 200) dt = 200;
        float a = 1.0f - expf(-(float)dt / 30.0f);
        if (a > 0.85f) a = 0.85f;
        for (int i = 0; i < 24; i++) {
            g_curL[i][0] += (tL[i][0] - g_curL[i][0]) * a;
            g_curL[i][1] += (tL[i][1] - g_curL[i][1]) * a;
            g_curR[i][0] += (tR[i][0] - g_curR[i][0]) * a;
            g_curR[i][1] += (tR[i][1] - g_curR[i][1]) * a;
        }
    }

    jelly_update(now_ms);
    g_last_ms = now_ms;

    // clear only the region the ball can occupy (much cheaper than the full frame)
    {
        int cx = (int)g_cx, cy = (int)g_cy;
        int rad = (int)(80.0f * g_cs * 1.12f) + 8;
        int bx0 = cx - rad, bx1 = cx + rad;
        int by0 = cy - rad, by1 = cy + rad;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > g_w - 1) bx1 = g_w - 1;
        if (by1 > g_h - 1) by1 = g_h - 1;
        if (bx1 >= bx0 && by1 >= by0) {
            size_t row_bytes = (size_t)(bx1 - bx0 + 1) * sizeof(uint16_t);
            for (int y = by0; y <= by1; y++) {
                memset(fb + (size_t)y * g_w + bx0, 0, row_bytes);
            }
        }
    }

    // ---- deformed ball ----
    const float R = 80.0f * g_cs * (1.0f + 0.008f * sinf(g_phase * 1.6f));
    const float jc2 = g_jelly.c2.x, js2 = g_jelly.s2.x;
    const float jc3 = g_jelly.c3.x, js3 = g_jelly.s3.x;
    const float jc4 = g_jelly.c4.x;
    const float cy_j = g_cy + g_jelly.bounce * g_cs;

    static float bx[kBallSegs], by[kBallSegs];
    for (int i = 0; i < kBallSegs; i++) {
        float th = 6.2831853f * (float)i / (float)kBallSegs;
        float c = cosf(th), s = sinf(th);
        float c2 = 2.0f * c * c - 1.0f, s2 = 2.0f * s * c;
        float c3 = 4.0f * c * c * c - 3.0f * c;
        float s3 = 3.0f * s - 4.0f * s * s * s;
        float c4 = 8.0f * c * c * c * c - 8.0f * c * c + 1.0f;
        float rad = R * (1.0f + jc2 * c2 + js2 * s2 + jc3 * c3 + js3 * s3 + jc4 * c4);
        bx[i] = g_cx + rad * c;
        by[i] = cy_j + rad * s;
    }
    fill_poly_aa(fb, bx, by, kBallSegs, g_ball_color);

    // ---- eyes (smoothed + squashed with the ball) ----
    static float smL[192][2], smR[192][2];
    static float exL[192], eyL[192], exR[192], eyR[192];
    int eye_n = 0;
    smooth_closed(g_curL, smL, g_eye_iters, &eye_n);
    smooth_closed(g_curR, smR, g_eye_iters, &eye_n);
    if (g_eye_round > 0.001f) {
        round_outline(smL, eye_n, g_eye_round);
        round_outline(smR, eye_n, g_eye_round);
    }

    const float sqx = 1.0f + jc2;
    const float sqy = 1.0f - jc2;
    const float eye_dy = g_jelly.bounce * 0.8f;

    for (int i = 0; i < eye_n; i++) {
        float lx = 160.0f + (smL[i][0] - 160.0f) * sqx;
        float ly = 120.0f + (smL[i][1] - 120.0f) * sqy + eye_dy;
        exL[i] = sx(lx);
        eyL[i] = sy(ly);
        float rx = 160.0f + (smR[i][0] - 160.0f) * sqx;
        float ry = 120.0f + (smR[i][1] - 120.0f) * sqy + eye_dy;
        exR[i] = sx(rx);
        eyR[i] = sy(ry);
    }
    fill_poly_aa(fb, exL, eyL, eye_n, kColorEye);
    fill_poly_aa(fb, exR, eyR, eye_n, kColorEye);
}

}  // namespace qiuqiu
