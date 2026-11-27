#pragma once
#include "common/config.h"
#include "common/ipc_ring.h"
#include "encoder.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace dr {

struct RecorderAppState {
    SharedConfigView config_view;
    IpcRing ring;
    VideoEncoder encoder;
    std::wstring output_dir = L".\\recordings";
    std::wstring hook_dll_path;
    uint32_t recording_fps = 60;
    uint32_t h264_quality = 85;
    std::wstring extra_ffmpeg_flags;
    UINT hotkey_record_vk = VK_F9;
    UINT hotkey_record_mod = 0;
    std::atomic<bool> running{true};
    std::atomic<bool> stop_requested{false};

    std::mutex preview_mutex;
    std::vector<uint8_t> preview_bgra;
    uint32_t preview_w = 0;
    uint32_t preview_h = 0;
};

int run_win32_ui(HINSTANCE hInstance, RecorderAppState& app);

} // namespace dr

