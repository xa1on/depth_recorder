#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace dr {

enum class DepthMode : uint32_t {
    Gray8 = 0,
    ColorPhase = 1,
    Rgb24Lossless = 2,
};

enum class OverlayCorner : uint32_t {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
};

inline constexpr uint32_t kConfigVersion = 5;
inline constexpr wchar_t kConfigMappingName[] = L"Local\\DepthRecorderConfigV5";
inline constexpr wchar_t kRingMappingName[]   = L"Local\\DepthRecorderRingV5";
inline constexpr wchar_t kFrameEventName[]    = L"Local\\DepthRecorderFrameEventV5";
inline constexpr uint32_t kMaxSwapchains      = 8;
inline constexpr uint32_t kLogRingSize        = 32;

struct LogEntry {
    volatile LONG seq = 0; // odd while writing, even when stable
    char text[224]{};
};

struct SwapchainInfo {
    volatile LONG seq = 0; // odd while writer updates, even when stable
    uint64_t id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t window = 0;
    uint64_t last_present_ms = 0;
};

enum class RecordingActivity : LONG { Normal, WaitingForCapture, EncoderStalled };

struct SharedConfig {
    uint32_t magic = 0x44524331; // 'DRC1'
    uint32_t version = kConfigVersion;

    // Recording state (0 = idle, 1 = recording)
    volatile LONG recording = 0;
    volatile LONG depth_enabled = 1;
    volatile LONG depth_mode = static_cast<LONG>(DepthMode::Gray8);

    // Linearization (used for Gray8 / ColorPhase)
    volatile LONG linearize = 1;
    volatile LONG reversed_z = 1;
    float z_near = 0.1f;
    float z_far = 500.0f;

    // In-game FPS overlay
    volatile LONG overlay_enabled = 0;
    volatile LONG overlay_corner = static_cast<LONG>(OverlayCorner::TopLeft);
    volatile LONG preview_enabled = 1;
    volatile LONG target_rec_fps = 60;

    // Incremented by hotkey/UI to cycle through candidate depth buffers
    volatile LONG cycle_depth_seq = 0;
    // Incremented when depth_recorder.exe exits so injected hook DLLs unload cleanly
    volatile LONG unload_seq = 0;

    // Live telemetry written by hook
    volatile LONG target_pid = 0; // 0 accepts the first producer, -1 pauses all producers
    volatile LONG hooked_pid = 0;
    volatile LONG published_pairs = 0;
    volatile LONG presented_frames = 0;
    volatile LONG capture_generation = 0;
    volatile LONG preview_pair_generation = -1;
    volatile LONG active_backend = 0; // 1=DX11, 2=OpenGL, 3=Vulkan, 4=DX12
    volatile LONG candidate_depth_count = 0;
    volatile LONG selected_depth_index = 0;
    volatile LONG current_fps_x10 = 0; // FPS * 10
    volatile LONG recording_activity = 0;
    volatile LONG actual_rec_fps_x10 = 0; // Actual unique encoded FPS * 10
    volatile LONG dropped_frames = 0;

    SwapchainInfo swapchains[kMaxSwapchains]{};
    volatile LONG swapchain_count = 0;
    volatile LONG selected_swapchain_index = 0;

    LogEntry log_entries[kLogRingSize]{};
    volatile LONG log_write_seq = 0;
};

inline void log_push(SharedConfig* cfg, const char* tag, const char* fmt, ...) {
    if (!cfg || !tag || !fmt) return;
    const LONG ticket = InterlockedIncrement(&cfg->log_write_seq);
    auto& dst = cfg->log_entries[static_cast<uint32_t>(ticket - 1) % kLogRingSize];
    InterlockedExchange(&dst.seq, ticket * 2 - 1);
    MemoryBarrier();
    const int n = std::snprintf(dst.text, sizeof(dst.text), "[%s] ", tag);
    if (n > 0 && static_cast<size_t>(n) < sizeof(dst.text)) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(dst.text + n, sizeof(dst.text) - static_cast<size_t>(n), fmt, args);
        va_end(args);
    }
    dst.text[sizeof(dst.text) - 1] = '\0';
    MemoryBarrier();
    InterlockedExchange(&dst.seq, ticket * 2);
}

inline bool read_log_entry(const SharedConfig* cfg, LONG ticket, std::string& out) {
    if (!cfg || ticket <= 0) return false;
    const auto& src = cfg->log_entries[static_cast<uint32_t>(ticket - 1) % kLogRingSize];
    const LONG expected = ticket * 2;
    const LONG s1 = InterlockedCompareExchange(const_cast<volatile LONG*>(&src.seq), 0, 0);
    if (s1 != expected) return false;
    MemoryBarrier();
    char buf[sizeof(src.text)]{};
    std::memcpy(buf, src.text, sizeof(buf) - 1);
    MemoryBarrier();
    const LONG s2 = InterlockedCompareExchange(const_cast<volatile LONG*>(&src.seq), 0, 0);
    if (s2 != expected) return false;
    out = buf;
    return true;
}

inline bool read_swapchain_info(const SharedConfig* cfg, LONG index, SwapchainInfo& out) {
    if (!cfg || index < 0 || index >= static_cast<LONG>(kMaxSwapchains)) return false;
    const auto& src = cfg->swapchains[index];
    for (int attempt = 0; attempt < 8; ++attempt) {
        const LONG s1 = InterlockedCompareExchange(const_cast<volatile LONG*>(&src.seq), 0, 0);
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        out.id = src.id;
        out.width = src.width;
        out.height = src.height;
        out.window = src.window;
        out.last_present_ms = src.last_present_ms;
        MemoryBarrier();
        const LONG s2 = InterlockedCompareExchange(const_cast<volatile LONG*>(&src.seq), 0, 0);
        if (s1 == s2) {
            out.seq = s2;
            return true;
        }
    }
    return false;
}

struct SwapchainTarget {
    uint64_t id = 0;
    uint64_t window = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const SwapchainTarget& other) const {
        return (window && other.window) ? (window == other.window) : (id == other.id && window == other.window);
    }
};

inline SwapchainTarget swapchain_target(const SwapchainInfo& info) {
    return {info.id, info.window};
}

// Registers a swapchain/window in SharedConfig and returns true if it is currently selected by the user.
inline bool register_and_is_selected_swapchain(SharedConfig* cfg, uint64_t id, uint32_t w, uint32_t h, HWND hwnd) {
    if (!cfg || id == 0 || w < 64 || h < 64) return false;
    const LONG pid = static_cast<LONG>(GetCurrentProcessId());
    const LONG target = InterlockedCompareExchange(&cfg->target_pid, pid, 0);
    if (target != 0 && target != pid) return false;
    LONG count = std::clamp<LONG>(static_cast<LONG>(cfg->swapchain_count), 0, kMaxSwapchains);
    LONG found = -1;
    LONG vacant = -1;
    const uint64_t window = reinterpret_cast<uintptr_t>(hwnd);
    const uint64_t now = GetTickCount64();
    for (LONG i = 0; i < count; ++i) {
        SwapchainInfo entry{};
        if (!read_swapchain_info(cfg, i, entry)) continue;
        if (entry.id == id && entry.window == window) {
            found = i;
            break;
        }
        if (window && entry.window == window) {
            // Keep a stable window selection if the app recreates its swapchain.
            if (now - entry.last_present_ms < 500) return false;
            found = i;
            break;
        }
        // The recorder owns selection changes. A different window must not inherit the selected slot.
        if (vacant == -1 && i != cfg->selected_swapchain_index && entry.window &&
            !IsWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(entry.window)))) vacant = i;
    }
    const bool append = found == -1 && vacant == -1 && count < static_cast<LONG>(kMaxSwapchains);
    if (found == -1) found = append ? count : vacant;
    if (found == -1) return false;

    auto& dst = cfg->swapchains[found];
    InterlockedIncrement(&dst.seq); // odd = writing
    MemoryBarrier();
    dst.id = id;
    dst.width = w;
    dst.height = h;
    dst.window = window;
    dst.last_present_ms = now;
    MemoryBarrier();
    InterlockedIncrement(&dst.seq); // even = stable

    if (append) InterlockedExchange(&cfg->swapchain_count, ++count);
    const LONG sel = cfg->selected_swapchain_index;
    if (sel < 0 || sel >= count) return false;
    return found == sel;
}

inline LONG largest_swapchain_index(const SharedConfig& cfg) {
    const LONG count = std::clamp<LONG>(static_cast<LONG>(cfg.swapchain_count), 0, kMaxSwapchains);
    uint64_t largest = 0;
    LONG selected = -1;
    for (LONG i = 0; i < count; ++i) {
        SwapchainInfo entry{};
        if (!read_swapchain_info(&cfg, i, entry) || !entry.id) continue;
        if (entry.window && !IsWindow(reinterpret_cast<HWND>(static_cast<uintptr_t>(entry.window)))) continue;
        const uint64_t area = static_cast<uint64_t>(entry.width) * entry.height;
        if (area > largest) {
            largest = area;
            selected = i;
        }
    }
    return selected;
}

class SharedConfigView {
public:
    ~SharedConfigView() { close(); }

    bool open_or_create(bool create_new) {
        if (create_new) {
            handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedConfig), kConfigMappingName);
        } else {
            handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kConfigMappingName);
        }
        if (!handle_) return false;
        const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
        ptr_ = static_cast<SharedConfig*>(MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedConfig)));
        if (!ptr_) {
            close();
            return false;
        }
        if (create_new && !existed) {
            *ptr_ = SharedConfig{};
        }
        if (ptr_->magic != 0x44524331 || ptr_->version != kConfigVersion) {
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (ptr_) {
            UnmapViewOfFile(ptr_);
            ptr_ = nullptr;
        }
        if (handle_) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    SharedConfig* get() const { return ptr_; }

private:
    HANDLE handle_ = nullptr;
    SharedConfig* ptr_ = nullptr;
};

} // namespace dr
