#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dr {

// Real-time CFR pacer using monotonic QPC timestamps:
// - Skips excess frames when game FPS > target_fps (returns 0).
// - Repeats frames when game FPS < target_fps or frames drop (returns > 1) so video stays 1x real-time.
// - Tracks actual unique frames encoded per second for live telemetry.
class QpcFramePacer {
public:
    void reset(uint32_t target_fps) {
        target_fps_ = std::max<uint32_t>(1, target_fps);
        start_qpc_ = 0;
        encoded_count_ = 0;
        win_start_qpc_ = 0;
        win_unique_frames_ = 0;
        actual_rec_fps_ = 0.0;
    }

    uint32_t compute_writes_for_frame(int64_t qpc_ts, int64_t qpc_freq) {
        if (qpc_freq <= 0) return 1;
        if (start_qpc_ == 0) {
            start_qpc_ = qpc_ts;
            win_start_qpc_ = qpc_ts;
            encoded_count_ = 1;
            win_unique_frames_ = 1;
            actual_rec_fps_ = static_cast<double>(target_fps_);
            return 1;
        }

        const double elapsed = static_cast<double>(qpc_ts - start_qpc_) / static_cast<double>(qpc_freq);
        const uint64_t expected_idx = (elapsed > 0.0)
            ? static_cast<uint64_t>(std::floor(elapsed * static_cast<double>(target_fps_)))
            : 0ULL;

        if (encoded_count_ > expected_idx) {
            return 0; // Game is ahead of target recording FPS; skip
        }

        const uint64_t needed = (expected_idx - encoded_count_) + 1ULL;
        const uint32_t writes = static_cast<uint32_t>(std::min<uint64_t>(needed, 30ULL));
        encoded_count_ += writes;

        win_unique_frames_++;
        const double win_dt = static_cast<double>(qpc_ts - win_start_qpc_) / static_cast<double>(qpc_freq);
        if (win_dt >= 0.5) {
            actual_rec_fps_ = static_cast<double>(win_unique_frames_) / win_dt;
            win_start_qpc_ = qpc_ts;
            win_unique_frames_ = 0;
        }
        return writes;
    }

    double actual_rec_fps() const { return actual_rec_fps_; }

private:
    uint32_t target_fps_ = 60;
    int64_t start_qpc_ = 0;
    uint64_t encoded_count_ = 0;
    int64_t win_start_qpc_ = 0;
    uint64_t win_unique_frames_ = 0;
    double actual_rec_fps_ = 0.0;
};

} // namespace dr
