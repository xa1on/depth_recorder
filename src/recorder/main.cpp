#include "recording_session.h"
#include "injector.h"
#include "ui_win32.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <shellapi.h>
#include <string>
#include <thread>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    struct HandleCloser {
        void operator()(void* handle) const { if (handle) CloseHandle(handle); }
    };
    const std::unique_ptr<void, HandleCloser> instance(CreateMutexW(nullptr, TRUE, LR"(Local\DepthRecorderInstance)"));
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Depth Recorder is already open.", L"Depth Recorder", MB_ICONERROR);
        return 1;
    }

    dr::RecorderAppState app;
    if (!app.config_view.open_or_create(true) || !app.ring.open_or_create(true)) {
        MessageBoxW(nullptr, L"Failed to initialize shared memory IPC.", L"Depth Recorder", MB_ICONERROR);
        return 1;
    }
    *app.config_view.get() = dr::SharedConfig{};
    app.config_view.get()->target_pid = -1;
    app.ring.reset_indices();

    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    app.hook_dll_path = (exe_dir / L"depth_hook.dll").wstring();
    app.output_dir = (exe_dir / L"recordings").wstring();
    app.log.init((exe_dir / L"depth_recorder.log").wstring());

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring cli_launch;
    uint64_t cli_frames = 0;
    DWORD attach_pid = 0;
    bool warmup_encoder = false;
    bool cli_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--launch" && i + 1 < argc) {
            cli_launch = argv[++i];
            cli_mode = true;
        } else if (arg == L"--attach" && i + 1 < argc) {
            attach_pid = std::wcstoul(argv[++i], nullptr, 10);
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
        } else if (arg == L"--discard") {
            app.discard_output = true;
        } else if (arg == L"--software-encoder") {
            app.software_encoder = true;
        } else if (arg == L"--warmup-encoder") {
            warmup_encoder = true;
        } else if (arg == L"--no-depth") {
            app.config_view.get()->depth_enabled = 0;
        } else if (arg == L"--no-linearize") {
            app.config_view.get()->linearize = 0;
        } else if (arg == L"--overlay") {
            app.config_view.get()->overlay_enabled = 1;
        } else if (arg == L"--no-overlay") {
            app.config_view.get()->overlay_enabled = 0;
        } else if (arg == L"--drop-dead-frames") {
            app.drop_dead_frames = true;
        } else if (arg == L"--extra-ffmpeg" && i + 1 < argc) {
            app.extra_ffmpeg_flags = argv[++i];
        } else if (arg == L"--fps" && i + 1 < argc) {
            const uint32_t f = static_cast<uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            if (f >= 1 && f <= 240) {
                app.recording_fps = f;
                app.config_view.get()->target_rec_fps = static_cast<LONG>(f);
            }
        }
    }
    if (argv) LocalFree(argv);

    dr::FrameInbox inbox;

    if (cli_mode) {
        auto* cfg = app.config_view.get();
        if (cli_frames == 0) cli_frames = 60;
        warmup_encoder = warmup_encoder && !app.discard_output;
        ULONGLONG warmup_ms = 0;
        DWORD pid = 0;
        if (attach_pid) {
            InterlockedExchange(&cfg->target_pid, static_cast<LONG>(attach_pid));
            InterlockedExchange(&cfg->selected_swapchain_index, -1);
            cfg->preview_enabled = warmup_encoder ? 1 : 0;
            if (!dr::inject_dll(attach_pid, app.hook_dll_path)) return 2;
            const ULONGLONG deadline = GetTickCount64() + 5000;
            while (!cfg->swapchain_count && GetTickCount64() < deadline) Sleep(20);
            Sleep(300);
            const LONG selected = dr::largest_swapchain_index(*cfg);
            if (selected < 0) {
                InterlockedExchange(&cfg->target_pid, -1);
                InterlockedIncrement(&cfg->unload_seq);
                return 2;
            }
            InterlockedExchange(&cfg->selected_swapchain_index, selected);
        } else {
            if (cli_launch.empty()) return 2;
            InterlockedExchange(&cfg->target_pid, 0);
        }

        if (!warmup_encoder) dr::start_recording(app, false);
        std::thread consumer(dr::ring_consumer_loop, std::ref(app), std::ref(inbox));
        std::thread writer(dr::writer_loop, std::ref(app), std::ref(inbox), cli_frames);
        if (!attach_pid && !dr::launch_and_inject(cli_launch, app.hook_dll_path, pid)) {
            app.running.store(false);
            writer.join();
            consumer.join();
            return 2;
        }
        bool started = !warmup_encoder;
        if (warmup_encoder) {
            const auto began = GetTickCount64();
            while (GetTickCount64() - began < 20000 && app.encoder_failed_generation != cfg->capture_generation) {
                if ((started = dr::start_recording(app, true))) break;
                Sleep(20);
            }
            warmup_ms = GetTickCount64() - began;
            if (!started) dr::finish_recording(app, inbox, false, L"The target did not supply a usable profile during encoder warmup.");
        }
        const uint64_t timeout_ticks = 400 + (cli_frames / app.recording_fps + 1) * 20;
        for (uint64_t t = 0; started && t < timeout_ticks && app.running.load(); ++t) Sleep(50);
        dr::stop_recording(app);
        app.running.store(false);
        writer.join();
        consumer.join();

        if (const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE); output && output != INVALID_HANDLE_VALUE) {
            char stats[512]{};
            const LONG selected = cfg->selected_swapchain_index;
            dr::SwapchainInfo viewport{};
            dr::read_swapchain_info(cfg, selected, viewport);
            const int length = snprintf(stats, sizeof(stats),
                "backend=%ld viewport=%ux%u candidates=%ld dropped=%ld present_fps=%.1f presents=%ld published=%ld received=%llu written=%llu unique=%llu warmup_ms=%llu inbox_full=%llu\n",
                cfg->active_backend, viewport.width, viewport.height, cfg->candidate_depth_count, cfg->dropped_frames,
                cfg->current_fps_x10 / 10.0, cfg->presented_frames, cfg->published_pairs,
                static_cast<unsigned long long>(app.received_pairs.load()),
                static_cast<unsigned long long>(app.written_pairs.load()),
                static_cast<unsigned long long>(app.unique_written_pairs.load()),
                static_cast<unsigned long long>(warmup_ms),
                static_cast<unsigned long long>(app.inbox_full_pairs.load()));
            DWORD written = 0;
            if (length > 0 && static_cast<size_t>(length) < sizeof(stats)) WriteFile(output, stats, static_cast<DWORD>(length), &written, nullptr);
            if (!app.recording.message.empty()) {
                const std::wstring message = app.recording.message + L"\n";
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                                                      nullptr, 0, nullptr, nullptr);
                if (bytes > 0) {
                    std::string utf8(static_cast<size_t>(bytes), '\0');
                    if (WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                                           utf8.data(), bytes, nullptr, nullptr)) {
                        WriteFile(output, utf8.data(), static_cast<DWORD>(bytes), &written, nullptr);
                    }
                }
            }
        }
        if (attach_pid) {
            InterlockedExchange(&cfg->target_pid, -1);
            InterlockedIncrement(&cfg->unload_seq);
            Sleep(300);
        } else if (HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
        return (app.recording.status == dr::RecordingStatus::Saved &&
                (app.drop_dead_frames || app.written_pairs >= cli_frames)) ? 0 : 3;
    }

    std::thread consumer(dr::ring_consumer_loop, std::ref(app), std::ref(inbox));
    std::thread writer(dr::writer_loop, std::ref(app), std::ref(inbox), 0);
    const int rc = dr::run_win32_ui(hInstance, app);
    dr::stop_recording(app);
    if (auto* cfg = app.config_view.get()) {
        InterlockedExchange(&cfg->recording, 0);
        InterlockedExchange(&cfg->preview_enabled, 0);
    }
    app.running.store(false);
    writer.join();
    consumer.join();
    if (auto* cfg = app.config_view.get()) {
        InterlockedIncrement(&cfg->unload_seq);
        Sleep(150);
    }
    return rc;
}
