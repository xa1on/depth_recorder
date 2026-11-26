#include "encoder.h"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace dr {

std::wstring VideoEncoder::find_ffmpeg() {
    wchar_t exe_dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    std::filesystem::path local_ffmpeg = std::filesystem::path(exe_dir).parent_path() / L"ffmpeg.exe";
    if (std::filesystem::exists(local_ffmpeg)) {
        return local_ffmpeg.wstring();
    }
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return found;
    }
    return L"";
}

std::string VideoEncoder::probe_h264_encoder(const std::wstring& ffmpeg_path) {
    if (ffmpeg_path.empty()) return "libopenh264";

    for (const char* cand : {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", "libopenh264"}) {
        std::wstring cand_w(cand, cand + std::strlen(cand));
        std::wstring cmd = L"\"" + ffmpeg_path +
            L"\" -hide_banner -loglevel error -f lavfi -i color=c=black:s=64x64:d=0.1 -c:v " +
            cand_w + L" -f null -";
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            DWORD exit_code = 1;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (exit_code == 0) return cand;
        }
    }
    return "libopenh264";
}

bool VideoEncoder::spawn_pipe(const std::wstring& cmdline, PipeProcess& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 4 * 1024 * 1024)) return false;
    SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = read_pipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::wstring mutable_cmd = cmdline;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(read_pipe);
    if (!ok) {
        CloseHandle(write_pipe);
        return false;
    }
    CloseHandle(pi.hThread);
    out.write_pipe = write_pipe;
    out.process = pi.hProcess;
    return true;
}

bool VideoEncoder::write_all(HANDLE pipe, const uint8_t* data, uint32_t bytes) {
    uint32_t written_total = 0;
    while (written_total < bytes) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, data + written_total, bytes - written_total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        written_total += chunk;
    }
    return true;
}

void VideoEncoder::close_pipe(PipeProcess& p) {
    if (p.write_pipe) {
        CloseHandle(p.write_pipe);
        p.write_pipe = nullptr;
    }
    if (p.process) {
        WaitForSingleObject(p.process, 5000);
        CloseHandle(p.process);
        p.process = nullptr;
    }
}

bool VideoEncoder::start(uint32_t width, uint32_t height, const EncoderSettings& settings) {
    stop();
    const std::wstring ffmpeg = find_ffmpeg();
    if (ffmpeg.empty()) return false;

    static const std::string h264_codec = probe_h264_encoder(ffmpeg);
    const std::wstring h264_w(h264_codec.begin(), h264_codec.end());

    std::filesystem::create_directories(settings.output_dir);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[64]{};
    swprintf_s(ts, L"%04d%02d%02d_%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const std::wstring res = std::to_wstring(width) + L"x" + std::to_wstring(height);
    const std::wstring fps_s = std::to_wstring(settings.fps ? settings.fps : 60);

    color_path_ = (std::filesystem::path(settings.output_dir) / (std::wstring(L"color_") + ts + L".mp4")).wstring();
    const std::wstring color_cmd =
        L"\"" + ffmpeg + L"\" -y -loglevel error -f rawvideo -pix_fmt bgra -s " + res +
        L" -r " + fps_s + L" -i - -c:v " + h264_w + L" -pix_fmt yuv420p \"" + color_path_ + L"\"";

    if (!spawn_pipe(color_cmd, color_proc_)) return false;

    has_depth_ = settings.depth_enabled;
    if (has_depth_) {
        std::wstring depth_cmd;
        if (settings.depth_mode == DepthMode::Rgb24Lossless) {
            depth_path_ = (std::filesystem::path(settings.output_dir) / (std::wstring(L"depth_") + ts + L".mkv")).wstring();
            depth_cmd =
                L"\"" + ffmpeg + L"\" -y -loglevel error -f rawvideo -pix_fmt rgb0 -s " + res +
                L" -r " + fps_s + L" -i - -c:v utvideo -pix_fmt rgb24 \"" + depth_path_ + L"\"";
        } else {
            depth_path_ = (std::filesystem::path(settings.output_dir) / (std::wstring(L"depth_") + ts + L".mp4")).wstring();
            const std::wstring in_fmt = (settings.depth_mode == DepthMode::Gray8) ? L"gray" : L"rgb0";
            depth_cmd =
                L"\"" + ffmpeg + L"\" -y -loglevel error -f rawvideo -pix_fmt " + in_fmt + L" -s " + res +
                L" -r " + fps_s + L" -i - -c:v " + h264_w + L" -pix_fmt yuv420p \"" + depth_path_ + L"\"";
        }
        if (!spawn_pipe(depth_cmd, depth_proc_)) {
            close_pipe(color_proc_);
            return false;
        }
    } else {
        depth_path_.clear();
    }

    width_ = width;
    height_ = height;
    frames_written_ = 0;
    running_ = true;
    return true;
}

bool VideoEncoder::write_frame(const uint8_t* color_bgra, uint32_t color_bytes,
                               const uint8_t* depth_data, uint32_t depth_bytes) {
    if (!running_) return false;
    if (!write_all(color_proc_.write_pipe, color_bgra, color_bytes)) return false;
    if (has_depth_ && depth_data && depth_bytes > 0) {
        if (!write_all(depth_proc_.write_pipe, depth_data, depth_bytes)) return false;
    }
    ++frames_written_;
    return true;
}

void VideoEncoder::stop() {
    if (!running_) return;
    close_pipe(color_proc_);
    close_pipe(depth_proc_);
    running_ = false;
}

} // namespace dr
