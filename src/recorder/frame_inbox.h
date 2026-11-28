#pragma once
#include "common/frame_extent.h"
#include "common/ipc_ring.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace dr {

struct FramePacket {
    FrameHeader header{};
    std::vector<uint8_t> color;
    std::vector<uint8_t> depth;
};

inline bool same_capture_format(const FrameHeader& left, const FrameHeader& right) {
    return left.capture_generation == right.capture_generation && left.width == right.width &&
           left.height == right.height && left.color_is_rgba == right.color_is_rgba &&
           left.depth_mode == right.depth_mode;
}

class FrameFitter {
public:
    // The canvas is fixed for the clip. Only the source extent can change the sampling map and padding.
    explicit FrameFitter(const FrameHeader& canvas)
        : canvas_(canvas),
          depth_bpp_(canvas.width && canvas.height ? canvas.depth_bytes / (canvas.width * canvas.height) : 0) {}

    void fit(const FramePacket& source) {
        const auto& input = source.header;
        if (input.width != source_width_ || input.height != source_height_) {
            source_width_ = input.width;
            source_height_ = input.height;
            rect_ = fit_frame(input.width, input.height, canvas_.width, canvas_.height);
            output_.color.assign(canvas_.color_bytes, 0);
            output_.depth.assign(canvas_.depth_bytes, 0);
            columns_.resize(rect_.width);
            rows_.resize(rect_.height);
            for (uint32_t x = 0; x < rect_.width; ++x) {
                columns_[x] = static_cast<uint32_t>((static_cast<uint64_t>(x) * input.width + input.width / 2) / rect_.width);
            }
            for (uint32_t y = 0; y < rect_.height; ++y) {
                rows_[y] = static_cast<size_t>((static_cast<uint64_t>(y) * input.height + input.height / 2) /
                                               rect_.height) * input.width;
            }
        }
        output_.header = canvas_;
        output_.header.qpc_timestamp = input.qpc_timestamp;
        const bool swizzle = input.color_is_rgba != canvas_.color_is_rgba;
        for (uint32_t y = 0; y < rect_.height; ++y) {
            const size_t row = rows_[y];
            const size_t target_row = static_cast<size_t>(rect_.top + y) * canvas_.width + rect_.left;
            if (rect_.width == input.width && !swizzle) {
                std::memcpy(output_.color.data() + target_row * 4, source.color.data() + row * 4, rect_.width * 4);
                if (depth_bpp_) {
                    std::memcpy(output_.depth.data() + target_row * depth_bpp_, source.depth.data() + row * depth_bpp_,
                                rect_.width * depth_bpp_);
                }
                continue;
            }
            for (uint32_t x = 0; x < rect_.width; ++x) {
                const size_t from = row + columns_[x];
                const size_t to = target_row + x;
                const auto* color = source.color.data() + from * 4;
                auto* dest = output_.color.data() + to * 4;
                if (swizzle) {
                    dest[0] = color[2];
                    dest[1] = color[1];
                    dest[2] = color[0];
                    dest[3] = color[3];
                } else {
                    std::memcpy(dest, color, 4);
                }
                if (depth_bpp_ == 1) {
                    output_.depth[to] = source.depth[from];
                } else if (depth_bpp_ == 4) {
                    std::memcpy(output_.depth.data() + to * 4, source.depth.data() + from * 4, 4);
                }
            }
        }
    }

    const FramePacket& for_frame(const FramePacket& frame) const {
        return output_.header.qpc_timestamp == frame.header.qpc_timestamp ? output_ : frame;
    }

private:
    FrameHeader canvas_;
    uint32_t depth_bpp_;
    uint32_t source_width_ = 0;
    uint32_t source_height_ = 0;
    FitRect rect_{};
    std::vector<uint32_t> columns_;
    std::vector<size_t> rows_;
    FramePacket output_;
};

class FrameInbox {
public:
    std::shared_ptr<FramePacket> acquire(const FrameHeader& header) {
        std::lock_guard lock(mutex_);
        if (!frames_.empty() && frames_.front()->header.capture_generation != header.capture_generation) {
            frames_.clear();
            queued_bytes_ = 0;
        }
        const size_t bytes = payload_size(header);
        // Keep the oldest queued frames when the encoder falls behind. Dropping
        // them would leave the writer trying to fill ticks older than every pair.
        if (frames_.size() >= 32 || queued_bytes_ + bytes > 384u * 1024u * 1024u) return {};
        for (auto& frame : pool_) {
            if (frame.use_count() == 1) return frame;
        }
        auto frame = std::make_shared<FramePacket>();
        pool_.push_back(frame);
        return frame;
    }

    void push(std::shared_ptr<FramePacket> frame) {
        std::lock_guard lock(mutex_);
        queued_bytes_ += payload_size(frame->header);
        const auto position = std::upper_bound(frames_.begin(), frames_.end(), frame->header.qpc_timestamp,
            [](int64_t timestamp, const auto& packet) { return timestamp < packet->header.qpc_timestamp; });
        frames_.insert(position, std::move(frame));
    }

    std::shared_ptr<const FramePacket> take(LONG generation, int64_t since, int64_t through, bool keep_latest_late = false) {
        std::lock_guard lock(mutex_);
        std::shared_ptr<const FramePacket> late;
        while (!frames_.empty()) {
            const auto& header = frames_.front()->header;
            if (header.capture_generation != generation || header.qpc_timestamp < since) {
                if (keep_latest_late && header.capture_generation == generation && header.qpc_timestamp <= through) {
                    late = frames_.front();
                }
                pop();
                continue;
            }
            if (header.qpc_timestamp > through) break;
            auto result = frames_.front();
            pop();
            return result;
        }
        return late;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        frames_.clear();
        pool_.clear();
        queued_bytes_ = 0;
    }

private:
    static size_t payload_size(const FrameHeader& header) {
        return static_cast<size_t>(header.color_bytes) + header.depth_bytes;
    }

    void pop() {
        queued_bytes_ -= payload_size(frames_.front()->header);
        frames_.pop_front();
    }

    std::mutex mutex_;
    size_t queued_bytes_ = 0;
    std::deque<std::shared_ptr<FramePacket>> frames_;
    std::vector<std::shared_ptr<FramePacket>> pool_;
};

} // namespace dr
