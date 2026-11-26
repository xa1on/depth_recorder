#pragma once
#include <cstdint>
#include <cstring>
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

inline constexpr wchar_t kConfigMappingName[] = L"Local\\DepthRecorderConfigV1";
inline constexpr wchar_t kRingMappingName[]   = L"Local\\DepthRecorderRingV1";
inline constexpr wchar_t kFrameEventName[]    = L"Local\\DepthRecorderFrameEventV1";

struct SharedConfig {
    uint32_t magic = 0x44524331; // 'DRC1'
    uint32_t version = 1;

    // Recording state (0 = idle, 1 = recording)
    volatile LONG recording = 0;
    volatile LONG depth_enabled = 1;
    volatile LONG depth_mode = static_cast<LONG>(DepthMode::Rgb24Lossless);

    // Linearization (used for Gray8 / ColorPhase)
    volatile LONG linearize = 1;
    volatile LONG reversed_z = 1;
    float z_near = 0.1f;
    float z_far = 500.0f;

    // In-game FPS overlay
    volatile LONG overlay_enabled = 1;
    volatile LONG overlay_corner = static_cast<LONG>(OverlayCorner::TopLeft);
    volatile LONG preview_enabled = 1;

    // Incremented by hotkey/UI to cycle through candidate depth buffers
    volatile LONG cycle_depth_seq = 0;
    // Incremented when depth_recorder.exe exits so injected hook DLLs unload cleanly
    volatile LONG unload_seq = 0;

    // Live telemetry written by hook
    volatile LONG hooked_pid = 0;
    volatile LONG active_backend = 0; // 1=DX11, 2=OpenGL, 3=Vulkan, 4=DX12
    volatile LONG candidate_depth_count = 0;
    volatile LONG selected_depth_index = 0;
    volatile LONG current_fps_x10 = 0; // FPS * 10
    volatile LONG dropped_frames = 0;
};

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

