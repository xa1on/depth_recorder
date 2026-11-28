#pragma once
#include "ui_win32.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <windows.h>

namespace dr {

inline std::wstring get_ini_path() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    return (std::filesystem::path(exe_path).parent_path() / L"depth_recorder.ini").wstring();
}

inline void load_settings_ini(RecorderAppState& app, SharedConfig* cfg) {
    const std::wstring ini = get_ini_path();
    wchar_t buf[512]{};
    if (GetPrivateProfileStringW(L"Settings", L"OutputDir", L"", buf, 512, ini.c_str()) > 0) {
        auto path = std::filesystem::path(buf);
        app.output_dir = (path.is_absolute() ? path : std::filesystem::path(ini).parent_path() / path).wstring();
    }
    app.recording_fps = std::clamp<uint32_t>(GetPrivateProfileIntW(L"Settings", L"RecordingFPS", 60, ini.c_str()), 1, 240);
    app.drop_dead_frames = GetPrivateProfileIntW(L"Settings", L"DropDeadFrames", 0, ini.c_str()) != 0;
    app.h264_quality = std::clamp<uint32_t>(GetPrivateProfileIntW(L"Settings", L"H264Quality", 85, ini.c_str()), 1, 100);
    if (GetPrivateProfileStringW(L"Settings", L"ExtraFFmpegFlags", L"", buf, 512, ini.c_str()) > 0) {
        app.extra_ffmpeg_flags = buf;
    }
    app.hotkey_record_vk = static_cast<UINT>(GetPrivateProfileIntW(L"Settings", L"RecordHotkeyVK", VK_F9, ini.c_str()));
    app.hotkey_record_mod = static_cast<UINT>(GetPrivateProfileIntW(L"Settings", L"RecordHotkeyMod", 0, ini.c_str()));

    if (cfg) {
        const UINT dm = std::min<UINT>(GetPrivateProfileIntW(L"Settings", L"DepthMode", 0, ini.c_str()), 2);
        InterlockedExchange(&cfg->depth_mode, static_cast<LONG>(dm));
        InterlockedExchange(&cfg->depth_enabled, GetPrivateProfileIntW(L"Settings", L"DepthEnabled", 1, ini.c_str()) ? 1 : 0);
        InterlockedExchange(&cfg->overlay_enabled, GetPrivateProfileIntW(L"Settings", L"OverlayEnabled", 0, ini.c_str()) ? 1 : 0);
        const UINT oc = std::min<UINT>(GetPrivateProfileIntW(L"Settings", L"OverlayCorner", 0, ini.c_str()), 3);
        InterlockedExchange(&cfg->overlay_corner, static_cast<LONG>(oc));
        const LONG pv = GetPrivateProfileIntW(L"Settings", L"PreviewEnabled", 1, ini.c_str()) ? 1 : 0;
        app.show_preview.store(pv != 0);
        InterlockedExchange(&cfg->target_rec_fps, static_cast<LONG>(app.recording_fps));
    }
}

inline void save_settings_ini(const RecorderAppState& app, const SharedConfig* cfg) {
    const std::wstring ini = get_ini_path();
    WritePrivateProfileStringW(L"Settings", L"OutputDir", app.output_dir.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"RecordingFPS", std::to_wstring(app.recording_fps).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"DropDeadFrames", app.drop_dead_frames ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"H264Quality", std::to_wstring(app.h264_quality).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"ExtraFFmpegFlags", app.extra_ffmpeg_flags.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"RecordHotkeyVK", std::to_wstring(app.hotkey_record_vk).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Settings", L"RecordHotkeyMod", std::to_wstring(app.hotkey_record_mod).c_str(), ini.c_str());
    if (cfg) {
        WritePrivateProfileStringW(L"Settings", L"DepthMode", std::to_wstring(cfg->depth_mode).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Settings", L"DepthEnabled", cfg->depth_enabled ? L"1" : L"0", ini.c_str());
        WritePrivateProfileStringW(L"Settings", L"OverlayEnabled", cfg->overlay_enabled ? L"1" : L"0", ini.c_str());
        WritePrivateProfileStringW(L"Settings", L"OverlayCorner", std::to_wstring(cfg->overlay_corner).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Settings", L"PreviewEnabled", app.show_preview ? L"1" : L"0", ini.c_str());
    }
}

} // namespace dr
