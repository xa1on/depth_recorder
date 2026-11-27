#include "ui_win32.h"
#include "injector.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shellapi.h>
#include <thread>
#include <vector>

namespace {

struct LatestFrame {
    std::mutex mutex;
    uint64_t frame_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth_mode = 0;
    bool has_depth = false;
    bool color_is_rgba = false;
    std::vector<uint8_t> color;
    std::vector<uint8_t> depth;
};

void ring_consumer_loop(dr::RecorderAppState& app, LatestFrame& latest) {
    while (app.running.load(std::memory_order_relaxed)) {
        WaitForSingleObject(app.ring.event_handle(), 50);

        dr::SharedConfig* cfg = app.config_view.get();
        dr::FrameHeader hdr{};
        while (const uint8_t* payload = app.ring.try_peek(hdr)) {
            if (!hdr.is_preview_only && hdr.color_bytes > 0) {
                std::lock_guard lock(latest.mutex);
                const bool same_geom = (latest.width == hdr.width && latest.height == hdr.height &&
                                        latest.depth_mode == hdr.depth_mode);
                latest.width = hdr.width;
                latest.height = hdr.height;
                latest.depth_mode = hdr.depth_mode;
                latest.color_is_rgba = (hdr.color_is_rgba != 0);
                latest.color.resize(hdr.color_bytes);
                std::memcpy(latest.color.data(), payload, hdr.color_bytes);

                if (hdr.has_depth && hdr.depth_bytes > 0) {
                    latest.depth.resize(hdr.depth_bytes);
                    std::memcpy(latest.depth.data(), payload + hdr.color_bytes, hdr.depth_bytes);
                    latest.has_depth = true;
                } else if (!cfg || cfg->depth_enabled == 0 || !same_geom) {
                    latest.has_depth = false;
                    latest.depth.clear();
                }
                latest.frame_id++;
            }

            if (hdr.has_depth && hdr.depth_bytes > 0 && hdr.width > 0 && hdr.height > 0 &&
                (hdr.is_preview_only || (hdr.frame_index % 6 == 0))) {
                const uint8_t* d = payload + hdr.color_bytes;
                const size_t px_count = static_cast<size_t>(hdr.width) * hdr.height;
                std::lock_guard lock(app.preview_mutex);
                app.preview_w = hdr.width;
                app.preview_h = hdr.height;
                app.preview_bgra.resize(px_count * 4);
                if (hdr.depth_mode == static_cast<uint32_t>(dr::DepthMode::Gray8)) {
                    for (size_t i = 0; i < px_count; ++i) {
                        const uint8_t v = d[i];
                        app.preview_bgra[i * 4 + 0] = v;
                        app.preview_bgra[i * 4 + 1] = v;
                        app.preview_bgra[i * 4 + 2] = v;
                        app.preview_bgra[i * 4 + 3] = 255;
                    }
                } else {
                    for (size_t i = 0; i < px_count; ++i) {
                        app.preview_bgra[i * 4 + 0] = d[i * 4 + 2]; // B
                        app.preview_bgra[i * 4 + 1] = d[i * 4 + 1]; // G
                        app.preview_bgra[i * 4 + 2] = d[i * 4 + 0]; // R
                        app.preview_bgra[i * 4 + 3] = 255;
                    }
                }
            }
            app.ring.release_pop();
        }
    }
}

void writer_loop(dr::RecorderAppState& app, LatestFrame& latest, uint64_t max_frames) {
    LARGE_INTEGER freq_li{};
    QueryPerformanceFrequency(&freq_li);
    const int64_t qpc_freq = freq_li.QuadPart > 0 ? freq_li.QuadPart : 10000000LL;

    uint32_t cur_w = 0, cur_h = 0, cur_mode = 999, cur_depth_en = 999, cur_fps = 0;
    bool cur_rgba = false;
    int64_t next_tick_qpc = 0;
    int64_t fps_win_start_qpc = 0;
    uint64_t fps_win_unique = 0;
    uint64_t last_written_id = 0;

    std::vector<uint8_t> color_buf;
    std::vector<uint8_t> depth_buf;

    while (app.running.load(std::memory_order_relaxed)) {
        dr::SharedConfig* cfg = app.config_view.get();
        const bool want_rec = (cfg && cfg->recording != 0);
        if (!want_rec) {
            if (app.encoder.is_running()) {
                app.encoder.stop();
            }
            if (cfg) InterlockedExchange(&cfg->actual_rec_fps_x10, 0);
            next_tick_qpc = 0;
            fps_win_start_qpc = 0;
            fps_win_unique = 0;
            last_written_id = 0;
            Sleep(10);
            continue;
        }

        uint64_t snap_id = 0;
        uint32_t snap_w = 0, snap_h = 0, snap_mode = 0;
        bool snap_has_depth = false, snap_rgba = false;
        {
            std::lock_guard lock(latest.mutex);
            if (latest.frame_id == 0 || latest.color.empty()) {
                // Wait for first frame
            } else {
                snap_id = latest.frame_id;
                snap_w = latest.width;
                snap_h = latest.height;
                snap_mode = latest.depth_mode;
                snap_has_depth = latest.has_depth;
                snap_rgba = latest.color_is_rgba;
                color_buf = latest.color;
                if (snap_has_depth && !latest.depth.empty()) {
                    depth_buf = latest.depth;
                } else {
                    depth_buf.clear();
                    snap_has_depth = false;
                }
            }
        }

        if (snap_id == 0 || color_buf.empty()) {
            Sleep(2);
            continue;
        }

        const uint32_t target_fps = std::clamp<uint32_t>(app.recording_fps ? app.recording_fps : 60, 1, 240);
        const uint32_t depth_en_u = snap_has_depth ? 1u : 0u;
        if (!app.encoder.is_running() ||
            cur_w != snap_w || cur_h != snap_h ||
            cur_mode != snap_mode || cur_depth_en != depth_en_u ||
            cur_fps != target_fps || cur_rgba != snap_rgba) {
            dr::EncoderSettings es{};
            es.output_dir = app.output_dir;
            es.depth_mode = static_cast<dr::DepthMode>(snap_mode);
            es.depth_enabled = snap_has_depth;
            es.fps = target_fps;
            es.h264_quality = app.h264_quality;
            es.color_is_rgba = snap_rgba;
            es.extra_ffmpeg_flags = app.extra_ffmpeg_flags;
            if (app.encoder.start(snap_w, snap_h, es)) {
                cur_w = snap_w;
                cur_h = snap_h;
                cur_mode = snap_mode;
                cur_depth_en = depth_en_u;
                cur_fps = target_fps;
                cur_rgba = snap_rgba;
            }
        }

        LARGE_INTEGER now_li{};
        QueryPerformanceCounter(&now_li);
        if (next_tick_qpc == 0) {
            next_tick_qpc = now_li.QuadPart;
            fps_win_start_qpc = now_li.QuadPart;
        }

        if (app.encoder.is_running()) {
            app.encoder.write_frame(color_buf.data(), static_cast<uint32_t>(color_buf.size()),
                                    snap_has_depth ? depth_buf.data() : nullptr,
                                    snap_has_depth ? static_cast<uint32_t>(depth_buf.size()) : 0u);
            if (snap_id != last_written_id) {
                fps_win_unique++;
                last_written_id = snap_id;
            }
            QueryPerformanceCounter(&now_li);
            const double win_sec = static_cast<double>(now_li.QuadPart - fps_win_start_qpc) / static_cast<double>(qpc_freq);
            if (win_sec >= 0.25 && cfg) {
                const double actual = static_cast<double>(fps_win_unique) / win_sec;
                InterlockedExchange(&cfg->actual_rec_fps_x10, static_cast<LONG>(actual * 10.0));
                fps_win_start_qpc = now_li.QuadPart;
                fps_win_unique = 0;
            }
        }

        if (max_frames > 0 && app.encoder.frames_written() >= max_frames) {
            if (cfg) InterlockedExchange(&cfg->recording, 0);
            app.encoder.stop();
            app.running.store(false);
            break;
        }

        const int64_t period_qpc = qpc_freq / static_cast<int64_t>(target_fps);
        next_tick_qpc += period_qpc;
        QueryPerformanceCounter(&now_li);
        if (now_li.QuadPart - next_tick_qpc > qpc_freq / 2) {
            next_tick_qpc = now_li.QuadPart;
        }
        while (app.running.load(std::memory_order_relaxed)) {
            QueryPerformanceCounter(&now_li);
            const int64_t rem_qpc = next_tick_qpc - now_li.QuadPart;
            if (rem_qpc <= 0) break;
            const double rem_ms = (static_cast<double>(rem_qpc) * 1000.0) / static_cast<double>(qpc_freq);
            if (rem_ms > 2.0) {
                Sleep(static_cast<DWORD>(rem_ms - 1.0));
            } else {
                YieldProcessor();
            }
        }
    }
    app.encoder.stop();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    dr::RecorderAppState app;
    if (!app.config_view.open_or_create(true) || !app.ring.open_or_create(true)) {
        MessageBoxW(nullptr, L"Failed to initialize shared memory IPC.", L"Depth Recorder", MB_ICONERROR);
        return 1;
    }
    app.ring.reset_indices();

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    app.hook_dll_path = (std::filesystem::path(exe_path).parent_path() / L"depth_hook.dll").wstring();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring cli_launch;
    uint64_t cli_frames = 0;
    bool cli_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--launch" && i + 1 < argc) {
            cli_launch = argv[++i];
            cli_mode = true;
        } else if (arg == L"--out" && i + 1 < argc) {
            app.output_dir = argv[++i];
        } else if (arg == L"--record-frames" && i + 1 < argc) {
            cli_frames = std::wcstoull(argv[++i], nullptr, 10);
        } else if (arg == L"--depth-mode" && i + 1 < argc) {
            std::wstring m = argv[++i];
            auto* cfg = app.config_view.get();
            if (m == L"gray8") cfg->depth_mode = static_cast<LONG>(dr::DepthMode::Gray8);
            else if (m == L"color_phase") cfg->depth_mode = static_cast<LONG>(dr::DepthMode::ColorPhase);
            else cfg->depth_mode = static_cast<LONG>(dr::DepthMode::Rgb24Lossless);
        } else if (arg == L"--no-depth") {
            app.config_view.get()->depth_enabled = 0;
        } else if (arg == L"--no-overlay") {
            app.config_view.get()->overlay_enabled = 0;
        } else if (arg == L"--fps" && i + 1 < argc) {
            const uint32_t f = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            if (f >= 1 && f <= 240) app.recording_fps = f;
        }
    }
    if (argv) LocalFree(argv);

    LatestFrame latest;

    if (cli_mode) {
        if (cli_frames == 0) cli_frames = 60;
        app.config_view.get()->recording = 1;
        std::thread consumer(ring_consumer_loop, std::ref(app), std::ref(latest));
        std::thread writer(writer_loop, std::ref(app), std::ref(latest), cli_frames);
        DWORD pid = 0;
        const bool launched = dr::launch_and_inject(cli_launch, app.hook_dll_path, pid);
        if (!launched) {
            app.running.store(false);
            writer.join();
            consumer.join();
            return 2;
        }
        // Wait up to 20s for N frames to be recorded
        for (int t = 0; t < 400 && app.running.load(); ++t) {
            Sleep(50);
        }
        app.running.store(false);
        writer.join();
        consumer.join();
        if (HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
        return (app.encoder.frames_written() >= cli_frames) ? 0 : 3;
    }

    std::thread consumer(ring_consumer_loop, std::ref(app), std::ref(latest));
    std::thread writer(writer_loop, std::ref(app), std::ref(latest), 0);
    const int rc = dr::run_win32_ui(hInstance, app);
    if (auto* cfg = app.config_view.get()) {
        InterlockedExchange(&cfg->recording, 0);
        InterlockedExchange(&cfg->preview_enabled, 0);
        InterlockedIncrement(&cfg->unload_seq);
        Sleep(150);
    }
    app.running.store(false);
    writer.join();
    consumer.join();
    return rc;
}
