#include "ui_win32.h"
#include "injector.h"
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
};

constexpr int kHotkeyRecordId = 1;
constexpr int kHotkeyCycleId = 2;
constexpr UINT_PTR kTimerId = 1;

struct UiContext {
    RecorderAppState* app = nullptr;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    std::vector<ProcessEntry> procs;
};

void populate_processes(UiContext* ctx) {
    HWND combo = GetDlgItem(ctx->hwnd, IDC_PROC_COMBO);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    ctx->procs = list_processes();
    for (const auto& p : ctx->procs) {
        std::wstring label = p.name + L"  (PID " + std::to_wstring(p.pid) + L")";
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!ctx->procs.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
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
    InterlockedExchange(&cfg->depth_enabled, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_DEPTH) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->linearize, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_LINEAR) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->reversed_z, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_REVZ) == BST_CHECKED ? 1 : 0);
    InterlockedExchange(&cfg->overlay_enabled, IsDlgButtonChecked(ctx->hwnd, IDC_CHK_OVERLAY) == BST_CHECKED ? 1 : 0);

    const LRESULT corner_idx = SendMessageW(GetDlgItem(ctx->hwnd, IDC_CORNER_COMBO), CB_GETCURSEL, 0, 0);
    if (corner_idx >= 0 && corner_idx <= 3) {
        InterlockedExchange(&cfg->overlay_corner, static_cast<LONG>(corner_idx));
    }
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

            add_ctrl(hwnd, ctx->font, L"STATIC", L"Target Process:", 0, 12, 14, 100, 20, 0);
            add_ctrl(hwnd, ctx->font, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 115, 10, 250, 240, IDC_PROC_COMBO);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Refresh", 0, 372, 10, 68, 24, IDC_BTN_REFRESH);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Attach", 0, 446, 10, 68, 24, IDC_BTN_ATTACH);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Launch EXE...", 0, 520, 10, 96, 24, IDC_BTN_LAUNCH);

            add_ctrl(hwnd, ctx->font, L"STATIC", L"Output Folder:", 0, 12, 46, 100, 20, 0);
            add_ctrl(hwnd, ctx->font, L"EDIT", ctx->app->output_dir.c_str(), WS_BORDER | ES_AUTOHSCROLL, 115, 44, 425, 22, IDC_EDIT_OUTDIR);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Browse...", 0, 546, 43, 70, 24, IDC_BTN_BROWSE);

            add_ctrl(hwnd, ctx->font, L"STATIC", L"Depth Format:", 0, 12, 80, 100, 20, 0);
            HWND mode_cb = add_ctrl(hwnd, ctx->font, L"COMBOBOX", L"", CBS_DROPDOWNLIST, 115, 76, 240, 120, IDC_MODE_COMBO);
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Gray8 (H.264 MP4)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ColorPhase (H.264 MP4, Lossy-Safe)"));
            SendMessageW(mode_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"RGB24 Lossless (UtVideo MKV, 16.7M)"));
            SendMessageW(mode_cb, CB_SETCURSEL, 2, 0);

            add_ctrl(hwnd, ctx->font, L"STATIC", L"Overlay Corner:", 0, 372, 80, 95, 20, 0);
            HWND corner_cb = add_ctrl(hwnd, ctx->font, L"COMBOBOX", L"", CBS_DROPDOWNLIST, 470, 76, 146, 120, IDC_CORNER_COMBO);
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Right"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Left"));
            SendMessageW(corner_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom-Right"));
            SendMessageW(corner_cb, CB_SETCURSEL, 0, 0);

            HWND c1 = add_ctrl(hwnd, ctx->font, L"BUTTON", L"Enable Depth Pass", BS_AUTOCHECKBOX, 12, 114, 135, 22, IDC_CHK_DEPTH);
            HWND c2 = add_ctrl(hwnd, ctx->font, L"BUTTON", L"Linearize (Gray8/Phase)", BS_AUTOCHECKBOX, 155, 114, 165, 22, IDC_CHK_LINEAR);
            HWND c3 = add_ctrl(hwnd, ctx->font, L"BUTTON", L"Reversed-Z", BS_AUTOCHECKBOX, 330, 114, 100, 22, IDC_CHK_REVZ);
            HWND c4 = add_ctrl(hwnd, ctx->font, L"BUTTON", L"In-Game FPS Overlay", BS_AUTOCHECKBOX, 445, 114, 165, 22, IDC_CHK_OVERLAY);
            SendMessageW(c1, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c2, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c3, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(c4, BM_SETCHECK, BST_CHECKED, 0);

            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Start Recording (F9)", BS_DEFPUSHBUTTON, 12, 148, 295, 32, IDC_BTN_RECORD);
            add_ctrl(hwnd, ctx->font, L"BUTTON", L"Cycle Depth Buffer (F10)", 0, 321, 148, 295, 32, IDC_BTN_CYCLE_DEPTH);
            add_ctrl(hwnd, ctx->font, L"STATIC", L"State: Idle", SS_SUNKEN, 12, 192, 604, 24, IDC_LBL_STATUS);

            populate_processes(ctx);
            RegisterHotKey(hwnd, kHotkeyRecordId, 0, ctx->app->hotkey_record_vk);
            RegisterHotKey(hwnd, kHotkeyCycleId, 0, ctx->app->hotkey_cycle_vk);
            SetTimer(hwnd, kTimerId, 200, nullptr);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IDC_BTN_REFRESH) {
                populate_processes(ctx);
            } else if (id == IDC_BTN_ATTACH) {
                const LRESULT idx = SendMessageW(GetDlgItem(hwnd, IDC_PROC_COMBO), CB_GETCURSEL, 0, 0);
                if (idx >= 0 && static_cast<size_t>(idx) < ctx->procs.size()) {
                    std::wstring err;
                    if (!inject_dll(ctx->procs[idx].pid, ctx->app->hook_dll_path, &err)) {
                        MessageBoxW(hwnd, err.empty() ? L"Failed to inject depth_hook.dll." : err.c_str(), L"Depth Recorder", MB_ICONERROR);
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
            } else if (id == IDC_MODE_COMBO || id == IDC_CORNER_COMBO ||
                       id == IDC_CHK_DEPTH || id == IDC_CHK_LINEAR ||
                       id == IDC_CHK_REVZ || id == IDC_CHK_OVERLAY) {
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
        case WM_TIMER: {
            update_status(ctx);
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
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
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
        CW_USEDEFAULT, CW_USEDEFAULT, 645, 265,
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

