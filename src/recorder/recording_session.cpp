#include "recording_session.h"
#include "timeline.h"

#include <algorithm>
#include <cstring>

namespace dr {

int64_t qpc_now() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t qpc_frequency() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

namespace {

class RecordingTimer {
public:
    RecordingTimer() : timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)) {}
    ~RecordingTimer() { if (timer_) CloseHandle(timer_); }

    void wait_until(int64_t deadline, int64_t now, int64_t frequency) const {
        // Recheck Stop promptly at low target frame rates.
        const int64_t remaining = std::min(deadline - now, frequency / 100);
        if (timer_ && remaining > 0) {
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<int64_t>(1, remaining * 10000000 / frequency);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_, 15);
                return;
            }
        }
        Sleep(1);
    }

private:
    HANDLE timer_;
};

void update_preview(RecorderAppState& app, const FrameHeader& hdr, const uint8_t* depth) {
    const uint32_t step = std::max(1u, (std::max(hdr.width, hdr.height) + 319) / 320);
    const uint32_t width = (hdr.width + step - 1) / step;
    const uint32_t height = (hdr.height + step - 1) / step;
    const bool gray = hdr.depth_mode == static_cast<uint32_t>(DepthMode::Gray8);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t source = static_cast<size_t>(y * step) * hdr.width + x * step;
            const auto* d = depth + source * (gray ? 1 : 4);
            auto* p = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            p[0] = gray ? d[0] : d[2];
            p[1] = gray ? d[0] : d[1];
            p[2] = d[0];
            p[3] = 255;
        }
    }
    std::lock_guard lock(app.preview_mutex);
    if (!app.show_preview || app.config_view.get()->capture_generation != hdr.capture_generation) return;
    app.preview_w = width;
    app.preview_h = height;
    app.preview_source_w = hdr.width;
    app.preview_source_h = hdr.height;
    app.preview_bgra.swap(pixels);
}

bool accept_preview(RecorderAppState& app, SharedConfig& cfg, const FrameHeader& hdr) {
    std::lock_guard lock(app.recording.mutex);
    auto& profile = app.recording.preview_profile;
    const bool same = profile.color_bytes && same_capture_format(profile, hdr);
    if (hdr.color_bytes) {
        if (!same) {
            app.encoder_ready_generation.store(-1);
            app.encoder_failed_generation.store(-1);
        }
        profile = hdr;
        InterlockedExchange(&cfg.preview_pair_generation, hdr.capture_generation);
        app.log.write("REC", "Preview ready: %ux%u color=%s depth=%u mode=%u gen=%ld",
                      hdr.width, hdr.height, hdr.color_is_rgba ? "RGBA" : "BGRA",
                      hdr.has_depth, hdr.depth_mode, hdr.capture_generation);
        return true;
    }
    if (same) return true;
    // A depth-only preview from a new format needs one fresh full pair.
    app.ready_generation.store(-1);
    app.encoder_ready_generation.store(-1);
    app.encoder_failed_generation.store(-1);
    profile = {};
    InterlockedExchange(&cfg.preview_pair_generation, -1);
    return false;
}

struct RecordingRequest {
    EncoderSettings settings;
    LONG generation;
    HWND window;
    int64_t start;
    bool drop_dead_frames;
};

void write_recording(RecorderAppState& app, FrameInbox& inbox, const RecordingRequest& request, uint64_t max_frames) {
    auto* cfg = app.config_view.get();
    const int64_t frequency = qpc_frequency();
    const int64_t readback_delay = std::clamp<int64_t>(2 * frequency / request.settings.fps, frequency / 80, frequency / 20);
    RecordingTimer timer;
    auto stop_time = [&] {
        std::lock_guard lock(app.recording.mutex);
        return app.recording.stop_requested;
    };
    auto enter_saving = [&] {
        std::lock_guard lock(app.recording.mutex);
        app.recording.status = RecordingStatus::Saving;
        InterlockedExchange(&cfg->recording, 0);
    };

    auto take_first = [&](int64_t since) -> std::shared_ptr<const FramePacket> {
        while (true) {
            const int64_t now = qpc_now();
            const int64_t stop = stop_time();
            if (auto pair = inbox.take(request.generation, since, stop ? stop : now)) return pair;
            if ((stop && now >= stop + readback_delay) || !app.running ||
                now - since >= 5 * frequency || (request.window && !IsWindow(request.window))) return {};
            Sleep(2);
        }
    };

    auto frame = take_first(request.start);
    if (!frame) {
        finish_recording(app, inbox, false, L"No valid frame pairs were captured.");
        return;
    }

    const auto format = frame->header;
    auto settings = request.settings;
    settings.depth_mode = static_cast<DepthMode>(format.depth_mode);
    settings.color_is_rgba = format.color_is_rgba != 0;
    if (!app.discard_output) {
        if (!app.encoder.start(format.width, format.height, settings)) {
            finish_recording(app, inbox, false, app.encoder.error());
            return;
        }
    }
    const auto first = frame->header;
    RecordingTimeline timeline(first.qpc_timestamp, frequency, request.settings.fps);
    {
        std::lock_guard lock(app.recording.mutex);
        if (app.recording.status == RecordingStatus::Starting) app.recording.status = RecordingStatus::Recording;
    }

    uint64_t slot = 0;
    uint64_t written = 0;
    FrameFitter fitter(first);
    int64_t last_written = 0;
    app.clip_started_qpc.store(first.qpc_timestamp);
    std::wstring reason;
    bool ok = true;

    while (true) {
        const int64_t now = qpc_now();
        int64_t stop = stop_time();
        if (!stop && !app.running) stop = now;
        if (request.drop_dead_frames && slot &&
            now - timeline.sample_time(slot + 1) > readback_delay + frequency / 4) {
            // Real-time mode writes every slot. Drop mode can catch up by
            // jumping to the most recent slot whose readback window has ended.
            const int64_t through = now - readback_delay;
            uint64_t boundary = timeline.frame_count(through);
            if (timeline.sample_time(boundary) > through) --boundary;
            if (stop) boundary = std::min(boundary, timeline.frame_count(stop));
            if (max_frames) boundary = std::min(boundary, max_frames);
            if (boundary > slot + 1) {
                slot = boundary - 1;
            }
        }
        // Short backpressure can catch up through the timeline. Past this bound,
        // the encoder cannot keep up with real-time recording.
        if (!request.drop_dead_frames && slot > 0 && now - timeline.sample_time(slot) > frequency * 5 / 2 + readback_delay) {
            ok = false;
            reason = L"The encoder could not keep up with real-time recording.";
            break;
        }
        if (stop) {
            enter_saving();
            if (slot >= timeline.frame_count(stop)) break;
        }
        if (max_frames && slot >= max_frames) break;

        const int64_t sample = timeline.sample_time(slot);
        const int64_t next = timeline.sample_time(slot + 1);
        const int64_t due = next + readback_delay;
        if (now < due) {
            timer.wait_until(due, now, frequency);
            continue;
        }

        if (slot > 0) {
            // Keep a nearby second arrival for the next tick. Readbacks sometimes
            // arrive in bursts, even when the hook is sampling at the output rate.
            // During a slowdown, use the freshest late pair if nothing newer is ready.
            const int64_t cutoff = sample + (next - sample) / 2;
            const int64_t since = timeline.sample_time(slot - 1);
            auto newer = inbox.take(request.generation, since, stop ? std::min(cutoff, stop) : cutoff, true);
            if (newer && newer->header.qpc_timestamp > frame->header.qpc_timestamp) {
                const auto& hdr = newer->header;
                if (hdr.depth_mode == first.depth_mode && hdr.has_depth == first.has_depth) {
                    if (!app.discard_output && (hdr.width != first.width || hdr.height != first.height ||
                                                hdr.color_is_rgba != first.color_is_rgba)) {
                        fitter.fit(*newer);
                    }
                    frame = std::move(newer);
                }
            }
        }
        ++slot;
        const bool fresh = last_written != frame->header.qpc_timestamp;
        if (request.drop_dead_frames && !fresh) continue;
        const auto& output = fitter.for_frame(*frame);
        if (!app.discard_output) {
            app.encoder_write_qpc.store(qpc_now());
            ok = app.encoder.write_frame(output.color.data(), output.header.color_bytes,
                                         output.depth.data(), output.header.depth_bytes);
            app.encoder_write_qpc.store(0);
            if (!ok) break;
        }
        app.written_pairs.store(++written);
        if (fresh) {
            last_written = frame->header.qpc_timestamp;
            app.unique_written_pairs.fetch_add(1, std::memory_order_relaxed);
        }
    }
    app.clip_started_qpc.store(0);
    InterlockedExchange(&cfg->recording_activity, static_cast<LONG>(RecordingActivity::Normal));
    enter_saving();
    if (!app.discard_output) ok = app.encoder.stop(ok, reason) && ok;
    InterlockedExchange(&cfg->actual_rec_fps_x10, 0);
    finish_recording(app, inbox, ok, app.discard_output ? reason : app.encoder.error(),
                     app.discard_output ? L"" : app.encoder.last_folder());
}

void prepare_selected_window(RecorderAppState& app) {
    auto* cfg = app.config_view.get();
    const LONG generation = cfg->capture_generation;
    if (app.ready_generation != generation || app.encoder_ready_generation == generation ||
        app.encoder_failed_generation == generation || app.discard_output) return;

    FrameHeader profile{};
    {
        std::lock_guard lock(app.recording.mutex);
        profile = app.recording.preview_profile;
    }
    if (!profile.color_bytes || profile.capture_generation != generation) return;
    EncoderSettings settings;
    settings.fps = std::clamp<uint32_t>(static_cast<uint32_t>(cfg->target_rec_fps), 1, 240);
    settings.h264_quality = app.h264_quality;
    settings.depth_enabled = cfg->depth_enabled != 0;
    settings.depth_mode = static_cast<DepthMode>(profile.depth_mode);
    settings.color_is_rgba = profile.color_is_rgba != 0;
    settings.software_only = app.software_encoder;
    settings.extra_ffmpeg_flags = app.extra_ffmpeg_flags;
    const bool ready = app.encoder.prepare(profile.width, profile.height, settings);
    if (ready) {
        app.log.write("ENC", "Encoder prepared: %ux%u @ %u fps (depth=%u mode=%u)",
                      profile.width, profile.height, settings.fps, settings.depth_enabled ? 1u : 0u,
                      static_cast<uint32_t>(settings.depth_mode));
    } else {
        app.log.write_w("ENC", L"Encoder prepare failed: " + app.encoder.error());
    }
    {
        std::lock_guard lock(app.recording.mutex);
        if (cfg->capture_generation == generation && app.recording.preview_profile.color_bytes &&
            same_capture_format(app.recording.preview_profile, profile)) {
            (ready ? app.encoder_ready_generation : app.encoder_failed_generation).store(generation);
        }
    }
}

} // namespace

void ring_consumer_loop(RecorderAppState& app, FrameInbox& inbox) {
    auto* cfg = app.config_view.get();
    const auto frequency = qpc_frequency();
    int64_t last_sample = qpc_now();
    LONG previous_presents = 0;
    uint64_t previous_unique = 0;
    LONG previous_generation = -1;
    while (app.running.load(std::memory_order_relaxed)) {
        WaitForSingleObject(app.ring.event_handle(), 30);
        FrameHeader hdr{};
        while (const uint8_t* payload = app.ring.try_peek(hdr)) {
            const uint64_t pixels = static_cast<uint64_t>(hdr.width) * hdr.height;
            const uint64_t total = static_cast<uint64_t>(hdr.color_bytes) + hdr.depth_bytes + sizeof(hdr);
            const bool gray = hdr.depth_mode == static_cast<uint32_t>(DepthMode::Gray8);
            if (cfg->target_pid > 0 && hdr.producer_pid == static_cast<uint32_t>(cfg->target_pid) &&
                hdr.capture_generation == cfg->capture_generation && hdr.qpc_timestamp > 0 &&
                pixels && pixels <= kMaxSlotPayloadBytes / 4 &&
                total <= kMaxSlotPayloadBytes &&
                (hdr.color_bytes == 4 * pixels || (hdr.is_preview_only && hdr.color_bytes == 0)) &&
                hdr.depth_mode <= static_cast<uint32_t>(DepthMode::Rgb24Lossless) &&
                (hdr.has_depth ? hdr.depth_bytes == pixels * (gray ? 1 : 4) : hdr.depth_bytes == 0)) {
                if ((hdr.has_depth || !cfg->depth_enabled) &&
                    (!hdr.is_preview_only || accept_preview(app, *cfg, hdr))) {
                    app.ready_qpc.store(app.ready_generation == hdr.capture_generation ?
                                        std::max(app.ready_qpc.load(), hdr.qpc_timestamp) : hdr.qpc_timestamp);
                    app.ready_generation.store(hdr.capture_generation);
                    if (!hdr.is_preview_only) {
                        app.received_pairs.fetch_add(1, std::memory_order_relaxed);
                        if (auto packet = inbox.acquire(hdr)) {
                            packet->header = hdr;
                            packet->color.assign(payload, payload + hdr.color_bytes);
                            packet->depth.assign(payload + hdr.color_bytes, payload + hdr.color_bytes + hdr.depth_bytes);
                            inbox.push(std::move(packet));
                        } else {
                            app.inbox_full_pairs.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                if (app.show_preview && hdr.has_depth && (hdr.is_preview_only || hdr.frame_index % 6 == 0)) {
                    update_preview(app, hdr, payload + hdr.color_bytes);
                }
            }
            app.ring.release_pop();
        }
        app.log.drain_shared(cfg);
        const auto now = qpc_now();
        const int64_t began = app.clip_started_qpc.load();
        const int64_t writing = app.encoder_write_qpc.load();
        const LONG generation = cfg->capture_generation;
        if (began && cfg->recording) {
            auto activity = RecordingActivity::Normal;
            if (writing && now - writing >= frequency / 2) {
                activity = RecordingActivity::EncoderStalled;
            } else if (now - std::max(began, app.ready_qpc.load()) >= frequency / 2) {
                activity = RecordingActivity::WaitingForCapture;
            }
            InterlockedExchange(&cfg->recording_activity, static_cast<LONG>(activity));
        }
        if (generation != previous_generation || now - last_sample >= frequency / 2) {
            const LONG presents = cfg->presented_frames;
            const uint64_t unique = app.unique_written_pairs.load();
            if (began && cfg->recording && generation == previous_generation && now > last_sample &&
                presents >= previous_presents && unique >= previous_unique) {
                const double to_fps = 10.0 * frequency / (now - last_sample);
                app.present_fps_x10.store(static_cast<LONG>((presents - previous_presents) * to_fps));
                InterlockedExchange(&cfg->actual_rec_fps_x10, static_cast<LONG>((unique - previous_unique) * to_fps));
            }
            previous_presents = presents;
            previous_unique = unique;
            previous_generation = generation;
            last_sample = now;
        }
    }
}

void finish_recording(RecorderAppState& app, FrameInbox& inbox, bool success,
                      std::wstring message, std::wstring folder) {
    InterlockedExchange(&app.config_view.get()->recording, 0);
    // Keep the current clip active until its queued frames have been released.
    inbox.clear();
    std::lock_guard lock(app.recording.mutex);
    app.recording.status = success ? RecordingStatus::Saved : RecordingStatus::Failed;
    app.recording.message = std::move(message);
    app.recording.folder = std::move(folder);
    app.log.write_w("REC", std::wstring(success ? L"Recording saved: " : L"Recording failed: ") +
                    (app.recording.folder.empty() ? app.recording.message : app.recording.folder));
}

void writer_loop(RecorderAppState& app, FrameInbox& inbox, uint64_t max_frames) {
    while (true) {
        RecordingRequest request{};
        {
            std::lock_guard lock(app.recording.mutex);
            if (app.recording.status == RecordingStatus::Starting ||
                app.recording.status == RecordingStatus::Saving) {
                request = {app.recording.settings, app.recording.generation, app.recording.window,
                           app.recording.start_requested, app.recording.drop_dead_frames};
            }
        }
        if (request.start) {
            write_recording(app, inbox, request, max_frames);
            if (max_frames) {
                app.running.store(false);
                break;
            }
        }
        if (!app.running) break;
        if (!request.start) prepare_selected_window(app);
        Sleep(5);
    }
}

bool start_recording(RecorderAppState& app, bool require_ready) {
    auto* cfg = app.config_view.get();
    const auto now = qpc_now();
    std::lock_guard lock(app.recording.mutex);
    auto& state = app.recording;
    if (state.status == RecordingStatus::Starting || state.status == RecordingStatus::Recording ||
        state.status == RecordingStatus::Saving) return false;
    if (require_ready && (app.ready_generation != cfg->capture_generation ||
                          app.encoder_ready_generation != cfg->capture_generation ||
                          now - app.ready_qpc >= qpc_frequency())) return false;

    state.settings = {app.output_dir, static_cast<DepthMode>(cfg->depth_mode), cfg->depth_enabled != 0,
                      app.recording_fps, app.h264_quality, false, app.software_encoder, app.extra_ffmpeg_flags};
    state.drop_dead_frames = app.drop_dead_frames;
    state.window = nullptr;
    const LONG index = cfg->selected_swapchain_index;
    SwapchainInfo sc_entry{};
    if (read_swapchain_info(cfg, index, sc_entry)) {
        state.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(sc_entry.window));
    }
    state.start_requested = now;
    state.stop_requested = 0;
    state.message.clear();
    state.folder.clear();
    state.generation = InterlockedIncrement(&cfg->capture_generation);
    state.status = RecordingStatus::Starting;
    app.received_pairs.store(0);
    app.inbox_full_pairs.store(0);
    app.written_pairs.store(0);
    app.unique_written_pairs.store(0);
    app.present_fps_x10.store(0);
    InterlockedExchange(&cfg->recording_activity, 0);
    InterlockedExchange(&cfg->published_pairs, 0);
    InterlockedExchange(&cfg->presented_frames, 0);
    InterlockedExchange(&cfg->recording, 1);
    return true;
}

void stop_recording(RecorderAppState& app) {
    std::lock_guard lock(app.recording.mutex);
    if (app.recording.status == RecordingStatus::Starting || app.recording.status == RecordingStatus::Recording) {
        app.recording.stop_requested = qpc_now();
        app.recording.status = RecordingStatus::Saving;
        InterlockedExchange(&app.config_view.get()->recording, 0);
    }
}

} // namespace dr
