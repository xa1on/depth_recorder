#include "ui_win32.h"
#include "injector.h"
#include <filesystem>
#include <shellapi.h>
#include <thread>

namespace {

void consumer_loop(dr::RecorderAppState& app, uint64_t max_frames) {
    uint32_t cur_w = 0, cur_h = 0, cur_mode = 999, cur_depth_en = 999;
    while (app.running.load(std::memory_order_relaxed)) {
        WaitForSingleObject(app.ring.event_handle(), 50);

        dr::SharedConfig* cfg = app.config_view.get();
        const bool want_rec = (cfg && cfg->recording != 0);

        dr::FrameHeader hdr{};
        while (const uint8_t* payload = app.ring.try_peek(hdr)) {
            if (want_rec) {
                if (!app.encoder.is_running() ||
                    cur_w != hdr.width || cur_h != hdr.height ||
                    cur_mode != hdr.depth_mode || cur_depth_en != hdr.has_depth) {
                    dr::EncoderSettings es{};
                    es.output_dir = app.output_dir;
                    es.depth_mode = static_cast<dr::DepthMode>(hdr.depth_mode);
                    es.depth_enabled = (hdr.has_depth != 0);
                    es.fps = 60;
                    if (app.encoder.start(hdr.width, hdr.height, es)) {
                        cur_w = hdr.width;
                        cur_h = hdr.height;
                        cur_mode = hdr.depth_mode;
                        cur_depth_en = hdr.has_depth;
                    }
                }
                if (app.encoder.is_running()) {
                    const uint8_t* color_ptr = payload;
                    const uint8_t* depth_ptr = hdr.has_depth ? (payload + hdr.color_bytes) : nullptr;
                    app.encoder.write_frame(color_ptr, hdr.color_bytes, depth_ptr, hdr.depth_bytes);
                }
            } else if (app.encoder.is_running()) {
                app.encoder.stop();
            }
            app.ring.release_pop();

            if (max_frames > 0 && app.encoder.frames_written() >= max_frames) {
                if (cfg) InterlockedExchange(&cfg->recording, 0);
                app.encoder.stop();
                app.running.store(false);
                break;
            }
        }

        if (!want_rec && app.encoder.is_running()) {
            app.encoder.stop();
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
        }
    }
    if (argv) LocalFree(argv);

    if (cli_mode) {
        if (cli_frames == 0) cli_frames = 60;
        app.config_view.get()->recording = 1;
        std::thread consumer(consumer_loop, std::ref(app), cli_frames);
        DWORD pid = 0;
        const bool launched = dr::launch_and_inject(cli_launch, app.hook_dll_path, pid);
        if (!launched) {
            app.running.store(false);
            consumer.join();
            return 2;
        }
        // Wait up to 20s for N frames to be recorded
        for (int t = 0; t < 400 && app.running.load(); ++t) {
            Sleep(50);
        }
        app.running.store(false);
        consumer.join();
        if (HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
        return (app.encoder.frames_written() >= cli_frames) ? 0 : 3;
    }

    std::thread consumer(consumer_loop, std::ref(app), 0);
    const int rc = dr::run_win32_ui(hInstance, app);
    app.running.store(false);
    consumer.join();
    return rc;
}
