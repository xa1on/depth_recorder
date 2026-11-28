#include "encoder.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <vector>

namespace dr {

std::wstring VideoEncoder::find_ffmpeg() {
    wchar_t override_path[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"DEPTH_RECORDER_FFMPEG", override_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(override_path, ec)) {
            return override_path;
        }
    }
    wchar_t exe_dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    std::filesystem::path local_ffmpeg = std::filesystem::path(exe_dir).parent_path() / L"ffmpeg.exe";
    if (std::filesystem::exists(local_ffmpeg)) {
        return local_ffmpeg.wstring();
    }
    return L"";
}

namespace {

constexpr DWORD kWriteTimeoutMs = 2500;

std::wstring h264_options(const std::string& h264_codec, uint32_t quality) {
    const uint32_t q = std::clamp<uint32_t>(quality, 1, 100);
    const uint32_t qp = 42 - (q * 32) / 100;
    const uint32_t mbps = std::max<uint32_t>(2, (q * 16) / 100);
    if (h264_codec == "h264_nvenc") {
        return L"-preset p1 -tune ull -rc vbr -cq " + std::to_wstring(qp) + L" -b:v 0";
    }
    if (h264_codec == "h264_amf") {
        return L"-quality speed -rc cqp -qp_i " + std::to_wstring(qp) + L" -qp_p " + std::to_wstring(qp);
    }
    if (h264_codec == "h264_qsv") {
        return L"-preset veryfast -global_quality " + std::to_wstring(qp);
    }
    if (h264_codec == "h264_mf") {
        return L"-hw_encoding 1 -b:v " + std::to_wstring(mbps) + L"M";
    }
    if (h264_codec == "libx264") {
        return L"-preset ultrafast -crf " + std::to_wstring(qp);
    }
    return L"-b:v " + std::to_wstring(mbps) + L"M -maxrate " + std::to_wstring(mbps * 2) + L"M";
}

std::wstring output_padding(uint32_t width, uint32_t height) {
    return ((width | height) & 1u) ? L" -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\"" : L"";
}

std::wstring h264_output_options(const std::string& codec, uint32_t width, uint32_t height,
                                 uint32_t quality, bool rgb_input, const std::wstring& extra_flags) {
    std::wstring pixel_format = L" -pix_fmt yuv420p";
    if (codec == "h264_nvenc") pixel_format = rgb_input ? L"" : L" -pix_fmt nv12";
    if (codec == "h264_amf" || codec == "h264_mf") pixel_format = L" -pix_fmt nv12";
    const std::wstring extra = extra_flags.empty() ? L"" : (L" " + extra_flags);
    return output_padding(width, height) + L" -c:v " + std::wstring(codec.begin(), codec.end()) + L" " +
           h264_options(codec, quality) + pixel_format + extra;
}

std::wstring depth_output_options(const std::string& codec, uint32_t width, uint32_t height,
                                  const EncoderSettings& settings) {
    if (settings.depth_mode == DepthMode::Rgb24Lossless) {
        const std::wstring extra = settings.extra_ffmpeg_flags.empty() ? L"" : (L" " + settings.extra_ffmpeg_flags);
        return output_padding(width, height) + L" -c:v utvideo -pix_fmt rgb24" + extra;
    }
    return h264_output_options(codec, width, height, settings.h264_quality,
                               settings.depth_mode != DepthMode::Gray8, settings.extra_ffmpeg_flags);
}

std::wstring encoder_command(const std::wstring& ffmpeg) {
    return L"\"" + ffmpeg + L"\" -nostdin -n -hide_banner -loglevel error -filter_threads 1";
}

std::wstring make_id() {
    static std::atomic<uint32_t> sequence{0};
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t id[100]{};
    swprintf_s(id, L"clip_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%u",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
               now.wMilliseconds, GetCurrentProcessId(), ++sequence);
    return id;
}

} // namespace

std::wstring VideoEncoder::new_clip_name() { return make_id(); }

bool VideoEncoder::spawn_pipe(const std::wstring& cmdline, PipeProcess& out) {
    static std::atomic<uint32_t> sequence{0};
    const std::wstring name = LR"(\\.\pipe\DepthRecorderEncoder-)" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    out.write_pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 256 * 1024, 0, 0, nullptr);
    if (out.write_pipe == INVALID_HANDLE_VALUE) {
        out.write_pipe = nullptr;
        return false;
    }
    out.write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!out.write_event) {
        CloseHandle(out.write_pipe);
        out.write_pipe = nullptr;
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const HANDLE read_pipe = CreateFileW(name.c_str(), GENERIC_READ, 0, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (read_pipe == INVALID_HANDLE_VALUE) {
        CloseHandle(out.write_event);
        CloseHandle(out.write_pipe);
        out = {};
        return false;
    }

    wchar_t temp_dir[MAX_PATH]{};
    wchar_t err_file[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp_dir) && GetTempFileNameW(temp_dir, L"drf", 0, err_file)) {
        out.stderr_file = CreateFileW(err_file, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (out.stderr_file == INVALID_HANDLE_VALUE) out.stderr_file = nullptr;
    }

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = read_pipe;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = out.stderr_file ? out.stderr_file : GetStdHandle(STD_ERROR_HANDLE);

    std::wstring mutable_cmd = cmdline;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(read_pipe);
    if (!ok) {
        if (out.stderr_file) CloseHandle(out.stderr_file);
        CloseHandle(out.write_event);
        CloseHandle(out.write_pipe);
        out = {};
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    return true;
}

bool VideoEncoder::finish_process(PipeProcess& p, DWORD timeout, std::wstring& error) {
    if (p.write_pipe) CloseHandle(p.write_pipe);
    if (p.write_event) CloseHandle(p.write_event);
    bool ok = false;
    if (p.process) {
        if (WaitForSingleObject(p.process, timeout) != WAIT_OBJECT_0) {
            TerminateProcess(p.process, 1);
            WaitForSingleObject(p.process, 1000);
        }
        DWORD exit_code = 1;
        GetExitCodeProcess(p.process, &exit_code);
        ok = (exit_code == 0);
        CloseHandle(p.process);
    }
    if (p.stderr_file) {
        char message[512]{};
        DWORD size = 0;
        SetFilePointer(p.stderr_file, 0, nullptr, FILE_BEGIN);
        if (ReadFile(p.stderr_file, message, sizeof(message) - 1, &size, nullptr) && size) {
            // All commands use -loglevel error. Some failures (including -n) can still exit with code zero.
            if (ok) error = L"FFmpeg reported an error.";
            ok = false;
            wchar_t detail[std::size(message)]{};
            const int length = MultiByteToWideChar(CP_UTF8, 0, message, static_cast<int>(size),
                                                   detail, static_cast<int>(std::size(detail)));
            if (length > 0) error.append(L" ").append(detail, static_cast<size_t>(length));
        }
        CloseHandle(p.stderr_file);
    }
    p = {};
    return ok;
}

bool VideoEncoder::prepare(uint32_t width, uint32_t height, const EncoderSettings& settings) {
    const auto ffmpeg = find_ffmpeg();
    if (ffmpeg.empty()) {
        error_ = L"The portable package is missing ffmpeg.exe.";
        return false;
    }
    const EncoderKey key{ffmpeg, width, height, settings.fps, settings.h264_quality, settings.depth_enabled,
                         settings.depth_mode, settings.color_is_rgba, settings.software_only, settings.extra_ffmpeg_flags};
    if (key == prepared_key_ && !prepared_codec_.empty()) return true;

    prepared_key_ = key;
    prepared_codec_.clear();
    error_.clear();
    const auto command = encoder_command(ffmpeg);
    const auto size = std::to_wstring(width) + L"x" + std::to_wstring(height);
    const auto input = L" -f lavfi -i color=size=" + size + L":rate=" + std::to_wstring(settings.fps) + L",format=";
    const wchar_t* color = settings.color_is_rgba ? L"rgba" : L"bgra";
    const wchar_t* depth = settings.depth_mode == DepthMode::Gray8 ? L"gray" : L"rgb0";
    const std::wstring null_output = L" -frames:v 1 -f null -";
    for (const char* candidate : {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", "libx264", "libopenh264"}) {
        if (settings.software_only && std::string_view(candidate).starts_with("h264_")) continue;
        auto probe_cmd = command + input + color;
        if (settings.depth_enabled) probe_cmd += input + depth;
        probe_cmd += L" -map 0:v" + h264_output_options(candidate, width, height, settings.h264_quality, true, settings.extra_ffmpeg_flags) + null_output;
        if (settings.depth_enabled) {
            probe_cmd += L" -map 1:v" + depth_output_options(candidate, width, height, settings) + null_output;
        }
        PipeProcess probe;
        std::wstring ignored;
        if (spawn_pipe(probe_cmd, probe) && finish_process(probe, 2000, ignored)) {
            prepared_codec_ = candidate;
            return true;
        }
    }
    error_ = L"No encoder supports this recording profile.";
    return false;
}

bool VideoEncoder::start(uint32_t width, uint32_t height, const EncoderSettings& settings) {
    if (running_) return false;
    error_.clear();
    last_folder_.clear();
    frames_written_ = 0;
    if (!prepare(width, height, settings)) return false;
    has_depth_ = settings.depth_enabled;
    color_frame_bytes_ = width * height * 4;
    depth_frame_bytes_ = width * height * (settings.depth_mode == DepthMode::Gray8 ? 1 : 4);

    const auto command = encoder_command(prepared_key_.ffmpeg);
    const std::wstring res = std::to_wstring(width) + L"x" + std::to_wstring(height);
    const std::wstring fps = std::to_wstring(settings.fps);
    const auto raw_input = [&](const wchar_t* format) {
        return L" -probesize 32 -analyzeduration 0 -f rawvideo -pixel_format " + std::wstring(format) + L" -video_size " + res +
               L" -framerate " + fps + L" -i pipe:0";
    };
    const wchar_t* color_format = settings.color_is_rgba ? L"rgba" : L"bgra";
    const wchar_t* depth_format = settings.depth_mode == DepthMode::Gray8 ? L"gray" : L"rgb0";
    const auto launch = [&](const std::wstring& color_output, const std::wstring& depth_output) {
        if (spawn_pipe(command + raw_input(color_format) + color_output, color_proc_) &&
            (!has_depth_ || spawn_pipe(command + raw_input(depth_format) + depth_output, depth_proc_))) {
            running_ = true;
            return true;
        }
        std::wstring detail;
        finish_process(color_proc_, 250, detail);
        finish_process(depth_proc_, 250, detail);
        error_ = L"No usable encoder could start for this recording.";
        return false;
    };

    const std::wstring id = make_id();
    const auto parent = std::filesystem::path(settings.output_dir);
    final_folder_ = (parent / id).wstring();
    // Partial output remains identifiable even if the process or final rename fails.
    staging_folder_ = (parent / (id + L"_partial")).wstring();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec || !std::filesystem::create_directory(staging_folder_, ec)) {
        error_ = L"The recording folder could not be created.";
        return false;
    }
    const auto folder = std::filesystem::path(staging_folder_);
    color_path_ = (folder / (id + L"_color.mp4")).wstring();

    const bool lossless = settings.depth_mode == DepthMode::Rgb24Lossless;
    depth_path_ = has_depth_ ? (folder / (id + (lossless ? L"_depth.mkv" : L"_depth.mp4"))).wstring() : L"";

    const auto color_options = h264_output_options(prepared_codec_, width, height, settings.h264_quality, true, settings.extra_ffmpeg_flags);
    if (launch(color_options + L" \"" + color_path_ + L"\"",
               depth_output_options(prepared_codec_, width, height, settings) + L" \"" + depth_path_ + L"\"")) return true;

    prepared_codec_.clear();
    std::filesystem::remove(staging_folder_, ec);
    return false;
}

bool VideoEncoder::write_frame(const uint8_t* color, uint32_t color_bytes, const uint8_t* depth, uint32_t depth_bytes) {
    if (!running_ || color_bytes != color_frame_bytes_ || (has_depth_ && (!depth || depth_bytes != depth_frame_bytes_))) {
        error_ = L"An incomplete frame pair reached the encoder.";
        return false;
    }
    struct PendingWrite {
        const PipeProcess& pipe;
        const uint8_t* data;
        uint32_t count;
        OVERLAPPED operation{};
        bool pending = false;
        bool ok = false;
    };
    PendingWrite writes[] = {{color_proc_, color, color_bytes}, {depth_proc_, depth, depth_bytes}};
    const size_t count = has_depth_ ? 2 : 1;
    HANDLE pending_events[2]{};
    DWORD pending_count = 0;
    bool failed = false;
    for (size_t i = 0; i < count; ++i) {
        auto& write = writes[i];
        ResetEvent(write.pipe.write_event);
        write.operation.hEvent = write.pipe.write_event;
        DWORD transferred = 0;
        if (WriteFile(write.pipe.write_pipe, write.data, write.count, &transferred, &write.operation)) {
            write.ok = transferred == write.count;
            failed = failed || !write.ok;
        } else if (GetLastError() == ERROR_IO_PENDING) {
            write.pending = true;
            pending_events[pending_count++] = write.pipe.write_event;
        } else {
            failed = true;
        }
    }

    const ULONGLONG deadline = GetTickCount64() + kWriteTimeoutMs;
    bool timed_out = false;
    while (!failed && pending_count) {
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        const DWORD result = WaitForMultipleObjects(pending_count, pending_events, FALSE, remaining);
        if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + pending_count) {
            timed_out = result == WAIT_TIMEOUT;
            failed = true;
            break;
        }
        const DWORD event = result - WAIT_OBJECT_0;
        for (size_t i = 0; i < count; ++i) {
            auto& write = writes[i];
            if (!write.pending || write.operation.hEvent != pending_events[event]) continue;
            DWORD transferred = 0;
            write.ok = GetOverlappedResult(write.pipe.write_pipe, &write.operation, &transferred, FALSE) &&
                       transferred == write.count;
            write.pending = false;
            failed = !write.ok;
            break;
        }
        pending_events[event] = pending_events[--pending_count];
    }

    if (failed) {
        // Cancel both before draining either. The buffers and OVERLAPPED objects
        // must stay alive until the local named-pipe cancellation completes.
        for (auto& write : writes) {
            if (write.pending) CancelIoEx(write.pipe.write_pipe, &write.operation);
        }
        for (auto& write : writes) {
            if (!write.pending) continue;
            DWORD ignored = 0;
            GetOverlappedResult(write.pipe.write_pipe, &write.operation, &ignored, TRUE);
        }
        error_ = timed_out ? L"The encoder timed out while writing a frame pair." :
                 !writes[0].ok && !writes[0].pending ? L"Writing the color video failed." : L"Writing the depth video failed.";
        return false;
    }
    ++frames_written_;
    return true;
}

bool VideoEncoder::stop(bool complete, const std::wstring& reason) {
    if (!running_) return false;
    if (!reason.empty()) error_ = reason;
    // Signal EOF on both pipes before waiting for either muxer.
    if (color_proc_.write_pipe) CloseHandle(color_proc_.write_pipe);
    if (depth_proc_.write_pipe) CloseHandle(depth_proc_.write_pipe);
    color_proc_.write_pipe = nullptr;
    depth_proc_.write_pipe = nullptr;
    std::wstring color_error, depth_error;
    const ULONGLONG deadline = GetTickCount64() + (complete ? 5000 : 1500);
    const auto remaining = [deadline] {
        const auto now = GetTickCount64();
        return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    };
    const bool color_ok = finish_process(color_proc_, remaining(), color_error);
    const bool depth_ok = finish_process(depth_proc_, remaining(), depth_error);
    running_ = false;
    if (!color_error.empty()) error_ += (error_.empty() ? L"Color video. " : L" Color video. ") + color_error;
    if (!depth_error.empty()) error_ += (error_.empty() ? L"Depth video. " : L" Depth video. ") + depth_error;

    std::error_code ec;
    const bool color_exists = std::filesystem::is_regular_file(color_path_, ec);
    const bool depth_exists = !has_depth_ || std::filesystem::is_regular_file(depth_path_, ec);
    const bool success = complete && frames_written_ && color_ok && depth_ok && color_exists && depth_exists;
    if (!success) prepared_codec_.clear();
    ec.clear();
    if (success) std::filesystem::rename(staging_folder_, final_folder_, ec);
    last_folder_ = success && !ec ? final_folder_ : staging_folder_;
    color_path_ = (std::filesystem::path(last_folder_) / std::filesystem::path(color_path_).filename()).wstring();
    if (has_depth_) depth_path_ = (std::filesystem::path(last_folder_) / std::filesystem::path(depth_path_).filename()).wstring();
    if (ec) error_ = L"Could not finalize the output folder.";
    if (!success && error_.empty()) error_ = L"The recordings could not be finalized.";
    return success && !ec;
}

} // namespace dr
