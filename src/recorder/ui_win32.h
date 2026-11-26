#pragma once
#include "common/config.h"
#include "common/ipc_ring.h"
#include "encoder.h"
#include <atomic>
#include <string>
#include <windows.h>

namespace dr {

struct RecorderAppState {
    SharedConfigView config_view;
    IpcRing ring;
    VideoEncoder encoder;
    std::wstring output_dir = L".\\recordings";
    std::wstring hook_dll_path;
    UINT hotkey_record_vk = VK_F9;
    UINT hotkey_cycle_vk = VK_F10;
    std::atomic<bool> running{true};
    std::atomic<bool> stop_requested{false};
};

int run_win32_ui(HINSTANCE hInstance, RecorderAppState& app);

} // namespace dr

