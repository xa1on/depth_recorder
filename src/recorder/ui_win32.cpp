#include "ui_win32.h"
#include "injector.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <vector>

namespace dr {

namespace {

enum ControlId : int {
    IDC_PROC_COMBO = 101,
    IDC_BTN_REFRESH = 102,
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
    IDC_HK_CYCLE = 118,
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
    IDC_EDIT_FFMPEG = 131,
};

constexpr int kHotkeyRecordId = 1;
constexpr int kHotkeyCycleId = 2;
constexpr UINT_PTR kTimerId = 1;

struct UiContext {
    RecorderAppState* app = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::vector<ProcessEntry> procs;
    std::vector<HWND> capture_ctrls;
    std::vector<HWND> settings_ctrls;
    UINT rec_mod = 0;
    UINT cyc_mod = 0;
    LONG last_auto_backend = 0;
    LONG last_swapchain_count = -1;
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
    int sel = 0;
    for (size_t i = 0; i < ctx->procs.size(); ++i) {
        const auto& p = ctx->procs[i];
        if (p.pid == prev_pid) sel = static_cast<int>(i);
        std::wstring label = p.name + L"  (PID " + std::to_wstring(p.pid) + L")";
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!ctx->procs.empty()) SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

void switch_tab(UiContext* ctx, int tab_idx) {
    for (HWND h : ctx->capture_ctrls) ShowWindow(h, tab_idx == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : ctx->settings_ctrls) ShowWindow(h, tab_idx == 1 ? SW_SHOW : SW_HIDE);
}

UINT hk_mods_to_win32(BYTE hk_mods) {
    UINT m = 0;
    if (hk_mods & HOTKEYF_ALT) m |= MOD_ALT;
    if (hk_mods & HOTKEYF_CONTROL) m |= MOD_CONTROL;
    if (hk_mods & HOTKEYF_SHIFT) m |= MOD_SHIFT;
    return m;
}

float slider_to_znear(int pos) {
    const float t = static_cast<float>(pos) / 1000.0f;
    return 0.01f * std::pow(1000.0f, t); // [0.01, 10.0]
}

int znear_to_slider(float zn) {
    zn = std::clamp(zn, 0.01f, 10.0f);
    return static_cast<int>(std::lround(1000.0f * (std::log10(zn / 0.01f) / 3.0f)));
}

float slider_to_zfar(int pos) {
    const float t = static_cast<float>(pos) / 1000.0f;
    return 10.0f * std::pow(1000.0f, t); // [10.0, 10000.0]
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
            // Adjust Z-Near / Z-Far empirically if the depth histogram is crushed black or washed white
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

    wchar_t dir_buf[MAX_PATH]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_OUTDIR), dir_buf, MAX_PATH);
    if (dir_buf[0]) ctx->app->output_dir = dir_buf;

    const LRESULT mode_idx = SendMessageW(GetDlgItem(ctx->hwnd, IDC_MODE_COMBO), CB_GETCURSEL, 0, 0);
    if (mode_idx >= 0 && mode_idx <= 2) {
        InterlockedExchange(&cfg->depth_mode, static_cast<LONG>(mode_idx));
    }
    const bool depth_en = (IsDlgButtonChecked(ctx->hwnd, IDC_CHK_DEPTH) == BST_CHECKED);
    const bool prev_en = (IsDlgButtonChecked(ctx->hwnd, IDC_CHK_PREVIEW) == BST_CHECKED);
    InterlockedExchange(&cfg->depth_enabled, depth_en ? 1 : 0);
    InterlockedExchange(&cfg->linearize, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_LINEAR) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->reversed_z, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_REVZ) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->overlay_enabled, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_OVERLAY) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->preview_enabled, (depth_en && prev_en) ? 1 : 0);
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

    ctx->app->h264_quality = static_cast<uint32_t>(SendMessageW(GetDlgItem(ctx->hwnd, IDC_SLIDER_QUAL), TBM_GETPOS, 0, 0));
    wchar_t ffbuf[256]{};
    GetWindowTextW(GetDlgItem(ctx->hwnd, IDC_EDIT_FFMPEG), ffbuf, 256);
    ctx->app->extra_ffmpeg_flags = ffbuf;
}

void toggle_recording(UiContext* ctx) {
    sync_ui_to_config(ctx);
    SharedConfig* cfg = ctx->app->config_view.get();
    if (!cfg) return;
    const LONG next = (cfg->recording == 0) ? 1 : 0;
    InterlockedExchange(&cfg->recording, next);
    SetWindowTextW(GetDlgItem(ctx->hwnd, IDC_BTN_RECORD), next ? L"Stop Recording (F9)" : L"Start Recording (F9)");
}

void update_status(UiContext* ctx) {
    SharedConfig* cfg = ctx->app->config_view.get();
    if (!cfg) return;
    const wchar_t* backend = L"None";
    switch (cfg->active_backend) {
        case 1: backend = L"DX11"; break;
        case 2: backend = L"OpenGL"; break;
        case 3: backend = L"Vulkan"; break;
        case 4: backend = L"DX12"; break;
    }
    wchar_t buf[256]{};
    swprintf_s(buf, L"State: %s | Backend: %s (PID %ld) | FPS: %.1f | Depth: %ld/%ld | Frames: %llu",
               cfg->recording ? L"RECORDING" : L"Idle",
               backend, cfg->hooked_pid,
               static_cast<double>(cfg->current_fps_x10) / 10.0,
               cfg->candidate_depth_count > 0 ? (cfg->selected_depth_index + 1) : 0,
               cfg->candidate_depth_count,
               static_cast<unsigned long long>(ctx->app->encoder.frames_written()));
    SetWindowTextW(GetDlgItem(ctx->hwnd, IDC_LBL_STATUS), buf);
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

            NONCLIENTMETRICSW ncm{sizeof(ncm)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            ctx->font = CreateFontIndirectW(&ncm.lfMessageFont);

            HWND tab = add_ctrl(hwnd, ctx->font, WC_TABCONTROLW, L"", 0, 8, 6, 614, 308, IDC_TAB_MAIN);
            TCITEMW tie{};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<wchar_t*>(L"Capture");
            TabCtrl_InsertItem(tab, 0, &tie);
            tie.pszText = const_cast<wchar_t*>(L"Settings");
            TabCtrl_InsertItem(tab, 1, &tie);

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

            cap(L"STATIC", L"Target Process:", 0, 12, 14, 100, 20, 0);
            cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 115, 10, 250, 240, IDC_PROC_COMBO);
            cap(L"BUTTON", L"Attach", 0, 372, 10, 68, 24, IDC_BTN_ATTACH);
            cap(L"BUTTON", L"Detach", 0, 446, 10, 68, 24, IDC_BTN_DETACH);
            cap(L"BUTTON", L"Launch EXE...", 0, 520, 10, 96, 24, IDC_BTN_LAUNCH);

            cap(L"STATIC", L"Target Window:", 0, 12, 46, 100, 20, 0);
            cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 115, 42, 501, 200, IDC_SWAP_COMBO);

            cap(L"STATIC", L"Output Folder:", 0, 12, 78, 100, 20, 0);
            cap(L"EDIT", ctx->app->output_dir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 115, 76, 425, 22, IDC_EDIT_OUTDIR);
            cap(L"BUTTON", L"Browse...", 0, 546, 75, 70, 24, IDC_BTN_BROWSE);

            cap(L"STATIC", L"Depth Format:", 0, 12, 112, 100, 20, 0);
            HWND mode_cb = cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 115, 108, 240, 120, IDC_MODE_COMBO);
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gray8 (H.264 MP4)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ColorPhase (H.264 MP4, Lossy-Safe)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"RGB24 Lossless (UtVideo MKV, 16.7M)"));
            SendMessageW(mode_cb, CB_SETCURSEL, 2, 0);

            cap(L"STATIC", L"Overlay Corner:", 0, 372, 112, 95, 20, 0);
            HWND corner_cb = cap(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 470, 108, 146, 120, IDC_CORNER_COMBO);
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Right"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Right"));
            SendMessageW(corner_cb, CB_SETCURSEL, 0, 0);

            HWND c1 = cap(L"BUTTON", L"Enable Depth Pass", BS_AUTOCHECKBOX, 12, 146, 135, 22, IDC_CHK_DEPTH);
            HWND c2 = cap(L"BUTTON", L"Linearize (Gray8/Phase)", BS_AUTOCHECKBOX, 155, 146, 165, 22, IDC_CHK_LINEAR);
            HWND c3 = cap(L"BUTTON", L"Reversed-Z", BS_AUTOCHECKBOX, 330, 146, 100, 22, IDC_CHK_REVZ);
            HWND c4 = cap(L"BUTTON", L"In-Game FPS Overlay", BS_AUTOCHECKBOX, 445, 146, 165, 22, IDC_CHK_OVERLAY);
            SendMessageW(c1, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c2, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c3, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c4, BM_SETCHECK, BST_CHECKED, 0);

            cap(L"STATIC", L"Z-Near:", 0, 12, 178, 52, 20, 0);
            cap(L"EDIT", L"0.100", WS_BORDER | ES_AUTOHSCROLL, 66, 175, 80, 22, IDC_EDIT_ZNEAR);
            HWND s_zn = cap(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 12, 200, 190, 24, IDC_SLIDER_ZNEAR);
            SendMessageW(s_zn, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessageW(s_zn, TBM_SETPOS, TRUE, znear_to_slider(0.1f));

            cap(L"STATIC", L"Z-Far:", 0, 220, 178, 46, 20, 0);
            cap(L"EDIT", L"500.0", WS_BORDER | ES_AUTOHSCROLL, 268, 175, 86, 22, IDC_EDIT_ZFAR);
            HWND s_zf = cap(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 220, 200, 190, 24, IDC_SLIDER_ZFAR);
            SendMessageW(s_zf, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessageW(s_zf, TBM_SETPOS, TRUE, zfar_to_slider(500.0f));

            cap(L"BUTTON", L"Auto Config", 0, 424, 180, 192, 34, IDC_BTN_AUTOCONFIG);

            cap(L"BUTTON", L"Start Recording (F9)", BS_DEFPUSHBUTTON, 12, 234, 295, 32, IDC_BTN_RECORD);
            cap(L"BUTTON", L"Cycle Depth Buffer (F10)", 0, 321, 234, 295, 32, IDC_BTN_CYCLE_DEPTH);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"State: Idle", SS_SUNKEN, 12, 320, 604, 24, IDC_LBL_STATUS);

            // Settings tab controls
            set(L"STATIC", L"Record Toggle Keybind:", 0, 20, 24, 160, 20, 0);
            HWND hk_rec = set(HOTKEY_CLASSW, L"", WS_BORDER, 190, 20, 200, 24, IDC_HK_RECORD);
            SendMessageW(hk_rec, HKM_SETHOTKEY, MAKEWORD(ctx->app->hotkey_record_vk, 0), 0);

            set(L"STATIC", L"Cycle Depth Keybind:", 0, 20, 60, 160, 20, 0);
            HWND hk_cyc = set(HOTKEY_CLASSW, L"", WS_BORDER, 190, 60, 200, 24, IDC_HK_CYCLE);
            SendMessageW(hk_cyc, HKM_SETHOTKEY, MAKEWORD(ctx->app->hotkey_cycle_vk, 0), 0);

            HWND c_prev = set(L"BUTTON", L"Live Depth Preview", BS_AUTOCHECKBOX, 190, 96, 160, 22, IDC_CHK_PREVIEW);
            SendMessageW(c_prev, BM_SETCHECK, BST_CHECKED, 0);

            set(L"STATIC", L"H.264 Quality:", 0, 20, 130, 160, 20, 0);
            HWND s_qual = set(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 190, 126, 200, 24, IDC_SLIDER_QUAL);
            SendMessageW(s_qual, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
            SendMessageW(s_qual, TBM_SETPOS, TRUE, static_cast<LPARAM>(ctx->app->h264_quality));
            set(L"STATIC", L"85%", 0, 398, 130, 50, 20, IDC_LBL_QUAL);

            set(L"STATIC", L"Extra FFmpeg Flags:", 0, 20, 164, 160, 20, 0);
            set(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 190, 161, 300, 22, IDC_EDIT_FFMPEG);

            set(L"BUTTON", L"Apply Keybinds / Settings", 0, 190, 196, 200, 28, IDC_BTN_APPLY_HK);

            add_ctrl(hwnd, ctx->font, L"STATIC", L"Depth Preview:", 0, 632, 8, 240, 18, 0);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"", SS_OWNERDRAW | WS_BORDER, 632, 30, 240, 240, IDC_PREVIEW_BOX);
            switch_tab(ctx, 0);

            populate_processes(ctx);
            RegisterHotKey(hwnd, kHotkeyRecordId, 0, ctx->app->hotkey_record_vk);
            RegisterHotKey(hwnd, kHotkeyCycleId, 0, ctx->app->hotkey_cycle_vk);
            SetTimer(hwnd, kTimerId, 200, nullptr);
            return 0;
        }
        case WM_HSCROLL: {
            HWND src = reinterpret_cast<HWND>(lp);
            if (src == GetDlgItem(hwnd, IDC_SLIDER_ZNEAR)) {
                const int pos = static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0));
                const float zn = slider_to_znear(pos);
                wchar_t buf[32]{};
                swprintf_s(buf, L"%.3f", zn);
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_ZNEAR), buf);
                sync_ui_to_config(ctx);
            } else if (src == GetDlgItem(hwnd, IDC_SLIDER_ZFAR)) {
                const int pos = static_cast<int>(SendMessageW(src, TBM_GETPOS, 0, 0));
                const float zf = slider_to_zfar(pos);
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
            if (id == IDC_PROC_COMBO && code == CBN_DROPDOWN) {
                populate_processes(ctx);
            } else if (id == IDC_BTN_ATTACH) {
                const LRESULT idx = SendMessageW(GetDlgItem(hwnd, IDC_PROC_COMBO), CB_GETCURSEL, 0, 0);
                if (idx >= 0 && static_cast<size_t>(idx) < ctx->procs.size()) {
                    std::wstring err;
                    if (!inject_dll(ctx->procs[idx].pid, ctx->app->hook_dll_path, &err)) {
                        MessageBoxW(hwnd, err.empty() ? L"Failed to inject depth_hook.dll." : err.c_str(), L"Depth Recorder", MB_ICONERROR);
                    }
                }
            } else if (id == IDC_BTN_DETACH) {
                if (auto* cfg = ctx->app->config_view.get()) {
                    InterlockedExchange(&cfg->recording, 0);
                    InterlockedIncrement(&cfg->unload_seq);
                    InterlockedExchange(&cfg->hooked_pid, 0);
                    InterlockedExchange(&cfg->active_backend, 0);
                    InterlockedExchange(&cfg->candidate_depth_count, 0);
                    InterlockedExchange(&cfg->swapchain_count, 0);
                    InterlockedExchange(&cfg->selected_swapchain_index, 0);
                    ctx->last_auto_backend = 0;
                    ctx->last_swapchain_count = -1;
                    SetWindowTextW(GetDlgItem(hwnd, IDC_BTN_RECORD), L"Start Recording (F9)");
                    std::lock_guard lock(ctx->app->preview_mutex);
                    ctx->app->preview_bgra.clear();
                }
            } else if (id == IDC_SWAP_COMBO && code == CBN_SELCHANGE) {
                const LRESULT idx = SendMessageW(GetDlgItem(hwnd, IDC_SWAP_COMBO), CB_GETCURSEL, 0, 0);
                if (idx >= 0) {
                    if (auto* cfg = ctx->app->config_view.get()) {
                        InterlockedExchange(&cfg->selected_swapchain_index, static_cast<LONG>(idx));
                    }
                }
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
                    if (!launch_and_inject(file, ctx->app->hook_dll_path, pid, &err)) {
                        MessageBoxW(hwnd, err.empty() ? L"Failed to launch and inject target executable." : err.c_str(), L"Depth Recorder", MB_ICONERROR);
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
                    }
                    CoTaskMemFree(pidl);
                }
            } else if (id == IDC_BTN_RECORD) {
                toggle_recording(ctx);
            } else if (id == IDC_BTN_CYCLE_DEPTH) {
                if (auto* cfg = ctx->app->config_view.get()) InterlockedIncrement(&cfg->cycle_depth_seq);
            } else if (id == IDC_BTN_APPLY_HK) {
                sync_ui_to_config(ctx);
                const LRESULT r_hk = SendMessageW(GetDlgItem(hwnd, IDC_HK_RECORD), HKM_GETHOTKEY, 0, 0);
                const LRESULT c_hk = SendMessageW(GetDlgItem(hwnd, IDC_HK_CYCLE), HKM_GETHOTKEY, 0, 0);
                if (LOWORD(r_hk) != 0) {
                    UnregisterHotKey(hwnd, kHotkeyRecordId);
                    ctx->app->hotkey_record_vk = LOBYTE(LOWORD(r_hk));
                    ctx->rec_mod = hk_mods_to_win32(HIBYTE(LOWORD(r_hk)));
                    RegisterHotKey(hwnd, kHotkeyRecordId, ctx->rec_mod, ctx->app->hotkey_record_vk);
                }
                if (LOWORD(c_hk) != 0) {
                    UnregisterHotKey(hwnd, kHotkeyCycleId);
                    ctx->app->hotkey_cycle_vk = LOBYTE(LOWORD(c_hk));
                    ctx->cyc_mod = hk_mods_to_win32(HIBYTE(LOWORD(c_hk)));
                    RegisterHotKey(hwnd, kHotkeyCycleId, ctx->cyc_mod, ctx->app->hotkey_cycle_vk);
                }
            } else if (id == IDC_BTN_AUTOCONFIG) {
                run_auto_config(ctx, false);
            } else if ((id == IDC_EDIT_ZNEAR || id == IDC_EDIT_ZFAR) && code == EN_KILLFOCUS) {
                sync_ui_to_config(ctx);
                if (auto* cfg = ctx->app->config_view.get()) {
                    set_z_controls(hwnd, cfg->z_near, cfg->z_far);
                }
            } else if (id == IDC_MODE_COMBO || id == IDC_CORNER_COMBO ||
                       id == IDC_CHK_DEPTH || id == IDC_CHK_LINEAR ||
                       id == IDC_CHK_REVZ || id == IDC_CHK_OVERLAY || id == IDC_CHK_PREVIEW) {
                sync_ui_to_config(ctx);
            }
            return 0;
        }
        case WM_HOTKEY: {
            if (wp == kHotkeyRecordId) toggle_recording(ctx);
            else if (wp == kHotkeyCycleId) {
                if (auto* cfg = ctx->app->config_view.get()) InterlockedIncrement(&cfg->cycle_depth_seq);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis->CtlID == IDC_PREVIEW_BOX && ctx && ctx->app) {
                std::lock_guard lock(ctx->app->preview_mutex);
                if (!ctx->app->preview_bgra.empty() && ctx->app->preview_w > 0 && ctx->app->preview_h > 0) {
                    BITMAPINFO bmi{};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = static_cast<LONG>(ctx->app->preview_w);
                    bmi.bmiHeader.biHeight = -static_cast<LONG>(ctx->app->preview_h);
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;
                    SetStretchBltMode(dis->hDC, COLORONCOLOR);
                    StretchDIBits(dis->hDC, 0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top,
                                  0, 0, static_cast<int>(ctx->app->preview_w), static_cast<int>(ctx->app->preview_h),
                                  ctx->app->preview_bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                } else {
                    FillRect(dis->hDC, &dis->rcItem, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                }
                return TRUE;
            }
            return FALSE;
        }
        case WM_TIMER: {
            if (auto* cfg = ctx->app->config_view.get()) {
                if (cfg->active_backend != 0 && cfg->active_backend != ctx->last_auto_backend) {
                    ctx->last_auto_backend = cfg->active_backend;
                    run_auto_config(ctx, true);
                }
                if (cfg->swapchain_count != ctx->last_swapchain_count) {
                    ctx->last_swapchain_count = cfg->swapchain_count;
                    HWND sc_cb = GetDlgItem(hwnd, IDC_SWAP_COMBO);
                    SendMessageW(sc_cb, CB_RESETCONTENT, 0, 0);
                    const LONG cnt = std::clamp<LONG>(static_cast<LONG>(cfg->swapchain_count), 0L, static_cast<LONG>(kMaxSwapchains));
                    for (LONG i = 0; i < cnt; ++i) {
                        wchar_t sbuf[128]{};
                        swprintf_s(sbuf, L"[%ld] %ux%u %s", i, cfg->swapchains[i].width, cfg->swapchains[i].height,
                                   cfg->swapchains[i].title[0] ? cfg->swapchains[i].title : L"(Untitled)");
                        SendMessageW(sc_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sbuf));
                    }
                    if (cnt > 0) {
                        const LONG sel = std::clamp<LONG>(static_cast<LONG>(cfg->selected_swapchain_index), 0L, cnt - 1);
                        SendMessageW(sc_cb, CB_SETCURSEL, sel, 0);
                    }
                }
            }
            update_status(ctx);
            if (HWND pv = GetDlgItem(hwnd, IDC_PREVIEW_BOX)) {
                InvalidateRect(pv, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hwnd, kTimerId);
            UnregisterHotKey(hwnd, kHotkeyRecordId);
            UnregisterHotKey(hwnd, kHotkeyCycleId);
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
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DepthRecorderMainWnd";
    RegisterClassExW(&wc);

    UiContext ctx{&app};
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Depth Recorder (Portable)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 895, 388,
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

