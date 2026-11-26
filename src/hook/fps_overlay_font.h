#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace dr {

// Renders a compact 5x7 bitmap string (e.g. "144 FPS") into an RGBA8 buffer
// (Green when idle, Red when recording) with a dark translucent background.
inline void render_fps_bitmap(const char* text, bool recording, std::vector<uint8_t>& out_rgba, uint32_t& out_w, uint32_t& out_h) {
    struct Glyph { char ch; uint8_t rows[7]; };
    static constexpr Glyph kFont[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
        {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
        {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
        {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    };

    const uint32_t len = static_cast<uint32_t>(std::strlen(text));
    const uint32_t pad = 3;
    const uint32_t scale = 2;
    const uint32_t cw = 6 * scale;
    out_w = len * cw + pad * 2;
    out_h = 7 * scale + pad * 2;
    out_rgba.assign(out_w * out_h * 4, 0);

    // Dark translucent background
    for (uint32_t i = 0; i < out_w * out_h; ++i) {
        out_rgba[i * 4 + 0] = 12;
        out_rgba[i * 4 + 1] = 12;
        out_rgba[i * 4 + 2] = 14;
        out_rgba[i * 4 + 3] = 185;
    }

    const uint8_t fg_r = recording ? 255 : 30;
    const uint8_t fg_g = recording ? 59 : 230;
    const uint8_t fg_b = recording ? 48 : 118;

    for (uint32_t ci = 0; ci < len; ++ci) {
        const uint8_t* rows = kFont[14].rows; // space fallback
        for (const auto& g : kFont) {
            if (g.ch == text[ci]) { rows = g.rows; break; }
        }
        for (uint32_t ry = 0; ry < 7; ++ry) {
            for (uint32_t rx = 0; rx < 5; ++rx) {
                if ((rows[ry] >> (4 - rx)) & 1) {
                    for (uint32_t sy = 0; sy < scale; ++sy) {
                        for (uint32_t sx = 0; sx < scale; ++sx) {
                            const uint32_t px = pad + ci * cw + rx * scale + sx;
                            const uint32_t py = pad + ry * scale + sy;
                            const size_t idx = (static_cast<size_t>(py) * out_w + px) * 4;
                            out_rgba[idx + 0] = fg_r;
                            out_rgba[idx + 1] = fg_g;
                            out_rgba[idx + 2] = fg_b;
                            out_rgba[idx + 3] = 255;
                        }
                    }
                }
            }
        }
    }
}

} // namespace dr

