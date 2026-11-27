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
    std::wstring extra_ffmpeg_flags;
};

class VideoEncoder {
public:
    ~VideoEncoder() { stop(); }

    static std::wstring find_ffmpeg();
    static std::string probe_h264_encoder(const std::wstring& ffmpeg_path);

    bool start(uint32_t width, uint32_t height, const EncoderSettings& settings);
    bool write_frame(const uint8_t* color_bgra, uint32_t color_bytes,
                     const uint8_t* depth_data, uint32_t depth_bytes);
    void stop();

    bool is_running() const { return running_; }
    uint64_t frames_written() const { return frames_written_; }
    const std::wstring& last_color_path() const { return color_path_; }
    const std::wstring& last_depth_path() const { return depth_path_; }

private:
    struct PipeProcess {
        HANDLE write_pipe = nullptr;
        HANDLE process = nullptr;
    };

    static bool spawn_pipe(const std::wstring& cmdline, PipeProcess& out);
    static bool write_all(HANDLE pipe, const uint8_t* data, uint32_t bytes);
    static void close_pipe(PipeProcess& p);

    PipeProcess color_proc_;
    PipeProcess depth_proc_;
    std::wstring color_path_;
    std::wstring depth_path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool has_depth_ = false;
    bool running_ = false;
    uint64_t frames_written_ = 0;
};

} // namespace dr

