#pragma once

#include <cstdint>

namespace dr {

// The capture and output clocks use the same QPC timebase. Frame 0 is the
// first complete pair; subsequent samples are evenly spaced from that origin.
class RecordingTimeline {
public:
    RecordingTimeline(int64_t first, int64_t frequency, uint32_t fps)
        : first_(first), frequency_(frequency), fps_(fps) {}

    int64_t sample_time(uint64_t index) const {
        return first_ + static_cast<int64_t>(index / fps_) * frequency_ +
               static_cast<int64_t>(index % fps_) * frequency_ / fps_;
    }

    uint64_t frame_count(int64_t stop) const {
        const int64_t elapsed = stop > first_ ? stop - first_ : 0;
        const auto count = static_cast<uint64_t>(elapsed / frequency_) * fps_ +
                           (elapsed % frequency_ * fps_ + frequency_ / 2) / frequency_;
        return count ? count : 1;
    }

private:
    int64_t first_;
    int64_t frequency_;
    uint32_t fps_;
};

} // namespace dr