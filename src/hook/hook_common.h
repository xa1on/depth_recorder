#pragma once
#include "common/config.h"
#include "common/ipc_ring.h"
#include "depth_heuristic.h"
#include "fps_overlay_font.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace dr {

inline void ensure_ipc_connected(SharedConfigView& cfg_view, IpcRing& ring) {
    if (!cfg_view.get()) cfg_view.open_or_create(false);
    if (!ring.event_handle()) {
        if (!ring.open_or_create(false) && cfg_view.get()) {
            InterlockedExchange(&cfg_view.get()->dropped_frames, static_cast<LONG>(GetLastError()) + 100000);
        }
    }
}

inline double tick_fps_and_telemetry(SharedConfig* cfg, int32_t backend_id, int64_t& last_qpc,
                                     int64_t qpc_freq, LONG& last_cycle_seq,
                                     uint32_t& selected_depth_idx, int64_t& out_now_qpc) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    out_now_qpc = now.QuadPart;

    double fps = 60.0;
    if (last_qpc != 0 && qpc_freq > 0) {
        const double dt = static_cast<double>(now.QuadPart - last_qpc) / static_cast<double>(qpc_freq);
        if (dt > 1e-5) fps = 1.0 / dt;
    }
    last_qpc = now.QuadPart;

    if (cfg) {
        InterlockedExchange(&cfg->hooked_pid, static_cast<LONG>(GetCurrentProcessId()));
        const LONG prev_backend = InterlockedExchange(&cfg->active_backend, backend_id);
        if (prev_backend != backend_id) {
            const char* name = backend_id == 1 ? "DX11" : backend_id == 2 ? "GL" : backend_id == 3 ? "VK" : "DX12";
            log_push(cfg, name, "Hook active (pid=%lu)", GetCurrentProcessId());
        }
        InterlockedExchange(&cfg->current_fps_x10, static_cast<LONG>(fps * 10.0));
        if (cfg->recording) InterlockedIncrement(&cfg->presented_frames);
        if (cfg->cycle_depth_seq != last_cycle_seq) {
            last_cycle_seq = cfg->cycle_depth_seq;
            selected_depth_idx++;
        }
    }
    return fps;
}

inline bool select_depth_candidate(const std::vector<DepthCandidate>& ranked, SharedConfig* cfg,
                                   uint32_t& selected_depth_idx, DepthCandidate& out_cand) {
    if (cfg) {
        const LONG prev_cnt = InterlockedExchange(&cfg->candidate_depth_count, static_cast<LONG>(ranked.size()));
        if (prev_cnt != static_cast<LONG>(ranked.size())) {
            log_push(cfg, "DEPTH", "Depth candidates: %zu", ranked.size());
        }
        if (!ranked.empty()) {
            selected_depth_idx %= static_cast<uint32_t>(ranked.size());
            InterlockedExchange(&cfg->selected_depth_index, static_cast<LONG>(selected_depth_idx));
        }
    }
    if (ranked.empty()) return false;
    out_cand = ranked[selected_depth_idx % ranked.size()];
    return true;
}

inline bool should_capture_preview_now(int64_t now_qpc, int64_t qpc_freq, int64_t& last_preview_qpc) {
    if (qpc_freq <= 0) return true;
    if (last_preview_qpc == 0 || (now_qpc - last_preview_qpc) >= (qpc_freq / 6)) {
        last_preview_qpc = now_qpc;
        return true;
    }
    return false;
}

inline bool should_capture_recording_now(int64_t now_qpc, int64_t qpc_freq, uint32_t target_fps, int64_t& next_capture_qpc) {
    if (qpc_freq <= 0 || target_fps == 0) return true;
    const int64_t period = qpc_freq / std::clamp<uint32_t>(target_fps, 1, 240);
    if (period == 0) return true;
    if (!next_capture_qpc) next_capture_qpc = now_qpc;
    // A slightly early present can fill the next slot without shifting the capture clock.
    const int64_t eligible = now_qpc + period / 4;
    if (eligible < next_capture_qpc) return false;
    next_capture_qpc += ((eligible - next_capture_qpc) / period + 1) * period;
    return true;
}

inline void publish_frame(IpcRing& ring, SharedConfig* cfg, const FrameHeader& hdr) {
    ring.commit_push(hdr);
    if (cfg && !hdr.is_preview_only && hdr.color_bytes && (hdr.has_depth || !cfg->depth_enabled) &&
        cfg->capture_generation == hdr.capture_generation && cfg->target_pid == static_cast<LONG>(hdr.producer_pid)) {
        InterlockedIncrement(&cfg->published_pairs);
    }
}

inline RecordingActivity recording_activity(const SharedConfig* cfg) {
    return cfg ? static_cast<RecordingActivity>(cfg->recording_activity) : RecordingActivity::Normal;
}

struct OverlayBitmapCache {
    std::string last_text;
    bool last_recording = false;
    int64_t last_update_qpc = 0;
    std::vector<uint8_t> rgba;
    uint32_t bw = 0;
    uint32_t bh = 0;

    bool update(double fps, double rec_fps, bool recording, RecordingActivity activity, int64_t now_qpc, int64_t qpc_freq) {
        // Throttle text re-rasterization to 4 Hz while always keeping a valid initial bitmap.
        if (bw > 0 && bh > 0 && last_recording == recording && qpc_freq > 0 &&
            last_update_qpc != 0 && (now_qpc - last_update_qpc) < (qpc_freq / 4)) {
            return false;
        }
        last_update_qpc = now_qpc;
        char text[48]{};
        if (recording && activity == RecordingActivity::WaitingForCapture) {
            std::snprintf(text, sizeof(text), "%.0f - rec: WAIT", fps);
        } else if (recording && activity == RecordingActivity::EncoderStalled) {
            std::snprintf(text, sizeof(text), "%.0f - rec: STALL", fps);
        } else if (recording) {
            std::snprintf(text, sizeof(text), "%.0f - rec: %.0f", fps, rec_fps);
        } else {
            std::snprintf(text, sizeof(text), "%.0f", fps);
        }
        if (bw > 0 && bh > 0 && last_recording == recording && last_text == text) {
            return false;
        }
        last_text = text;
        last_recording = recording;
        render_fps_bitmap(text, recording, rgba, bw, bh);
        return true;
    }
};

inline void compute_overlay_pixel_rect(uint32_t bw, uint32_t bh, uint32_t view_w, uint32_t view_h,
                                       OverlayCorner corner, int32_t& out_x, int32_t& out_y) {
    constexpr int32_t pad = 12;
    out_x = pad;
    out_y = pad;
    if (corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight) {
        out_x = static_cast<int32_t>(view_w) - pad - static_cast<int32_t>(bw);
    }
    if (corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight) {
        out_y = static_cast<int32_t>(view_h) - pad - static_cast<int32_t>(bh);
    }
}

inline void compute_overlay_ndc_rect(uint32_t bw, uint32_t bh, uint32_t view_w, uint32_t view_h,
                                     OverlayCorner corner, float out_ndc[4]) {
    int32_t px = 0, py = 0;
    compute_overlay_pixel_rect(bw, bh, view_w, view_h, corner, px, py);
    const float fw = static_cast<float>(view_w);
    const float fh = static_cast<float>(view_h);
    const float x0 = -1.0f + (2.0f * static_cast<float>(px)) / fw;
    const float y1 = 1.0f - (2.0f * static_cast<float>(py)) / fh;
    const float w_ndc = (2.0f * static_cast<float>(bw)) / fw;
    const float h_ndc = (2.0f * static_cast<float>(bh)) / fh;
    out_ndc[0] = x0;
    out_ndc[1] = y1 - h_ndc;
    out_ndc[2] = x0 + w_ndc;
    out_ndc[3] = y1;
}

inline void unpack_r10g10b10a2_to_bgra(const uint8_t* src, uint8_t* dst_bgra, size_t pixel_count, bool bgr_order) {
    const auto* p32 = reinterpret_cast<const uint32_t*>(src);
    for (size_t i = 0; i < pixel_count; ++i) {
        const uint32_t v = p32[i];
        const uint8_t c0 = static_cast<uint8_t>((v & 0x3FFu) >> 2);
        const uint8_t c1 = static_cast<uint8_t>(((v >> 10) & 0x3FFu) >> 2);
        const uint8_t c2 = static_cast<uint8_t>(((v >> 20) & 0x3FFu) >> 2);
        dst_bgra[i * 4 + 0] = bgr_order ? c0 : c2;
        dst_bgra[i * 4 + 1] = c1;
        dst_bgra[i * 4 + 2] = bgr_order ? c2 : c0;
        dst_bgra[i * 4 + 3] = 255;
    }
}

struct DepthConvertParams {
    uint32_t width;
    uint32_t height;
    uint32_t mode;
    uint32_t linearize;
    uint32_t reversed_z;
    float z_near;
    float z_far;
    float pad;
    float source_scale_x;
    float source_scale_y;
    float source_left;
    float source_top;
};

inline DepthConvertParams make_depth_convert_params(uint32_t w, uint32_t h, uint32_t source_w, uint32_t source_h,
                                                     uint32_t mode, const SharedConfig* cfg,
                                                     const DepthRegion* region = nullptr) {
    DepthConvertParams p{};
    p.width = w;
    p.height = h;
    p.mode = mode;
    p.linearize = cfg ? static_cast<uint32_t>(cfg->linearize) : 1;
    p.reversed_z = cfg ? static_cast<uint32_t>(cfg->reversed_z) : 1;
    p.z_near = cfg ? cfg->z_near : 0.1f;
    p.z_far = cfg ? cfg->z_far : 500.0f;
    p.source_scale_x = static_cast<float>(source_w) / static_cast<float>(w);
    p.source_scale_y = static_cast<float>(source_h) / static_cast<float>(h);
    if (region && region->left >= 0 && region->top >= 0 && region->width >= 64 && region->height >= 64 &&
        region->left + region->width <= source_w && region->top + region->height <= source_h &&
        DepthHeuristic::is_plausible_size(static_cast<uint32_t>(region->width), static_cast<uint32_t>(region->height), w, h)) {
        p.source_left = region->left;
        p.source_top = region->top;
        p.source_scale_x = region->width / static_cast<float>(w);
        p.source_scale_y = region->height / static_cast<float>(h);
    }
    return p;
}

} // namespace dr
