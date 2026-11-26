#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dr {

inline constexpr float kTwoPi = 6.2831853071795864769f;
inline constexpr float kPhaseCycles = 8.0f;

inline float saturate(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline uint8_t to_u8(float v) {
    return static_cast<uint8_t>(std::lround(saturate(v) * 255.0f));
}

// Converts hardware depth [0, 1] to normalized linear depth [0, 1]
inline float linearize_depth(float d, float z_near, float z_far, bool reversed_z) {
    d = saturate(d);
    if (reversed_z) d = 1.0f - d;
    const float denom = z_far - d * (z_far - z_near);
    if (denom <= 1e-6f) return 1.0f;
    const float z_view = (z_near * z_far) / denom;
    return saturate((z_view - z_near) / std::max(z_far - z_near, 1e-6f));
}

// Mode 2: Continuous phase encoding resilient to lossy YUV 4:2:0 H.264 compression.
// R = coarse depth, G/B = quadrature cosine/sine waves at kPhaseCycles.
inline void encode_color_phase(float d, uint8_t& r, uint8_t& g, uint8_t& b) {
    d = saturate(d);
    const float theta = kTwoPi * kPhaseCycles * d;
    r = to_u8(d);
    g = to_u8(0.5f + 0.5f * std::cos(theta));
    b = to_u8(0.5f + 0.5f * std::sin(theta));
}

inline float decode_color_phase(uint8_t r, uint8_t g, uint8_t b) {
    const float d_coarse = static_cast<float>(r) / 255.0f;
    const float cx = (static_cast<float>(g) / 255.0f) * 2.0f - 1.0f;
    const float sy = (static_cast<float>(b) / 255.0f) * 2.0f - 1.0f;
    float phi = std::atan2(sy, cx); // [-pi, pi]
    if (phi < 0.0f) phi += kTwoPi;
    const float frac = phi / kTwoPi; // [0, 1)
    const float k = std::round(kPhaseCycles * d_coarse - frac);
    return saturate((k + frac) / kPhaseCycles);
}

// Mode 3: Exact 24-bit integer packing (all 16,777,216 depth levels).
inline void encode_rgb24(float d, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint32_t u24 = static_cast<uint32_t>(std::clamp(std::llround(static_cast<double>(saturate(d)) * 16777215.0), 0LL, 16777215LL));
    r = static_cast<uint8_t>((u24 >> 16) & 0xFF);
    g = static_cast<uint8_t>((u24 >> 8) & 0xFF);
    b = static_cast<uint8_t>(u24 & 0xFF);
}

inline float decode_rgb24(uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t u24 = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    return static_cast<float>(static_cast<double>(u24) / 16777215.0);
}

} // namespace dr

