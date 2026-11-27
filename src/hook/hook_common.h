#pragma once
#include "common/config.h"
#include "common/ipc_ring.h"
#include "depth_heuristic.h"
#include "fps_overlay_font.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
        InterlockedExchange(&cfg->active_backend, backend_id);
        InterlockedExchange(&cfg->current_fps_x10, static_cast<LONG>(fps * 10.0));
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
        InterlockedExchange(&cfg->candidate_depth_count, static_cast<LONG>(ranked.size()));
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

inline void push_frame_to_ring(IpcRing& ring, SharedConfig* cfg, uint64_t frame_idx,
                               int64_t qpc_ts, int64_t qpc_freq, uint32_t w, uint32_t h,
                               uint32_t mode, bool has_depth,
                               const void* color_data, uint32_t color_bytes,
                               const void* depth_data, uint32_t depth_bytes,
                               bool color_is_rgba = false, bool is_preview_only = false) {
    FrameHeader hdr{};
    hdr.frame_index = frame_idx;
    hdr.qpc_timestamp = qpc_ts;
    hdr.qpc_freq = qpc_freq;
    hdr.width = w;
    hdr.height = h;
    hdr.color_bytes = is_preview_only ? 0 : color_bytes;
    hdr.depth_bytes = has_depth ? depth_bytes : 0;
    hdr.depth_mode = mode;
    hdr.has_depth = has_depth ? 1 : 0;
    hdr.color_is_rgba = color_is_rgba ? 1 : 0;
    hdr.is_preview_only = is_preview_only ? 1 : 0;

    if (!ring.try_push(hdr, hdr.color_bytes ? color_data : nullptr,
                       (has_depth && depth_bytes > 0) ? depth_data : nullptr)) {
        if (cfg && !is_preview_only) InterlockedIncrement(&cfg->dropped_frames);
    }
}

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

inline void render_fps_overlay_rgba(double fps, double rec_fps, bool recording,
                                    std::vector<uint8_t>& out_rgba, uint32_t& out_bw, uint32_t& out_bh) {
    char text[48]{};
    if (recording) {
        std::snprintf(text, sizeof(text), "%.0f - rec: %.0f", fps, rec_fps);
    } else {
        std::snprintf(text, sizeof(text), "%.0f", fps);
    }
    render_fps_bitmap(text, recording, out_rgba, out_bw, out_bh);
}

inline void swizzle_rgba_to_bgra(uint8_t* bgra, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        std::swap(bgra[i * 4 + 0], bgra[i * 4 + 2]);
    }
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
};

inline DepthConvertParams make_depth_convert_params(uint32_t w, uint32_t h, uint32_t mode, const SharedConfig* cfg) {
    DepthConvertParams p{};
    p.width = w;
    p.height = h;
    p.mode = mode;
    p.linearize = cfg ? static_cast<uint32_t>(cfg->linearize) : 1;
    p.reversed_z = cfg ? static_cast<uint32_t>(cfg->reversed_z) : 1;
    p.z_near = cfg ? cfg->z_near : 0.1f;
    p.z_far = cfg ? cfg->z_far : 500.0f;
    return p;
}

} // namespace dr
