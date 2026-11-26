#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dr {

struct DepthCandidate {
    uintptr_t key = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t draw_calls = 0;
    uint32_t clears = 0;
    uint64_t last_frame = 0;
};

class DepthHeuristic {
public:
    void begin_frame(uint64_t frame_idx) {
        current_frame_ = frame_idx;
        for (auto& c : candidates_) {
            c.draw_calls = 0;
            c.clears = 0;
        }
    }

    void record_draw(uintptr_t key, uint32_t w, uint32_t h, uint32_t swap_w, uint32_t swap_h) {
        if (!key || !is_plausible_size(w, h, swap_w, swap_h)) return;
        auto& c = get_or_add(key, w, h);
        c.draw_calls++;
        c.last_frame = current_frame_;
    }

    void record_clear(uintptr_t key, uint32_t w, uint32_t h, uint32_t swap_w, uint32_t swap_h) {
        if (!key || !is_plausible_size(w, h, swap_w, swap_h)) return;
        auto& c = get_or_add(key, w, h);
        c.clears++;
        c.last_frame = current_frame_;
    }

    // Prune stale candidates not seen for > 120 frames
    void end_frame_prune() {
        candidates_.erase(
            std::remove_if(candidates_.begin(), candidates_.end(), [&](const DepthCandidate& c) {
                return (current_frame_ > c.last_frame + 120);
            }),
            candidates_.end());
    }

    std::vector<DepthCandidate> ranked_candidates() const {
        std::vector<DepthCandidate> out;
        for (const auto& c : candidates_) {
            if (c.last_frame == current_frame_ && c.draw_calls > 0) {
                out.push_back(c);
            }
        }
        std::sort(out.begin(), out.end(), [](const DepthCandidate& a, const DepthCandidate& b) {
            if (a.draw_calls != b.draw_calls) return a.draw_calls > b.draw_calls;
            return (a.width * a.height) > (b.width * b.height);
        });
        return out;
    }

    static bool is_plausible_size(uint32_t w, uint32_t h, uint32_t swap_w, uint32_t swap_h) {
        if (w < 64 || h < 64 || swap_w == 0 || swap_h == 0) return false;
        if (w < swap_w / 2 || h < swap_h / 2) return false;
        const float aspect_d = static_cast<float>(w) / static_cast<float>(h);
        const float aspect_s = static_cast<float>(swap_w) / static_cast<float>(swap_h);
        return std::fabs(aspect_d - aspect_s) <= aspect_s * 0.15f;
    }

private:
    DepthCandidate& get_or_add(uintptr_t key, uint32_t w, uint32_t h) {
        for (auto& c : candidates_) {
            if (c.key == key) {
                c.width = w;
                c.height = h;
                return c;
            }
        }
        candidates_.push_back({key, w, h, 0, 0, current_frame_});
        return candidates_.back();
    }

    uint64_t current_frame_ = 0;
    std::vector<DepthCandidate> candidates_;
};

} // namespace dr

