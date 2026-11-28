#pragma once
#include "common/config.h"
#include "common/ipc_ring.h"
#include "encoder.h"
#include "logger.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace dr {

enum class RecordingStatus { Idle, Starting, Recording, Saving, Saved, Failed };

struct RecordingControl {
    std::mutex mutex;
    RecordingStatus status = RecordingStatus::Idle;
    EncoderSettings settings;
    bool drop_dead_frames = false;
    LONG generation = 0;
    HWND window = nullptr;
    int64_t start_requested = 0;
    int64_t stop_requested = 0;
    std::wstring message;
    std::wstring folder;
    FrameHeader preview_profile{};
};

struct RecorderAppState {
    SharedConfigView config_view;
    IpcRing ring;
    VideoEncoder encoder;
    RecorderLog log;
    std::wstring output_dir = L".\\recordings";
    std::wstring hook_dll_path;
    std::wstring extra_ffmpeg_flags;
    uint32_t recording_fps = 60;
    bool drop_dead_frames = false;
    std::atomic<uint32_t> h264_quality{85};
    UINT hotkey_record_vk = VK_F9;
    UINT hotkey_record_mod = 0;
    std::atomic<bool> running{true};
    RecordingControl recording;
    std::atomic<LONG> ready_generation{-1};
    std::atomic<LONG> encoder_ready_generation{-1};
    std::atomic<LONG> encoder_failed_generation{-1};
    std::atomic<int64_t> ready_qpc{0};
    std::atomic<uint64_t> received_pairs{0};
    std::atomic<uint64_t> inbox_full_pairs{0};
    std::atomic<uint64_t> written_pairs{0};
    std::atomic<uint64_t> unique_written_pairs{0};
    std::atomic<int64_t> clip_started_qpc{0};
    std::atomic<int64_t> encoder_write_qpc{0};
    std::atomic<LONG> present_fps_x10{0};
    std::atomic<bool> show_preview{true};
    bool discard_output = false;
    bool software_encoder = false;

    std::mutex preview_mutex;
    std::vector<uint8_t> preview_bgra;
    uint32_t preview_w = 0;
    uint32_t preview_h = 0;
    uint32_t preview_source_w = 0;
    uint32_t preview_source_h = 0;
};

bool start_recording(RecorderAppState& app, bool require_ready = true);
void stop_recording(RecorderAppState& app);

int run_win32_ui(HINSTANCE hInstance, RecorderAppState& app);

} // namespace dr

