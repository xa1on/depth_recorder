#include "common/depth_math.h"
#include "common/frame_extent.h"
#include "common/ipc_ring.h"
#include "hook/slot_ring.h"
#include "recorder/encoder.h"
#include "recorder/frame_inbox.h"
#include "recorder/logger.h"
#include "recorder/timeline.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

auto read_frames(const std::wstring& ffmpeg, const std::wstring& video, const std::filesystem::path& raw) {
    std::wstring command = L"\"" + ffmpeg + L"\" -nostdin -y -loglevel error -i \"" + video +
                           L"\" -f rawvideo -pix_fmt rgb24 \"" + raw.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    CHECK(CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child));
    CloseHandle(child.hThread);
    DWORD exit_code = 1;
    if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 1000);
    }
    GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hProcess);
    CHECK(exit_code == 0);
    std::ifstream input(raw, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void test_timing() {
    constexpr int64_t qpc_frequency = 10000019;
    constexpr int64_t start = 89123;
    for (const uint32_t fps : {24u, 30u, 60u, 120u, 240u}) {
        const dr::RecordingTimeline timeline(start, qpc_frequency, fps);
        CHECK(timeline.sample_time(0) == start);
        CHECK(timeline.sample_time(fps * 10) == start + qpc_frequency * 10);
        CHECK(timeline.frame_count(start + qpc_frequency * 10) == fps * 10);
        CHECK(timeline.frame_count(start + qpc_frequency * 10 + qpc_frequency / (3 * fps)) == fps * 10);
        CHECK(timeline.frame_count(start + qpc_frequency * 10 + qpc_frequency * 2 / (3 * fps)) == fps * 10 + 1);
    }
    for (int i = 0; i <= 1000; ++i) {
        const float depth = i / 1000.0f;
        uint8_t r{}, g{}, b{};
        dr::encode_rgb24(depth, r, g, b);
        CHECK(std::fabs(depth - dr::decode_rgb24(r, g, b)) < 1e-6f);
    }
}

void test_ipc_ring() {
    dr::IpcRing ring;
    CHECK(ring.open_or_create(true));
    ring.reset_indices();

    dr::FrameHeader peeked{};
    CHECK(ring.try_peek(peeked) == nullptr);

    for (uint64_t i = 0; i < dr::kRingSlots; ++i) {
        uint8_t* dst = ring.begin_push(8);
        CHECK(dst != nullptr);
        dst[0] = static_cast<uint8_t>(i + 1);
        dst[7] = static_cast<uint8_t>(i + 10);
        dr::FrameHeader hdr{};
        hdr.frame_index = i;
        hdr.width = 2;
        hdr.height = 1;
        hdr.color_bytes = 8;
        ring.commit_push(hdr);
    }
    CHECK(ring.begin_push(8) == nullptr);

    for (uint64_t i = 0; i < dr::kRingSlots; ++i) {
        const uint8_t* src = ring.try_peek(peeked);
        CHECK(src != nullptr);
        CHECK(peeked.frame_index == i);
        CHECK(src[0] == static_cast<uint8_t>(i + 1));
        CHECK(src[7] == static_cast<uint8_t>(i + 10));
        ring.release_pop();
    }
    CHECK(ring.try_peek(peeked) == nullptr);
    ring.close();
}

void test_slot_tracker() {
    dr::CaptureSlotTracker<3> tracker;
    CHECK(tracker.acquire_free_slot() == 0);
    tracker.mark_queued(0, 300, 1, false, 0, true, false, 0, false);
    CHECK(tracker.acquire_free_slot() == 1);
    tracker.mark_queued(1, 100, 1, false, 0, true, false, 0, false);
    CHECK(tracker.acquire_free_slot() == 2);
    tracker.mark_queued(2, 200, 0, false, 0, true, false, 0, false); // stale gen
    CHECK(tracker.acquire_free_slot() == 3);

    // pick_oldest_pending cancels stale gen 2 and returns slot 1 (qpc=100) before slot 0 (qpc=300)
    CHECK(tracker.pick_oldest_pending(1) == 1);
    CHECK(!tracker.slots[2].pending);
    tracker.slots[1].pending = false;
    CHECK(tracker.pick_oldest_pending(1) == 0);
    tracker.reset_all();
    CHECK(tracker.acquire_free_slot() == 0);
}

void test_frame_inbox_and_fitter() {
    dr::FrameInbox inbox;
    dr::FrameHeader h1{};
    h1.width = 4;
    h1.height = 4;
    h1.color_bytes = 64;
    h1.depth_bytes = 16;
    h1.has_depth = 1;
    h1.capture_generation = 2;
    h1.qpc_timestamp = 200;

    auto p1 = inbox.acquire(h1);
    CHECK(p1 != nullptr);
    p1->header = h1;
    p1->color.assign(64, 0xAA);
    p1->depth.assign(16, 0x11);
    inbox.push(p1);

    auto p2 = inbox.acquire(h1);
    CHECK(p2 != nullptr);
    p2->header = h1;
    p2->header.qpc_timestamp = 100;
    p2->color.assign(64, 0xBB);
    p2->depth.assign(16, 0x22);
    inbox.push(p2);

    // Ordered by QPC even though pushed out of order
    auto out1 = inbox.take(2, 50, 150);
    CHECK(out1 && out1->header.qpc_timestamp == 100);
    auto out2 = inbox.take(2, 150, 250);
    CHECK(out2 && out2->header.qpc_timestamp == 200);

    // FrameFitter RGBA -> BGRA swizzle + extent resize
    dr::FrameFitter fitter(h1);
    dr::FramePacket src{};
    src.header = h1;
    src.header.width = 2;
    src.header.height = 2;
    src.header.color_bytes = 16;
    src.header.depth_bytes = 4;
    src.header.color_is_rgba = 1;
    src.color = {10, 20, 30, 40,  10, 20, 30, 40,  10, 20, 30, 40,  10, 20, 30, 40};
    src.depth = {7, 7, 7, 7};
    fitter.fit(src);
    const auto& fitted = fitter.for_frame(src);
    CHECK(fitted.header.width == 4 && fitted.header.height == 4);
    CHECK(fitted.color[0] == 30 && fitted.color[1] == 20 && fitted.color[2] == 10 && fitted.color[3] == 40);
    CHECK(fitted.depth[0] == 7);
}

void test_shared_log_ring() {
    dr::SharedConfig cfg{};
    dr::log_push(&cfg, "VK", "hello %d", 42);
    std::string entry;
    CHECK(dr::read_log_entry(&cfg, 1, entry));
    CHECK(entry == "[VK] hello 42");
    CHECK(!dr::read_log_entry(&cfg, 2, entry));

    dr::RecorderLog log;
    log.drain_shared(&cfg);
    uint64_t ver = 0;
    std::wstring snap;
    CHECK(log.snapshot(ver, snap));
    CHECK(snap.find(L"[VK] hello 42") != std::wstring::npos);
    CHECK(!log.snapshot(ver, snap));
}

void test_target_selection() {
    const auto make_window = [] {
        return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    };
    const HWND primary = make_window();
    const HWND panel = make_window();
    const HWND replacement = make_window();
    CHECK(primary && panel && replacement);
    dr::SharedConfig config;
    CHECK(dr::register_and_is_selected_swapchain(&config, 0x1000, 1920, 1080, primary));
    const auto selected = dr::swapchain_target(config.swapchains[0]);
    CHECK(dr::largest_swapchain_index(config) == 0);

    // An allocator can reuse a swapchain pointer for a different window.
    CHECK(!dr::register_and_is_selected_swapchain(&config, 0x1000, 384, 256, panel));
    CHECK(config.swapchain_count == 2);
    CHECK(dr::largest_swapchain_index(config) == 0);
    CHECK(!dr::register_and_is_selected_swapchain(&config, 0x1000, 2048, 1080, panel));
    CHECK(dr::largest_swapchain_index(config) == 1);
    CHECK(!dr::register_and_is_selected_swapchain(&config, 0x1000, 384, 256, panel));

    config.swapchains[0].last_present_ms = GetTickCount64() - 501;
    CHECK(dr::register_and_is_selected_swapchain(&config, 0x2000, 1600, 900, primary));
    CHECK(dr::swapchain_target(config.swapchains[0]) == selected);

    CHECK(DestroyWindow(primary));
    CHECK(!dr::register_and_is_selected_swapchain(&config, 0x2000, 512, 256, replacement));
    CHECK(config.swapchain_count == 3);
    CHECK(dr::swapchain_target(config.swapchains[0]) == selected);
    CHECK(dr::largest_swapchain_index(config) == 2);
    CHECK(DestroyWindow(panel));
    CHECK(DestroyWindow(replacement));
}

void test_preview_extent() {
    const auto landscape = dr::fit_frame(2200, 1238, 240, 240);
    CHECK(landscape.width == 240 && landscape.height == 135 && landscape.left == 0 && landscape.top == 52);
    const auto portrait = dr::fit_frame(1080, 1920, 240, 240);
    CHECK(portrait.width == 135 && portrait.height == 240 && portrait.left == 52 && portrait.top == 0);
}

void test_encoder(const std::wstring& ffmpeg, const std::filesystem::path& directory) {
    constexpr uint32_t width = 128, height = 72, pixels = width * height;
    dr::EncoderSettings settings{directory.wstring(), dr::DepthMode::Rgb24Lossless, true, 60};
    settings.software_only = true;
    dr::VideoEncoder encoder;
    CHECK(encoder.prepare(width, height, settings));
    CHECK(std::filesystem::is_empty(directory));
    CHECK(encoder.start(width, height, settings));
    std::vector<uint8_t> color(pixels * 4), depth(pixels * 4);
    // Frame 2 is repeated to model a missing capture. Distinct levels survive H.264.
    const uint8_t ids[] = {1, 2, 2, 3, 4, 5, 6};
    for (const uint8_t id : ids) {
        for (uint32_t i = 0; i < pixels; ++i) {
            color[i * 4] = color[i * 4 + 1] = color[i * 4 + 2] = id * 32;
            depth[i * 4] = id;
            depth[i * 4 + 1] = static_cast<uint8_t>(i >> 8);
            depth[i * 4 + 2] = static_cast<uint8_t>(i);
        }
        CHECK(encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                                  depth.data(), static_cast<uint32_t>(depth.size())));
    }
    CHECK(encoder.stop());
    CHECK(std::distance(std::filesystem::directory_iterator(encoder.last_folder()),
                        std::filesystem::directory_iterator()) == 2);
    const auto color_frames = read_frames(ffmpeg, encoder.last_color_path(), directory / L"color.raw");
    const auto depth_frames = read_frames(ffmpeg, encoder.last_depth_path(), directory / L"depth.raw");
    CHECK(color_frames.size() == std::size(ids) * pixels * 3);
    CHECK(depth_frames.size() == color_frames.size());
    for (size_t frame = 0; frame < std::size(ids); ++frame) {
        const size_t base = frame * pixels * 3;
        const int level = ids[frame] * 32;
        CHECK(std::abs(color_frames[base] - level) < 8);
        for (uint32_t pixel = 0; pixel < pixels; ++pixel) {
            CHECK(depth_frames[base + pixel * 3] == ids[frame]);
            CHECK(depth_frames[base + pixel * 3 + 1] == static_cast<uint8_t>(pixel >> 8));
            CHECK(depth_frames[base + pixel * 3 + 2] == static_cast<uint8_t>(pixel));
        }
    }

    const auto first_output = encoder.last_folder();
    CHECK(encoder.start(width, height, settings));
    CHECK(encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                              depth.data(), static_cast<uint32_t>(depth.size())));
    CHECK(!encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()), nullptr, 0));
    CHECK(!encoder.stop(false, L"Recording fell behind the output clock."));
    CHECK(encoder.last_folder() != first_output);
    CHECK(encoder.last_folder().ends_with(L"_partial"));
    CHECK(std::filesystem::exists(first_output));
    CHECK(encoder.error().find(L"fell behind") != std::wstring::npos);
    // Occupy the output path so FFmpeg itself exits when it tries to open depth.
    CHECK(encoder.start(width, height, settings));
    std::ofstream(std::filesystem::path(encoder.last_depth_path())) << "existing file";
    CHECK(std::filesystem::is_regular_file(encoder.last_depth_path()));
    bool failed = false;
    for (int i = 0; i < 32; ++i) {
        if (!encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                                 depth.data(), static_cast<uint32_t>(depth.size()))) {
            failed = true;
            break;
        }
    }
    CHECK(failed);
    CHECK(!encoder.stop(false));
    CHECK(encoder.last_folder().ends_with(L"_partial"));
    CHECK(encoder.error().find(L"depth") != std::wstring::npos);
    CHECK(encoder.error().find(L"already exists") != std::wstring::npos);

    CHECK(encoder.start(width, height, settings));
    CHECK(encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                              depth.data(), static_cast<uint32_t>(depth.size())));
    const auto staging = std::filesystem::path(encoder.last_color_path()).parent_path().wstring();
    const std::wstring partial_suffix = L"_partial";
    CHECK(staging.ends_with(partial_suffix));
    const auto final_folder = std::filesystem::path(staging.substr(0, staging.size() - partial_suffix.size()));
    CHECK(std::filesystem::create_directory(final_folder));
    const auto sentinel = final_folder / L"keep.txt";
    std::ofstream(sentinel) << "existing folder";
    CHECK(!encoder.stop());
    CHECK(encoder.last_folder() == staging);
    CHECK(encoder.error().find(L"output folder") != std::wstring::npos);
    CHECK(std::filesystem::exists(sentinel));
    CHECK(std::filesystem::is_regular_file(encoder.last_color_path()));
    CHECK(std::filesystem::is_regular_file(encoder.last_depth_path()));

    CHECK(encoder.start(width, height, settings));
    CHECK(encoder.write_frame(color.data(), static_cast<uint32_t>(color.size()),
                              depth.data(), static_cast<uint32_t>(depth.size())));
    CHECK(encoder.stop());
    CHECK(!encoder.last_folder().ends_with(L"_partial"));
    CHECK(std::distance(std::filesystem::directory_iterator(encoder.last_folder()),
                        std::filesystem::directory_iterator()) == 2);
}

} // namespace

int main(int argc, char**) {
    try {
        test_timing();
        test_target_selection();
        test_preview_extent();
        test_ipc_ring();
        test_slot_tracker();
        test_frame_inbox_and_fitter();
        test_shared_log_ring();
        if (argc > 1) return 0;
        const auto ffmpeg = dr::VideoEncoder::find_ffmpeg();
        if (ffmpeg.empty()) {
            std::puts("FFmpeg is missing beside the test executable.");
            return 77;
        }
        const auto directory = std::filesystem::temp_directory_path() /
            (L"depth-recorder-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(directory);
        test_encoder(ffmpeg, directory);
        std::filesystem::remove_all(directory);
        std::puts("Timing and two-stream FFmpeg tests passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Test failed: %s\n", error.what());
        return 1;
    }
}
