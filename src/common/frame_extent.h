#pragma once

#include <algorithm>
#include <cstdint>

namespace dr {

struct FitRect {
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
};

inline FitRect fit_frame(uint32_t source_width, uint32_t source_height, uint32_t canvas_width, uint32_t canvas_height) {
    const uint64_t lhs = static_cast<uint64_t>(canvas_width) * source_height;
    const uint64_t rhs = static_cast<uint64_t>(canvas_height) * source_width;
    const auto width = lhs <= rhs ? canvas_width : std::max(1u, static_cast<uint32_t>(rhs / source_height));
    const auto height = lhs >= rhs ? canvas_height : std::max(1u, static_cast<uint32_t>(lhs / source_width));
    return {(canvas_width - width) / 2, (canvas_height - height) / 2, width, height};
}

} // namespace dr
