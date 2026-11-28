#pragma once
#include "common/config.h"
#include <cstdint>
#include <string>
#include <windows.h>

namespace dr {

struct EncoderSettings {
    std::wstring output_dir = L".";
    DepthMode depth_mode = DepthMode::Rgb24Lossless;
    bool depth_enabled = true;
    uint32_t fps = 60;
    uint32_t h264_quality = 85; // 1..100
    bool color_is_rgba = false;
    bool software_only = false;
    std::wstring extra_ffmpeg_flags;
};

class VideoEncoder {
public:
    ~VideoEncoder() { stop(false); }

    static std::wstring find_ffmpeg();
    static std::wstring new_clip_name();

    bool prepare(uint32_t width, uint32_t height, const EncoderSettings& settings);
    bool start(uint32_t width, uint32_t height, const EncoderSettings& settings);
    bool write_frame(const uint8_t* color_bgra, uint32_t color_bytes,
                     const uint8_t* depth_data, uint32_t depth_bytes);
    bool stop(bool complete = true, const std::wstring& reason = {});

    bool is_running() const { return running_; }
    uint64_t frames_written() const { return frames_written_; }
    const std::wstring& last_folder() const { return last_folder_; }
    const std::wstring& error() const { return error_; }
    const std::wstring& last_color_path() const { return color_path_; }
    const std::wstring& last_depth_path() const { return depth_path_; }

private:
    struct PipeProcess {
        HANDLE write_pipe = nullptr;
        HANDLE write_event = nullptr;
        HANDLE process = nullptr;
        HANDLE stderr_file = nullptr;
    };

    static bool spawn_pipe(const std::wstring& cmdline, PipeProcess& out);
    static bool finish_process(PipeProcess& p, DWORD timeout, std::wstring& error);

    struct EncoderKey {
        std::wstring ffmpeg;
        uint32_t width, height, fps, quality;
        bool depth_enabled;
        DepthMode depth_mode;
        bool rgba, software_only;
        std::wstring extra_flags;

        bool operator==(const EncoderKey&) const = default;
    };
    EncoderKey prepared_key_{};
    std::string prepared_codec_;
    PipeProcess color_proc_;
    PipeProcess depth_proc_;
    std::wstring staging_folder_;
    std::wstring final_folder_;
    std::wstring last_folder_;
    std::wstring error_;
    std::wstring color_path_;
    std::wstring depth_path_;
    uint32_t color_frame_bytes_ = 0;
    uint32_t depth_frame_bytes_ = 0;
    bool has_depth_ = false;
    bool running_ = false;
    uint64_t frames_written_ = 0;
};

} // namespace dr

