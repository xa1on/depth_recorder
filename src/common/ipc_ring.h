#pragma once
#include "config.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <windows.h>

namespace dr {

// Supports up to 4K frames: 3840*2160 * 4 (color) + 3840*2160 * 4 (depth) ~= 66 MB per slot.
// 3 slots = 200 MB virtual shared mapping (Windows commits pages lazily on touch).
inline constexpr uint32_t kRingSlots = 3;
inline constexpr uint32_t kMaxSlotPayloadBytes = 68u * 1024u * 1024u;
inline constexpr size_t   kRingTotalBytes = 4096u + kRingSlots * kMaxSlotPayloadBytes;

struct FrameHeader {
    uint64_t frame_index;
    int64_t  qpc_timestamp;
    int64_t  qpc_freq;
    uint32_t width;
    uint32_t height;
    uint32_t color_bytes;   // BGRA8 or RGBA8: width * height * 4 (0 for depth-only preview)
    uint32_t depth_bytes;   // 0 if !has_depth; width * height * (1 or 4)
    uint32_t depth_mode;    // DepthMode
    uint32_t has_depth;
    uint8_t  color_is_rgba; // 1 = RGBA8, 0 = BGRA8
    uint8_t  is_preview_only; // 1 = idle preview or readiness check
    uint8_t  reserved[2]{};
    LONG capture_generation;
    uint32_t producer_pid;
};

struct RingControlBlock {
    uint32_t magic; // 'DRNG'
    uint32_t slot_count;
    uint32_t slot_payload_bytes;
    uint32_t reserved;
    std::atomic<uint64_t> write_idx;
    std::atomic<uint64_t> read_idx;
};

static_assert(sizeof(FrameHeader) == 64);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

class IpcRing {
public:
    ~IpcRing() { close(); }

    bool open_or_create(bool create_new) {
        const DWORD size_hi = static_cast<DWORD>(static_cast<uint64_t>(kRingTotalBytes) >> 32);
        const DWORD size_lo = static_cast<DWORD>(kRingTotalBytes & 0xFFFFFFFFULL);
        bool existed = false;
        if (create_new) {
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, size_hi, size_lo, kRingMappingName);
            existed = GetLastError() == ERROR_ALREADY_EXISTS;
            event_ = CreateEventW(nullptr, FALSE, FALSE, kFrameEventName);
        } else {
            mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kRingMappingName);
            event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kFrameEventName);
        }
        if (!mapping_ || !event_) {
            close();
            return false;
        }
        base_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kRingTotalBytes));
        if (!base_) {
            close();
            return false;
        }
        ctrl_ = reinterpret_cast<RingControlBlock*>(base_);
        if (create_new && !existed) {
            ctrl_->magic = 0x44524E47;
            ctrl_->slot_count = kRingSlots;
            ctrl_->slot_payload_bytes = kMaxSlotPayloadBytes;
            ctrl_->write_idx.store(0, std::memory_order_relaxed);
            ctrl_->read_idx.store(0, std::memory_order_relaxed);
        }
        if (ctrl_->magic != 0x44524E47 || ctrl_->slot_count != kRingSlots ||
            ctrl_->slot_payload_bytes != kMaxSlotPayloadBytes) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; ctrl_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (event_) { CloseHandle(event_); event_ = nullptr; }
    }

    // Reserve next slot directly so caller can write payload into shared memory without an extra copy.
    uint8_t* begin_push(uint32_t payload_bytes) {
        if (!ctrl_) return nullptr;
        const size_t total = sizeof(FrameHeader) + static_cast<size_t>(payload_bytes);
        if (total > kMaxSlotPayloadBytes) return nullptr;

        const uint64_t w = ctrl_->write_idx.load(std::memory_order_relaxed);
        const uint64_t r = ctrl_->read_idx.load(std::memory_order_acquire);
        if (w - r >= kRingSlots) return nullptr; // ring full

        return slot_ptr(w % kRingSlots) + sizeof(FrameHeader);
    }

    void commit_push(const FrameHeader& hdr) {
        if (!ctrl_) return;
        const uint64_t w = ctrl_->write_idx.load(std::memory_order_relaxed);
        std::memcpy(slot_ptr(w % kRingSlots), &hdr, sizeof(FrameHeader));
        ctrl_->write_idx.store(w + 1, std::memory_order_release);
        SetEvent(event_);
    }

    bool try_push(const FrameHeader& hdr, const void* color_data, const void* depth_data) {
        uint8_t* dst_payload = begin_push(hdr.color_bytes + hdr.depth_bytes);
        if (!dst_payload) return false;
        if (hdr.color_bytes && color_data) std::memcpy(dst_payload, color_data, hdr.color_bytes);
        if (hdr.depth_bytes && depth_data) std::memcpy(dst_payload + hdr.color_bytes, depth_data, hdr.depth_bytes);
        commit_push(hdr);
        return true;
    }

    // Returns pointer to slot; caller MUST call release_pop() after consuming.
    const uint8_t* try_peek(FrameHeader& out_hdr) const {
        if (!ctrl_) return nullptr;
        const uint64_t r = ctrl_->read_idx.load(std::memory_order_relaxed);
        const uint64_t w = ctrl_->write_idx.load(std::memory_order_acquire);
        if (r == w) return nullptr;

        const uint8_t* src = slot_ptr(r % kRingSlots);
        std::memcpy(&out_hdr, src, sizeof(FrameHeader));
        return src + sizeof(FrameHeader);
    }

    void release_pop() {
        if (!ctrl_) return;
        const uint64_t r = ctrl_->read_idx.load(std::memory_order_relaxed);
        ctrl_->read_idx.store(r + 1, std::memory_order_release);
    }

    void reset_indices() {
        if (!ctrl_) return;
        ctrl_->write_idx.store(0, std::memory_order_relaxed);
        ctrl_->read_idx.store(0, std::memory_order_relaxed);
    }

    HANDLE event_handle() const { return event_; }

private:
    uint8_t* slot_ptr(uint64_t slot) const {
        return base_ + 4096u + slot * static_cast<size_t>(kMaxSlotPayloadBytes);
    }

    HANDLE mapping_ = nullptr;
    HANDLE event_ = nullptr;
    uint8_t* base_ = nullptr;
    RingControlBlock* ctrl_ = nullptr;
};

} // namespace dr

