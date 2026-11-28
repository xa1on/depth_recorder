#pragma once
#include "hook_common.h"
#include <cstdint>

namespace dr {

struct CaptureSlotMeta {
    bool pending = false;
    bool has_color = false;
    bool has_depth = false;
    bool preview_only = false;
    bool color_is_rgba = false;
    uint32_t mode = 0;
    uint32_t depth_bpp = 4;
    int64_t qpc = 0;
    LONG generation = -1;
};

template <uint32_t kSlots>
struct CaptureSlotTracker {
    CaptureSlotMeta slots[kSlots]{};

    void reset_all() {
        for (auto& s : slots) s.pending = false;
    }

    uint32_t acquire_free_slot() const {
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (!slots[i].pending) return i;
        }
        return kSlots;
    }

    uint32_t pick_oldest_pending(LONG generation) {
        uint32_t best = kSlots;
        for (uint32_t i = 0; i < kSlots; ++i) {
            if (!slots[i].pending) continue;
            if (slots[i].generation != generation) {
                slots[i].pending = false;
            } else if (best == kSlots || slots[i].qpc < slots[best].qpc) {
                best = i;
            }
        }
        return best;
    }

    void mark_queued(uint32_t slot, int64_t qpc, LONG generation, bool preview_only,
                     uint32_t mode, bool has_color, bool has_depth, uint32_t depth_bpp, bool color_is_rgba) {
        slots[slot] = {true, has_color, has_depth, preview_only, color_is_rgba, mode, depth_bpp, qpc, generation};
    }

    // Publishes a completed slot directly into IpcRing. Only clears pending if ring push succeeds or slot is invalid.
    template <typename CopyColorFn, typename CopyDepthFn>
    bool try_publish_slot(uint32_t slot, IpcRing& ring, SharedConfig* cfg, uint64_t frame_idx,
                          int64_t qpc_freq, uint32_t w, uint32_t h,
                          CopyColorFn&& copy_color, CopyDepthFn&& copy_depth) {
        auto& s = slots[slot];
        if (!s.pending) return false;

        if (!s.preview_only && (!s.has_color || (cfg && cfg->depth_enabled && !s.has_depth))) {
            s.pending = false;
            return false;
        }

        const uint64_t color_bytes_u64 = s.has_color ? static_cast<uint64_t>(w) * h * 4 : 0;
        const uint64_t depth_bytes_u64 = s.has_depth ? static_cast<uint64_t>(w) * h * s.depth_bpp : 0;
        if (color_bytes_u64 + depth_bytes_u64 + sizeof(FrameHeader) > kMaxSlotPayloadBytes) {
            s.pending = false;
            return false;
        }

        const uint32_t color_bytes = static_cast<uint32_t>(color_bytes_u64);
        const uint32_t depth_bytes = static_cast<uint32_t>(depth_bytes_u64);
        uint8_t* dst = ring.begin_push(color_bytes + depth_bytes);
        if (!dst) return false;

        if (s.has_color && !copy_color(dst)) return false;
        if (s.has_depth && !copy_depth(dst + color_bytes)) return false;

        FrameHeader hdr{};
        hdr.frame_index = frame_idx;
        hdr.qpc_timestamp = s.qpc;
        hdr.qpc_freq = qpc_freq;
        hdr.width = w;
        hdr.height = h;
        hdr.color_bytes = color_bytes;
        hdr.depth_bytes = depth_bytes;
        hdr.depth_mode = s.mode;
        hdr.has_depth = s.has_depth ? 1 : 0;
        hdr.color_is_rgba = s.color_is_rgba ? 1 : 0;
        hdr.is_preview_only = s.preview_only ? 1 : 0;
        hdr.capture_generation = s.generation;
        hdr.producer_pid = GetCurrentProcessId();
        publish_frame(ring, cfg, hdr);
        s.pending = false;
        return true;
    }
};

} // namespace dr
