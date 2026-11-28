#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace dr {

struct ProcessEntry {
    DWORD pid = 0;
    std::wstring name;
};

std::vector<ProcessEntry> list_processes();
bool process_has_vulkan_without_layer(DWORD pid);
bool inject_dll(DWORD pid, const std::wstring& dll_path, std::wstring* out_error = nullptr);
bool launch_and_inject(const std::wstring& exe_path, const std::wstring& dll_path, DWORD& out_pid, std::wstring* out_error = nullptr);

} // namespace dr

