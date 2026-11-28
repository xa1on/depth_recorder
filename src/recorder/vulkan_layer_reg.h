#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace dr {

inline constexpr const wchar_t* kVkImplicitLayersSubkey = LR"(SOFTWARE\Khronos\Vulkan\ImplicitLayers)";

inline std::wstring find_vulkan_layer_manifest() {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto recorder_dir = std::filesystem::path(exe_path).parent_path();
    auto manifest = recorder_dir / L"vulkan" / L"x64" / L"VkLayer_depth_recorder.json";
    if (!std::filesystem::exists(manifest)) {
        manifest = recorder_dir / L"VkLayer_depth_recorder.json";
    }
    return std::filesystem::exists(manifest) ? manifest.wstring() : L"";
}

inline bool is_vulkan_implicit_layer_registered() {
    const std::wstring manifest = find_vulkan_layer_manifest();
    if (manifest.empty()) return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kVkImplicitLayersSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, val = 1, size = sizeof(val);
    const LONG rc = RegQueryValueExW(key, manifest.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&val), &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_DWORD && val == 0;
}

inline bool set_vulkan_implicit_layer_registered(bool enable) {
    HKEY key = nullptr;
    const DWORD disp_open = enable
        ? RegCreateKeyExW(HKEY_CURRENT_USER, kVkImplicitLayersSubkey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, kVkImplicitLayersSubkey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (disp_open != ERROR_SUCCESS) return !enable;

    // Remove any existing VkLayer_depth_recorder.json entries (handles moved portable folders cleanly).
    std::vector<std::wstring> to_delete;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024]{};
        DWORD name_len = static_cast<DWORD>(std::size(name));
        const LONG rc = RegEnumValueW(key, i, name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc == ERROR_SUCCESS) {
            std::wstring s(name, name_len);
            if (s.ends_with(L"VkLayer_depth_recorder.json")) to_delete.push_back(std::move(s));
        }
    }
    for (const auto& name : to_delete) {
        RegDeleteValueW(key, name.c_str());
    }

    bool ok = true;
    if (enable) {
        const std::wstring manifest = find_vulkan_layer_manifest();
        if (manifest.empty()) {
            ok = false;
        } else {
            const DWORD zero = 0;
            ok = RegSetValueExW(key, manifest.c_str(), 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&zero), sizeof(zero)) == ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return ok;
}

} // namespace dr
