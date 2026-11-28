#include "ui_win32.h"
#include "settings_ini.h"
#include "vulkan_layer_reg.h"
#include "injector.h"
#include "common/frame_extent.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <filesystem>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>

namespace dr {

namespace {

enum ControlId : int {
    IDC_PROC_COMBO = 101,
    IDC_BTN_ATTACH = 103,
    IDC_BTN_LAUNCH = 104,
    IDC_EDIT_OUTDIR = 105,
    IDC_BTN_BROWSE = 106,
    IDC_MODE_COMBO = 107,
    IDC_CHK_DEPTH = 108,
    IDC_CHK_LINEAR = 109,
    IDC_CHK_REVZ = 110,
    IDC_CHK_OVERLAY = 111,
    IDC_CORNER_COMBO = 112,
    IDC_BTN_RECORD = 113,
    IDC_BTN_CYCLE_DEPTH = 114,
    IDC_LBL_STATUS = 115,
    IDC_TAB_MAIN = 116,
    IDC_HK_RECORD = 117,
    IDC_BTN_APPLY_HK = 119,
    IDC_EDIT_ZNEAR = 120,
    IDC_EDIT_ZFAR = 121,
    IDC_PREVIEW_BOX = 122,
    IDC_CHK_PREVIEW = 123,
    IDC_SLIDER_ZNEAR = 124,
    IDC_SLIDER_ZFAR = 125,
    IDC_BTN_AUTOCONFIG = 126,
    IDC_BTN_DETACH = 127,
    IDC_SWAP_COMBO = 128,
    IDC_SLIDER_QUAL = 129,
    IDC_LBL_QUAL = 130,
    IDC_RECFPS_COMBO = 132,
    IDC_BTN_OPEN = 133,
    IDC_LBL_STATS = 134,
    IDC_CHK_DROP_DEAD = 135,
    IDC_EDIT_FFMPEG = 136,
    IDC_CHK_VK_LAYER = 137,
    IDC_EDIT_LOG = 138,
    IDC_BTN_LOG_CLEAR = 139,
    IDC_BTN_LOG_COPY = 140,
    IDC_BTN_LOG_OPEN = 141,
};

constexpr int kHotkeyRecordId = 1;
constexpr UINT_PTR kTimerId = 1;

struct TargetWindow {
    SwapchainTarget target;
    LONG index;
    uint32_t width;
    uint32_t height;

    bool operator==(const TargetWindow&) const = default;
};

struct UiContext {
    RecorderAppState* app = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::vector<ProcessEntry> procs;
    std::vector<HWND> capture_ctrls;
    std::vector<HWND> settings_ctrls;
    std::vector<HWND> log_ctrls;
    std::vector<TargetWindow> listed_windows;
    SwapchainTarget manual_target;
    SwapchainTarget selected_target;
    ULONGLONG attached_at = 0;
    bool vulkan_layer_missing = false;
    uint64_t log_version = 0;
};

void populate_processes(UiContext* ctx) {
    HWND combo = GetDlgItem(ctx->hwnd, IDC_PROC_COMBO);
    DWORD prev_pid = 0;
    const LRESULT cur = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (cur >= 0 && static_cast<size_t>(cur) < ctx->procs.size()) {
        prev_pid = ctx->procs[cur].pid;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    ctx->procs = list_processes();
    int sel = -1;
    for (size_t i = 0; i < ctx->procs.size(); ++i) {
        const auto& p = ctx->procs[i];
        if (p.pid == prev_pid) sel = static_cast<int>(i);
        std::wstring label = p.name + L"  (PID " + std::to_wstring(p.pid) + L")";
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void switch_tab(UiContext* ctx, int tab_idx);

void refresh_log_box(UiContext* ctx, bool force_bottom = false) {
    if (!ctx || !ctx->app) return;
    ctx->app->log.drain_shared(ctx->app->config_view.get());
    HWND tab = GetDlgItem(ctx->hwnd, IDC_TAB_MAIN);
    if (!tab || TabCtrl_GetCurSel(tab) != 2) return;
    HWND edit = GetDlgItem(ctx->hwnd, IDC_EDIT_LOG);
    if (!edit) return;

    DWORD sel_start = 0, sel_end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
    if (!force_bottom && sel_start != sel_end) return; // Don't clobber active text selection while the user is copying.

    std::wstring text;
    if (!ctx->app->log.snapshot(ctx->log_version, text)) {
        if (force_bottom) SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
        return;
    }

    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    bool at_bottom = true;
    if (GetScrollInfo(edit, SB_VERT, &si)) {
        at_bottom = (si.nMax <= static_cast<int>(si.nPage)) ||
                    (si.nPos + static_cast<int>(si.nPage) >= si.nMax - 2);
    }
    const int top_line = static_cast<int>(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));

    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(edit, text.c_str());
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    if (force_bottom || at_bottom) {
        SendMessageW(edit, WM_VSCROLL, SB_BOTTOM, 0);
    } else if (top_line > 0) {
        SendMessageW(edit, EM_LINESCROLL, 0, top_line);
    }
    InvalidateRect(edit, nullptr, FALSE);
}

void switch_tab(UiContext* ctx, int tab_idx) {
    for (HWND h : ctx->capture_ctrls) ShowWindow(h, tab_idx == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : ctx->settings_ctrls) ShowWindow(h, tab_idx == 1 ? SW_SHOW : SW_HIDE);
    for (HWND h : ctx->log_ctrls) ShowWindow(h, tab_idx == 2 ? SW_SHOW : SW_HIDE);
    if (tab_idx == 2) refresh_log_box(ctx, true);
}

UINT hk_mods_to_win32(BYTE hk_mods) {
    UINT m = 0;
    if (hk_mods & HOTKEYF_ALT) m |= MOD_ALT;
    if (hk_mods & HOTKEYF_CONTROL) m |= MOD_CONTROL;
    if (hk_mods & HOTKEYF_SHIFT) m |= MOD_SHIFT;
    return m;
}

BYTE win32_mods_to_hk(UINT mods) {
    BYTE m = 0;
    if (mods & MOD_ALT) m |= HOTKEYF_ALT;
    if (mods & MOD_CONTROL) m |= HOTKEYF_CONTROL;
    if (mods & MOD_SHIFT) m |= HOTKEYF_SHIFT;
    return m;
}

float slider_to_znear(int pos) {
    return 0.01f * std::pow(1000.0f, static_cast<float>(pos) / 1000.0f);
}

int znear_to_slider(float zn) {
    zn = std::clamp(zn, 0.01f, 10.0f);
    return static_cast<int>(std::lround(1000.0f * (std::log10(zn / 0.01f) / 3.0f)));
}

float slider_to_zfar(int pos) {
    return 10.0f * std::pow(1000.0f, static_cast<float>(pos) / 1000.0f);
}

int zfar_to_slider(float zf) {
    zf = std::clamp(zf, 10.0f, 10000.0f);
    return static_cast<int>(std::lround(1000.0f * (std::log10(zf / 10.0f) / 3.0f)));
}

void set_z_controls(HWND hwnd, float zn, float zf) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.3f", zn);
    SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_ZNEAR), buf);
    SendMessageW(GetDlgItem(hwnd, IDC_SLIDER_ZNEAR), TBM_SETPOS, TRUE, znear_to_slider(zn));

    swprintf_s(buf, L"%.1f", zf);
    SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_ZFAR), buf);
    SendMessageW(GetDlgItem(hwnd, IDC_SLIDER_ZFAR), TBM_SETPOS, TRUE, zfar_to_slider(zf));
}

void sync_ui_to_config(UiContext* ctx);

void run_auto_config(UiContext* ctx, bool initial_attach) {
    auto* cfg = ctx->app->config_view.get();
    const LONG backend = cfg ? cfg->active_backend : 0;
    const bool is_gl = (backend == 2);
    const bool rev_z = !is_gl;

    float zn = is_gl ? 0.5f : 0.1f;
    float zf = is_gl ? 150.0f : 500.0f;

    if (!initial_attach && cfg) {
        zn = cfg->z_near;
        zf = cfg->z_far;
        std::vector<uint8_t> lumas;
        {
            std::lock_guard lock(ctx->app->preview_mutex);
            if (!ctx->app->preview_bgra.empty()) {
                const size_t n = ctx->app->preview_bgra.size() / 4;
                const size_t step = std::max<size_t>(1, n / 2048);
                lumas.reserve(n / step + 1);
                for (size_t i = 0; i < n; i += step) {
                    lumas.push_back(ctx->app->preview_bgra[i * 4 + 2]);
                }
            }
        }
        if (lumas.size() >= 64) {
            std::sort(lumas.begin(), lumas.end());
            const uint8_t p15 = lumas[lumas.size() * 15 / 100];
            const uint8_t p50 = lumas[lumas.size() / 2];
            const uint8_t p85 = lumas[lumas.size() * 85 / 100];
            if (p50 > 215 && p15 > 150) {
                zf = std::clamp(zf * 0.45f, 15.0f, 10000.0f);
                zn = std::clamp(zn * 1.5f, 0.01f, 5.0f);
            } else if (p50 < 40 && p85 < 100) {
                zf = std::clamp(zf * 2.2f, 15.0f, 10000.0f);
            }
        }
    }

    CheckDlgButton(ctx->hwnd, IDC_CHK_LINEAR, BST_CHECKED);
    CheckDlgButton(ctx->hwnd, IDC_CHK_REVZ, rev_z ? BST_CHECKED : BST_UNCHECKED);
    set_z_controls(ctx->hwnd, zn, zf);
    sync_ui_to_config(ctx);
}

void sync_ui_to_config(UiContext* ctx) {
    SharedConfig* cfg = ctx->app->config_view.get();
    if (!cfg) return;

    const LONG old_mode = cfg->depth_mode;
    const auto old_fps = ctx->app->recording_fps;
    const auto old_quality = ctx->app->h264_quality.load();
    const LONG old_depth = cfg->depth_enabled;
    const LONG old_linearize = cfg->linearize;
    const LONG old_reversed_z = cfg->reversed_z;
    const float old_near = cfg->z_near, old_far = cfg->z_far;
    const std::wstring old_extra = ctx->app->extra_ffmpeg_flags;

    wchar_t dir_buf[MAX_PATH]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_OUTDIR), dir_buf, MAX_PATH);
    if (dir_buf[0]) ctx->app->output_dir = dir_buf;

    wchar_t ff_buf[512]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_FFMPEG), ff_buf, 512);
    ctx->app->extra_ffmpeg_flags = ff_buf;

    const LRESULT mode_idx = SendMessageW(GetDlgItem(ctx->hwnd, IDC_MODE_COMBO), CB_GETCURSEL, 0, 0);
    if (mode_idx >= 0 && mode_idx <= 2) {
        InterlockedExchange(&cfg->depth_mode, static_cast<LONG>(mode_idx));
    }
    const bool depth_en = (IsDlgButtonChecked(ctx->hwnd, IDC_CHK_DEPTH) == BST_CHECKED);
    const bool prev_en = (IsDlgButtonChecked(ctx->hwnd, IDC_CHK_PREVIEW) == BST_CHECKED);
    ctx->app->drop_dead_frames = IsDlgButtonChecked(ctx->hwnd, IDC_CHK_DROP_DEAD) == BST_CHECKED;
    InterlockedExchange(&cfg->depth_enabled, depth_en ? 1 : 0);
    InterlockedExchange(&cfg->linearize, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_LINEAR) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->reversed_z, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_REVZ) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->overlay_enabled, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_OVERLAY) == BST_CHECKED ? 1 : 0);
    ctx->app->show_preview.store(depth_en && prev_en);
    if (!depth_en || !prev_en) {
        std::lock_guard lock(ctx->app->preview_mutex);
        ctx->app->preview_bgra.clear();
    }

    const LRESULT corner_idx = SendMessageW(GetDlgItem(ctx->hwnd, IDC_CORNER_COMBO), CB_GETCURSEL, 0, 0);
    if (corner_idx >= 0 && corner_idx <= 3) {
        InterlockedExchange(&cfg->overlay_corner, static_cast<LONG>(corner_idx));
    }

    wchar_t zbuf[32]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_ZNEAR), zbuf, 32);
    if (zbuf[0]) {
        const float zn = static_cast<float>(_wtof(zbuf));
        if (zn > 0.0f) cfg->z_near = zn;
    }
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_ZFAR), zbuf, 32);
    if (zbuf[0]) {
        const float zf = static_cast<float>(_wtof(zbuf));
        if (zf > cfg->z_near) cfg->z_far = zf;
    }

    wchar_t fpsbuf[16]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_RECFPS_COMBO), fpsbuf, 16);
    if (fpsbuf[0]) {
        const uint32_t f = static_cast<uint32_t>(_wtoi(fpsbuf));
        if (f >= 1 && f <= 240) {
            ctx->app->recording_fps = f;
            InterlockedExchange(&cfg->target_rec_fps, static_cast<LONG>(f));
        }
    }

    ctx->app->h264_quality = static_cast<uint32_t>(SendMessageW(GetDlgItem(ctx->hwnd, IDC_SLIDER_QUAL), TBM_GETPOS, 0, 0));
    if (cfg->depth_mode != old_mode || cfg->depth_enabled != old_depth ||
        cfg->linearize != old_linearize || cfg->reversed_z != old_reversed_z ||
        cfg->z_near != old_near || cfg->z_far != old_far ||
        ctx->app->recording_fps != old_fps || ctx->app->h264_quality != old_quality ||
        ctx->app->extra_ffmpeg_flags != old_extra) {
        InterlockedIncrement(&cfg->capture_generation);
        std::lock_guard lock(ctx->app->preview_mutex);
        ctx->app->preview_bgra.clear();
    }
}

bool is_active(RecordingStatus status) {
    return status == RecordingStatus::Starting || status == RecordingStatus::Recording ||
           status == RecordingStatus::Saving;
}

RecordingStatus recording_status(RecorderAppState& app) {
    std::lock_guard lock(app.recording.mutex);
    return app.recording.status;
}

void toggle_recording(UiContext* ctx) {
    if (is_active(recording_status(*ctx->app))) {
        stop_recording(*ctx->app);
    } else {
        sync_ui_to_config(ctx);
        start_recording(*ctx->app);
    }
}

void set_target(UiContext* ctx, LONG pid, LONG window_index = -1) {
    auto* cfg = ctx->app->config_view.get();
    InterlockedExchange(&cfg->target_pid, -1);
    InterlockedIncrement(&cfg->capture_generation);
    InterlockedExchange(&cfg->hooked_pid, 0);
    InterlockedExchange(&cfg->active_backend, 0);
    InterlockedExchange(&cfg->candidate_depth_count, 0);
    InterlockedExchange(&cfg->swapchain_count, 0);
    InterlockedExchange(&cfg->selected_swapchain_index, window_index);
    ctx->manual_target = {};
    ctx->selected_target = {};
    ctx->attached_at = pid >= 0 ? GetTickCount64() : 0;
    ctx->vulkan_layer_missing = false;
    InterlockedExchange(&cfg->target_pid, pid);
    {
        std::lock_guard lock(ctx->app->preview_mutex);
        ctx->app->preview_bgra.clear();
    }
    std::lock_guard lock(ctx->app->recording.mutex);
    ctx->app->recording.status = RecordingStatus::Idle;
    ctx->app->recording.message.clear();
    ctx->app->recording.folder.clear();
}

void update_status(UiContext* ctx) {
    auto& app = *ctx->app;
    auto* cfg = app.config_view.get();
    std::wstring message;
    RecordingStatus status;
    bool has_output_folder;
    {
        std::lock_guard lock(app.recording.mutex);
        status = app.recording.status;
        message = app.recording.message;
        has_output_folder = !app.recording.folder.empty();
    }

    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const bool valid_pair = cfg->selected_swapchain_index >= 0 && app.ready_generation == cfg->capture_generation &&
                       now.QuadPart - app.ready_qpc.load() < frequency.QuadPart;
    const bool ready = valid_pair && app.encoder_ready_generation == cfg->capture_generation;
    std::wstring label;
    switch (status) {
        case RecordingStatus::Starting: label = L"Starting\u2026"; break;
        case RecordingStatus::Recording:
            switch (static_cast<RecordingActivity>(cfg->recording_activity)) {
                case RecordingActivity::WaitingForCapture: label = L"Waiting for capture"; break;
                case RecordingActivity::EncoderStalled: label = L"Encoder stalled"; break;
                default: label = L"Recording"; break;
            }
            break;
        case RecordingStatus::Saving: label = L"Saving\u2026"; break;
        case RecordingStatus::Saved: label = L"Saved"; break;
        case RecordingStatus::Failed: label = L"Recording error"; break;
        default: {
            const wchar_t* bname = cfg->active_backend == 1 ? L"DX11" : cfg->active_backend == 2 ? L"OpenGL" : cfg->active_backend == 3 ? L"Vulkan" : cfg->active_backend == 4 ? L"DX12" : nullptr;
            if (ready) label = bname ? (std::wstring(L"Ready (") + bname + L")") : L"Ready";
            else if (valid_pair && app.encoder_failed_generation == cfg->capture_generation)
                label = L"The bundled encoder could not prepare this recording profile.";
            else if (valid_pair) label = L"Preparing encoder\u2026";
            else if (ctx->listed_windows.size() > 1 && cfg->selected_swapchain_index < 0) label = L"Choose a target window";
            else if (ctx->attached_at && ctx->listed_windows.empty() && ctx->vulkan_layer_missing) {
                label = is_vulkan_implicit_layer_registered()
                    ? L"Vulkan layer not loaded in target. Relaunch the game so it loads, or use Launch EXE."
                    : L"Vulkan layer not loaded. Enable 'Vulkan Implicit Layer (Attach)' in Settings & relaunch, or use Launch EXE.";
            } else if (ctx->attached_at && ctx->listed_windows.empty() && GetTickCount64() - ctx->attached_at > 6000) {
                label = is_vulkan_implicit_layer_registered()
                    ? L"No compatible window found. For Vulkan, relaunch the game or use Launch EXE."
                    : L"No compatible window found. For Vulkan, enable Implicit Layer in Settings & relaunch, or use Launch EXE.";
            } else if (ctx->attached_at) label = cfg->depth_enabled ? L"Waiting for color and depth" : L"Waiting for color";
            else label = L"Attach to a process";
            break;
        }
    }
    if (!message.empty()) label += L"  " + message;
    SetWindowTextW(GetDlgItem(ctx->hwnd, IDC_LBL_STATUS), label.c_str());
    const bool active = is_active(status);
    const bool saving = status == RecordingStatus::Saving;
    SetWindowTextW(GetDlgItem(ctx->hwnd, IDC_BTN_RECORD), saving ? L"Saving\u2026" : active ? L"Stop Recording" : L"Start Recording");
    EnableWindow(GetDlgItem(ctx->hwnd, IDC_BTN_RECORD), !saving && (active || ready));
    EnableWindow(GetDlgItem(ctx->hwnd, IDC_BTN_OPEN), has_output_folder &&
                 (status == RecordingStatus::Saved || status == RecordingStatus::Failed));
    const LONG cand_count = cfg->candidate_depth_count;
    const std::wstring cycle_label = (cfg->depth_enabled && cand_count > 0)
        ? L"Cycle Depth Buffer (" + std::to_wstring((cfg->selected_depth_index % cand_count) + 1) + L"/" + std::to_wstring(cand_count) + L")"
        : L"Cycle Depth Buffer";
    HWND cycle_btn = GetDlgItem(ctx->hwnd, IDC_BTN_CYCLE_DEPTH);
    wchar_t cur_cycle[64]{};
    GetWindowTextW(cycle_btn, cur_cycle, 64);
    if (cycle_label != cur_cycle) SetWindowTextW(cycle_btn, cycle_label.c_str());
    for (const int id : {IDC_PROC_COMBO, IDC_BTN_ATTACH, IDC_BTN_DETACH, IDC_BTN_LAUNCH, IDC_BTN_CYCLE_DEPTH,
                         IDC_CHK_LINEAR, IDC_CHK_REVZ, IDC_EDIT_ZNEAR, IDC_EDIT_ZFAR, IDC_SLIDER_ZNEAR,
                         IDC_SLIDER_ZFAR, IDC_BTN_AUTOCONFIG, IDC_RECFPS_COMBO, IDC_MODE_COMBO, IDC_CHK_DEPTH,
                         IDC_EDIT_OUTDIR, IDC_BTN_BROWSE, IDC_SLIDER_QUAL, IDC_CHK_OVERLAY, IDC_CORNER_COMBO,
                         IDC_CHK_PREVIEW, IDC_HK_RECORD, IDC_BTN_APPLY_HK, IDC_CHK_DROP_DEAD, IDC_EDIT_FFMPEG, IDC_CHK_VK_LAYER}) {
        EnableWindow(GetDlgItem(ctx->hwnd, id), !active);
    }
    const bool process_selected = SendMessageW(GetDlgItem(ctx->hwnd, IDC_PROC_COMBO), CB_GETCURSEL, 0, 0) >= 0;
    EnableWindow(GetDlgItem(ctx->hwnd, IDC_BTN_ATTACH), !active && process_selected);
    EnableWindow(GetDlgItem(ctx->hwnd, IDC_SWAP_COMBO), !active && ctx->listed_windows.size() > 1);
    wchar_t stats[256]{};
    const LONG present_rate = status == RecordingStatus::Recording ? app.present_fps_x10.load() : cfg->current_fps_x10;
    swprintf_s(stats, L"Game %.0f FPS   |   Unique %.0f FPS   |   Received %llu   |   Unique %llu   |   Output %llu",
               present_rate / 10.0, cfg->actual_rec_fps_x10 / 10.0,
               static_cast<unsigned long long>(app.received_pairs.load()),
               static_cast<unsigned long long>(app.unique_written_pairs.load()),
               static_cast<unsigned long long>(app.written_pairs.load()));
    SetWindowTextW(GetDlgItem(ctx->hwnd, IDC_LBL_STATS), stats);
}

void refresh_windows(UiContext* ctx) {
    auto* cfg = ctx->app->config_view.get();
    const LONG count = std::clamp<LONG>(static_cast<LONG>(cfg->swapchain_count), 0, kMaxSwapchains);
    std::vector<TargetWindow> windows;
    for (LONG i = 0; i < count; ++i) {
        SwapchainInfo entry{};
        if (!read_swapchain_info(cfg, i, entry) || !entry.id) continue;
        const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(entry.window));
        if (window && !IsWindow(window)) continue;
        windows.push_back({swapchain_target(entry), i, entry.width, entry.height});
    }
    LONG selected = cfg->selected_swapchain_index;
    const LONG previous = selected;
    const auto previous_target = ctx->selected_target;
    if (!is_active(recording_status(*ctx->app))) {
        const auto manual = std::find_if(windows.begin(), windows.end(), [&](const auto& entry) {
            return entry.target == ctx->manual_target;
        });
        if (manual == windows.end()) ctx->manual_target = {};
        selected = ctx->manual_target ? manual->index : largest_swapchain_index(*cfg);
        const auto current = std::find_if(windows.begin(), windows.end(), [&](const auto& entry) {
            SwapchainInfo cur{};
            return entry.index == selected && read_swapchain_info(cfg, entry.index, cur) && swapchain_target(cur) == entry.target;
        });
        if (current == windows.end()) selected = -1;
        ctx->selected_target = current == windows.end() ? SwapchainTarget{} : current->target;
        if (selected != previous || ctx->selected_target != previous_target) {
            InterlockedExchange(&cfg->selected_swapchain_index, selected);
            InterlockedIncrement(&cfg->capture_generation);
            std::lock_guard lock(ctx->app->preview_mutex);
            ctx->app->preview_bgra.clear();
        }
    }
    if (windows == ctx->listed_windows && selected == previous && ctx->selected_target == previous_target) return;

    ctx->listed_windows = std::move(windows);
    const HWND combo = GetDlgItem(ctx->hwnd, IDC_SWAP_COMBO);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < ctx->listed_windows.size(); ++i) {
        const auto& entry = ctx->listed_windows[i];
        wchar_t title[96]{}, label[144]{};
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(entry.target.window));
        if (window) GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        swprintf_s(label, L"%s  (%u x %u)", title[0] ? title : L"Untitled window", entry.width, entry.height);
        const LRESULT row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        if (row < 0) continue;
        SendMessageW(combo, CB_SETITEMDATA, row, i);
        if (entry.index == selected && entry.target == ctx->selected_target) SendMessageW(combo, CB_SETCURSEL, row, 0);
    }
}
HWND add_ctrl(HWND parent, HFONT font, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND ctrl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return ctrl;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<UiContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ctx = static_cast<UiContext*>(cs->lpCreateParams);
            ctx->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            load_settings_ini(*ctx->app, ctx->app->config_view.get());
            SharedConfig* cfg = ctx->app->config_view.get();

            NONCLIENTMETRICSW ncm{sizeof(ncm)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            ctx->font = CreateFontIndirectW(&ncm.lfMessageFont);

            HWND tab = add_ctrl(hwnd, ctx->font, WC_TABCONTROLW, L"", 0, 8, 6, 614, 280, IDC_TAB_MAIN);
            TCITEMW tie{};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<wchar_t*>(L"Capture");
            TabCtrl_InsertItem(tab, 0, &tie);
            tie.pszText = const_cast<wchar_t*>(L"Settings");
            TabCtrl_InsertItem(tab, 1, &tie);
            tie.pszText = const_cast<wchar_t*>(L"Log");
            TabCtrl_InsertItem(tab, 2, &tie);

            auto cap = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = add_ctrl(hwnd, ctx->font, cls, text, style, x, y + 26, w, h, id);
                ctx->capture_ctrls.push_back(c);
                return c;
            };
            auto set = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = add_ctrl(hwnd, ctx->font, cls, text, style, x, y + 26, w, h, id);
                ctx->settings_ctrls.push_back(c);
                return c;
            };

            // --- Capture Tab ---
            cap(L"STATIC", L"Target Process:", 0, 12, 14, 100, 20, 0);
            cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 115, 10, 250, 240, IDC_PROC_COMBO);
            cap(L"BUTTON", L"Attach", 0, 372, 10, 68, 24, IDC_BTN_ATTACH);
            cap(L"BUTTON", L"Detach", 0, 446, 10, 68, 24, IDC_BTN_DETACH);
            cap(L"BUTTON", L"Launch EXE...", 0, 520, 10, 96, 24, IDC_BTN_LAUNCH);

            cap(L"STATIC", L"Target Window:", 0, 12, 48, 100, 20, 0);
            cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 115, 44, 501, 200, IDC_SWAP_COMBO);

            HWND c_lin = cap(L"BUTTON", L"Linearize (Gray8/Phase)", BS_AUTOCHECKBOX, 12, 82, 175, 22, IDC_CHK_LINEAR);
            HWND c_rev = cap(L"BUTTON", L"Reversed-Z", BS_AUTOCHECKBOX, 200, 82, 110, 22, IDC_CHK_REVZ);
            SendMessageW(c_lin, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c_rev, BM_SETCHECK, BST_CHECKED, 0);

            cap(L"STATIC", L"Z-Near:", 0, 12, 116, 52, 20, 0);
            cap(L"EDIT", L"0.100", WS_BORDER | ES_AUTOHSCROLL, 66, 113, 80, 22, IDC_EDIT_ZNEAR);
            HWND s_zn = cap(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 12, 138, 190, 24, IDC_SLIDER_ZNEAR);
            SendMessageW(s_zn, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessageW(s_zn, TBM_SETPOS, TRUE, znear_to_slider(0.1f));

            cap(L"STATIC", L"Z-Far:", 0, 220, 116, 46, 20, 0);
            cap(L"EDIT", L"500.0", WS_BORDER | ES_AUTOHSCROLL, 268, 113, 86, 22, IDC_EDIT_ZFAR);
            HWND s_zf = cap(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 220, 138, 190, 24, IDC_SLIDER_ZFAR);
            SendMessageW(s_zf, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessageW(s_zf, TBM_SETPOS, TRUE, zfar_to_slider(500.0f));

            cap(L"BUTTON", L"Auto Config", 0, 424, 118, 192, 34, IDC_BTN_AUTOCONFIG);

            cap(L"BUTTON", L"Start Recording", BS_DEFPUSHBUTTON, 12, 194, 295, 34, IDC_BTN_RECORD);
            cap(L"BUTTON", L"Cycle Depth Buffer", 0, 321, 194, 160, 34, IDC_BTN_CYCLE_DEPTH);
            cap(L"STATIC", L"FPS", 0, 492, 202, 28, 22, 0);
            HWND fps_cb = cap(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL, 525, 198, 91, 130, IDC_RECFPS_COMBO);

            // --- Settings Tab (Persistent via depth_recorder.ini) ---
            set(L"STATIC", L"Output Folder:", 0, 14, 16, 100, 20, 0);
            set(L"EDIT", ctx->app->output_dir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 125, 13, 412, 22, IDC_EDIT_OUTDIR);
            set(L"BUTTON", L"Browse...", 0, 544, 12, 70, 24, IDC_BTN_BROWSE);

            set(L"STATIC", L"Depth mode:", 0, 14, 48, 105, 20, 0);
            HWND mode_cb = set(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 125, 44, 255, 120, IDC_MODE_COMBO);
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gray8 (H.264 MP4)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ColorPhase (H.264 MP4, Lossy-Safe)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"RGB24 Lossless (UtVideo MKV, 16.7M)"));
            SendMessageW(mode_cb, CB_SETCURSEL, cfg ? cfg->depth_mode : 0, 0);

            HWND c_vk = set(L"BUTTON", L"Vulkan Implicit Layer (Attach)", BS_AUTOCHECKBOX | WS_TABSTOP, 392, 46, 222, 22, IDC_CHK_VK_LAYER);
            SendMessageW(c_vk, BM_SETCHECK, is_vulkan_implicit_layer_registered() ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(c_vk, !find_vulkan_layer_manifest().empty());

            for (const wchar_t* fopt : {L"24", L"30", L"60", L"120", L"144"}) {
                SendMessageW(fps_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fopt));
            }
            SetWindowTextW(fps_cb, std::to_wstring(ctx->app->recording_fps).c_str());

            HWND c_dep = set(L"BUTTON", L"Enable Depth Pass", BS_AUTOCHECKBOX, 14, 80, 140, 22, IDC_CHK_DEPTH);
            HWND c_ovl = set(L"BUTTON", L"In-Game FPS Overlay", BS_AUTOCHECKBOX, 165, 80, 155, 22, IDC_CHK_OVERLAY);
            HWND c_prv = set(L"BUTTON", L"Live Depth Preview", BS_AUTOCHECKBOX, 330, 80, 145, 22, IDC_CHK_PREVIEW);
            SendMessageW(c_dep, BM_SETCHECK, (!cfg || cfg->depth_enabled) ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(c_ovl, BM_SETCHECK, (!cfg || cfg->overlay_enabled) ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(c_prv, BM_SETCHECK, ctx->app->show_preview ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND corner_cb = set(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 480, 78, 134, 120, IDC_CORNER_COMBO);
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Right"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Right"));
            SendMessageW(corner_cb, CB_SETCURSEL, cfg ? cfg->overlay_corner : 0, 0);

            set(L"STATIC", L"H.264 Quality:", 0, 14, 114, 100, 20, 0);
            HWND s_qual = set(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 125, 110, 200, 24, IDC_SLIDER_QUAL);
            SendMessageW(s_qual, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
            SendMessageW(s_qual, TBM_SETPOS, TRUE, static_cast<LPARAM>(ctx->app->h264_quality));
            wchar_t qbuf[16]{};
            swprintf_s(qbuf, L"%u%%", ctx->app->h264_quality.load());
            set(L"STATIC", qbuf, 0, 330, 114, 50, 20, IDC_LBL_QUAL);

            HWND drop_dead = set(L"BUTTON", L"Drop dead frames", BS_AUTOCHECKBOX | WS_TABSTOP, 14, 145, 150, 22, IDC_CHK_DROP_DEAD);
            SendMessageW(drop_dead, BM_SETCHECK, ctx->app->drop_dead_frames ? BST_CHECKED : BST_UNCHECKED, 0);
            set(L"STATIC", L"Skips missing frames in both videos. Playback may be shorter.", 0, 170, 147, 444, 20, 0);

            set(L"STATIC", L"Record Keybind:", 0, 14, 184, 105, 20, 0);
            HWND hk_rec = set(HOTKEY_CLASSW, L"", WS_BORDER, 125, 180, 180, 24, IDC_HK_RECORD);
            SendMessageW(hk_rec, HKM_SETHOTKEY,
                         MAKEWORD(ctx->app->hotkey_record_vk, win32_mods_to_hk(ctx->app->hotkey_record_mod)), 0);

            set(L"BUTTON", L"Apply & Save Settings", 0, 320, 178, 294, 28, IDC_BTN_APPLY_HK);

            set(L"STATIC", L"Extra FFmpeg:", 0, 14, 219, 105, 20, 0);
            set(L"EDIT", ctx->app->extra_ffmpeg_flags.c_str(), WS_BORDER | ES_AUTOHSCROLL, 125, 216, 489, 22, IDC_EDIT_FFMPEG);

            // --- Log Tab ---
            auto log_ctrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
                HWND c = add_ctrl(hwnd, ctx->font, cls, text, style, x, y + 26, w, h, id);
                ctx->log_ctrls.push_back(c);
                return c;
            };
            HWND log_edit = log_ctrl(
                L"EDIT", L"",
                WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                12, 10, 590, 206, IDC_EDIT_LOG);
            SendMessageW(log_edit, EM_SETLIMITTEXT, 262144, 0);
            log_ctrl(L"BUTTON", L"Clear", 0, 12, 222, 90, 24, IDC_BTN_LOG_CLEAR);
            log_ctrl(L"BUTTON", L"Copy All", 0, 110, 222, 100, 24, IDC_BTN_LOG_COPY);
            log_ctrl(L"BUTTON", L"Open Log File", 0, 218, 222, 120, 24, IDC_BTN_LOG_OPEN);
            refresh_log_box(ctx);

            // Shared Status Bar & Live Depth Preview
            add_ctrl(hwnd, ctx->font, L"STATIC", L"Attach to a process", SS_SUNKEN, 12, 294, 715, 42, IDC_LBL_STATUS);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Open folder", 0, 738, 294, 133, 28, IDC_BTN_OPEN);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"", 0, 12, 339, 859, 24, IDC_LBL_STATS);
            update_status(ctx);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"Depth Preview:", 0, 632, 8, 240, 18, 0);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER, 632, 30, 240, 240, IDC_PREVIEW_BOX);

            switch_tab(ctx, 0);
            populate_processes(ctx);
            RegisterHotKey(hwnd, kHotkeyRecordId, ctx->app->hotkey_record_mod, ctx->app->hotkey_record_vk);
            SetTimer(hwnd, kTimerId, 200, nullptr);
            return 0;
        }
        case WM_HSCROLL: {
            if (is_active(recording_status(*ctx->app))) return 0;
            HWND src = reinterpret_cast<HWND>(lp);
            if (src == GetDlgItem(hwnd, IDC_SLIDER_ZNEAR)) {
                const float zn = slider_to_znear(static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0)));
                wchar_t buf[32]{};
                swprintf_s(buf, L"%.3f", zn);
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_ZNEAR), buf);
                sync_ui_to_config(ctx);
            } else if (src == GetDlgItem(hwnd, IDC_SLIDER_ZFAR)) {
                const float zf = slider_to_zfar(static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0)));
                wchar_t buf[32]{};
                swprintf_s(buf, L"%.1f", zf);
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_ZFAR), buf);
                sync_ui_to_config(ctx);
            } else if (src == GetDlgItem(hwnd, IDC_SLIDER_QUAL)) {
                const int pos = static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0));
                wchar_t buf[16]{};
                swprintf_s(buf, L"%d%%", pos);
                SetWindowTextW(GetDlgItem(hwnd, IDC_LBL_QUAL), buf);
                sync_ui_to_config(ctx);
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == IDC_TAB_MAIN && nm->code == TCN_SELCHANGE) {
                switch_tab(ctx, TabCtrl_GetCurSel(nm->hwndFrom));
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (is_active(recording_status(*ctx->app)) && id != IDC_BTN_RECORD) return 0;
            if (id == IDC_PROC_COMBO && code == CBN_DROPDOWN) {
                populate_processes(ctx);
            } else if (id == IDC_BTN_ATTACH) {
                const LRESULT idx = SendMessageW(GetDlgItem(hwnd, IDC_PROC_COMBO), CB_GETCURSEL, 0, 0);
                if (idx >= 0 && static_cast<size_t>(idx) < ctx->procs.size() &&
                    ctx->app->config_view.get()->target_pid != static_cast<LONG>(ctx->procs[idx].pid)) {
                    if (ctx->app->config_view.get()->target_pid > 0) InterlockedIncrement(&ctx->app->config_view.get()->unload_seq);
                    set_target(ctx, static_cast<LONG>(ctx->procs[idx].pid));
                    std::wstring err;
                    if (!inject_dll(ctx->procs[idx].pid, ctx->app->hook_dll_path, &err)) {
                        ctx->app->log.write_w("UI", L"Attach failed for PID " + std::to_wstring(ctx->procs[idx].pid) +
                                              L": " + (err.empty() ? L"inject_dll failed" : err));
                        set_target(ctx, -1);
                        MessageBoxW(hwnd, err.empty() ? L"This process could not be attached." : err.c_str(), L"Depth Recorder", MB_ICONERROR);
                    } else {
                        ctx->vulkan_layer_missing = process_has_vulkan_without_layer(ctx->procs[idx].pid);
                        ctx->app->log.write("UI", "Attached to PID %lu (vulkan_layer_missing=%d implicit_reg=%d)",
                                            ctx->procs[idx].pid, ctx->vulkan_layer_missing ? 1 : 0,
                                            is_vulkan_implicit_layer_registered() ? 1 : 0);
                    }
                }
            } else if (id == IDC_BTN_DETACH) {
                ctx->app->log.write("UI", "Detached from target");
                InterlockedIncrement(&ctx->app->config_view.get()->unload_seq);
                set_target(ctx, -1);
            } else if (id == IDC_SWAP_COMBO && code == CBN_SELCHANGE && !is_active(recording_status(*ctx->app))) {
                const HWND combo = GetDlgItem(hwnd, IDC_SWAP_COMBO);
                const LRESULT row = SendMessageW(combo, CB_GETCURSEL, 0, 0);
                const LRESULT listed = row >= 0 ? SendMessageW(combo, CB_GETITEMDATA, row, 0) : CB_ERR;
                if (listed >= 0 && static_cast<size_t>(listed) < ctx->listed_windows.size()) {
                    ctx->manual_target = ctx->listed_windows[listed].target;
                    refresh_windows(ctx);
                }
            } else if (id == IDC_BTN_OPEN) {
                std::wstring folder;
                {
                    std::lock_guard lock(ctx->app->recording.mutex);
                    folder = ctx->app->recording.folder;
                }
                if (!folder.empty()) ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (id == IDC_BTN_LAUNCH) {
                wchar_t file[MAX_PATH]{};
                OPENFILENAMEW ofn{sizeof(ofn)};
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All Files\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    DWORD pid = 0;
                    std::wstring err;
                    if (ctx->app->config_view.get()->target_pid > 0) InterlockedIncrement(&ctx->app->config_view.get()->unload_seq);
                    set_target(ctx, 0);
                    if (!launch_and_inject(file, ctx->app->hook_dll_path, pid, &err)) {
                        set_target(ctx, -1);
                        MessageBoxW(hwnd, err.empty() ? L"Failed to launch target executable." : err.c_str(), L"Depth Recorder", MB_ICONERROR);
                    }
                }
            } else if (id == IDC_BTN_BROWSE) {
                BROWSEINFOW bi{};
                bi.hwndOwner = hwnd;
                bi.lpszTitle = L"Select Output Folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
                    wchar_t path[MAX_PATH]{};
                    if (SHGetPathFromIDListW(pidl, path)) {
                        SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_OUTDIR), path);
                        sync_ui_to_config(ctx);
                        save_settings_ini(*ctx->app, ctx->app->config_view.get());
                    }
                    CoTaskMemFree(pidl);
                }
            } else if (id == IDC_BTN_RECORD) {
                toggle_recording(ctx);
            } else if (id == IDC_BTN_CYCLE_DEPTH) {
                if (auto* cfg = ctx->app->config_view.get()) {
                    InterlockedIncrement(&cfg->cycle_depth_seq);
                    InterlockedIncrement(&cfg->capture_generation);
                }
            } else if (id == IDC_BTN_APPLY_HK) {
                sync_ui_to_config(ctx);
                const LRESULT r_hk = SendMessageW(GetDlgItem(hwnd, IDC_HK_RECORD), HKM_GETHOTKEY, 0, 0);
                if (LOWORD(r_hk) != 0) {
                    UnregisterHotKey(hwnd, kHotkeyRecordId);
                    ctx->app->hotkey_record_vk = LOBYTE(LOWORD(r_hk));
                    ctx->app->hotkey_record_mod = hk_mods_to_win32(HIBYTE(LOWORD(r_hk)));
                    RegisterHotKey(hwnd, kHotkeyRecordId, ctx->app->hotkey_record_mod, ctx->app->hotkey_record_vk);
                }
                save_settings_ini(*ctx->app, ctx->app->config_view.get());
            } else if (id == IDC_BTN_AUTOCONFIG) {
                run_auto_config(ctx, false);
            } else if ((id == IDC_EDIT_ZNEAR || id == IDC_EDIT_ZFAR) && code == EN_KILLFOCUS) {
                sync_ui_to_config(ctx);
                if (auto* cfg = ctx->app->config_view.get()) {
                    set_z_controls(hwnd, cfg->z_near, cfg->z_far);
                }
            } else if (id == IDC_RECFPS_COMBO && code == CBN_SELCHANGE) {
                const LRESULT sel = SendMessageW(GetDlgItem(hwnd, IDC_RECFPS_COMBO), CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    wchar_t fbuf[16]{};
                    SendMessageW(GetDlgItem(hwnd, IDC_RECFPS_COMBO), CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(fbuf));
                    SetWindowTextW(GetDlgItem(hwnd, IDC_RECFPS_COMBO), fbuf);
                }
                sync_ui_to_config(ctx);
                save_settings_ini(*ctx->app, ctx->app->config_view.get());
            } else if ((id == IDC_RECFPS_COMBO && code == CBN_KILLFOCUS) ||
                       id == IDC_MODE_COMBO || id == IDC_CORNER_COMBO ||
                       id == IDC_CHK_DEPTH || id == IDC_CHK_LINEAR ||
                       id == IDC_CHK_REVZ || id == IDC_CHK_OVERLAY || id == IDC_CHK_PREVIEW || id == IDC_CHK_DROP_DEAD) {
                sync_ui_to_config(ctx);
                save_settings_ini(*ctx->app, ctx->app->config_view.get());
            } else if (id == IDC_CHK_VK_LAYER) {
                const bool want = IsDlgButtonChecked(hwnd, IDC_CHK_VK_LAYER) == BST_CHECKED;
                if (!set_vulkan_implicit_layer_registered(want)) {
                    ctx->app->log.write("UI", "Failed to set Vulkan Implicit Layer (%d)", want ? 1 : 0);
                    MessageBoxW(hwnd, L"Could not update the HKCU Vulkan ImplicitLayers registry key.", L"Depth Recorder", MB_ICONERROR);
                } else {
                    ctx->app->log.write("UI", "Vulkan Implicit Layer %s", want ? "enabled" : "disabled");
                }
                CheckDlgButton(hwnd, IDC_CHK_VK_LAYER, is_vulkan_implicit_layer_registered() ? BST_CHECKED : BST_UNCHECKED);
            } else if (id == IDC_BTN_LOG_CLEAR) {
                ctx->app->log.clear_display();
                refresh_log_box(ctx);
            } else if (id == IDC_BTN_LOG_COPY) {
                HWND edit = GetDlgItem(hwnd, IDC_EDIT_LOG);
                const int len = GetWindowTextLengthW(edit);
                if (len > 0 && OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    if (HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(len + 1) * sizeof(wchar_t))) {
                        if (auto* dst = static_cast<wchar_t*>(GlobalLock(hmem))) {
                            GetWindowTextW(edit, dst, len + 1);
                            GlobalUnlock(hmem);
                            SetClipboardData(CF_UNICODETEXT, hmem);
                        } else {
                            GlobalFree(hmem);
                        }
                    }
                    CloseClipboard();
                }
            } else if (id == IDC_BTN_LOG_OPEN) {
                const auto path = ctx->app->log.path();
                if (!path.empty()) ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_HOTKEY: {
            if (wp == kHotkeyRecordId) toggle_recording(ctx);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_PREVIEW_BOX && ctx && ctx->app) {
                std::lock_guard lock(ctx->app->preview_mutex);
                FillRect(dis->hDC, &dis->rcItem, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                const int box_w = dis->rcItem.right - dis->rcItem.left;
                const int box_h = dis->rcItem.bottom - dis->rcItem.top;
                if (!ctx->app->preview_bgra.empty() && box_w > 0 && box_h > 0) {
                    const auto fit = fit_frame(ctx->app->preview_source_w, ctx->app->preview_source_h,
                                               static_cast<uint32_t>(box_w), static_cast<uint32_t>(box_h));
                    BITMAPINFO bmi{};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = static_cast<LONG>(ctx->app->preview_w);
                    bmi.bmiHeader.biHeight = -static_cast<LONG>(ctx->app->preview_h);
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode(dis->hDC, COLORONCOLOR);
                    StretchDIBits(dis->hDC, dis->rcItem.left + static_cast<int>(fit.left), dis->rcItem.top + static_cast<int>(fit.top),
                                  static_cast<int>(fit.width), static_cast<int>(fit.height),
                                  0, 0, static_cast<int>(ctx->app->preview_w), static_cast<int>(ctx->app->preview_h),
                                  ctx->app->preview_bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                }
                return TRUE;
            }
            return FALSE;
        }
        case WM_TIMER: {
            refresh_windows(ctx);
            update_status(ctx);
            refresh_log_box(ctx);
            if (HWND pv = GetDlgItem(hwnd, IDC_PREVIEW_BOX)) {
                InvalidateRect(pv, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DESTROY: {
            stop_recording(*ctx->app);
            save_settings_ini(*ctx->app, ctx->app->config_view.get());
            KillTimer(hwnd, kTimerId);
            UnregisterHotKey(hwnd, kHotkeyRecordId);
            if (ctx->font) DeleteObject(ctx->font);
            ctx->app->running.store(false);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int run_win32_ui(HINSTANCE hInstance, RecorderAppState& app) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hIcon = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DepthRecorderMainWnd";
    RegisterClassExW(&wc);

    UiContext ctx{&app};
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Depth Recorder",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 895, 404,
        nullptr, nullptr, hInstance, &ctx);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace dr
