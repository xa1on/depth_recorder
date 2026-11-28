#include "injector.h"
#include <algorithm>
#include <filesystem>
#include <psapi.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <cwchar>

namespace dr {

namespace {

std::wstring format_win32_error(const wchar_t* step, DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out = std::wstring(step) + L" failed (Error " + std::to_wstring(err);
    if (msg) {
        std::wstring sys(msg);
        while (!sys.empty() && (sys.back() == L'\r' || sys.back() == L'\n')) sys.pop_back();
        out += L": " + sys;
        LocalFree(msg);
    }
    out += L")";
    if (err == ERROR_ACCESS_DENIED) {
        out += L"\n\nLikely protected by anti-cheat (e.g. Hyperion/EAC/BattlEye) or running as Administrator.";
    }
    return out;
}

bool is_compatible_process(DWORD pid, DWORD self_pid) {
    if (pid <= 4 || pid == self_pid) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    BOOL is_wow64 = FALSE;
    if (!IsWow64Process(proc, &is_wow64)) {
        CloseHandle(proc);
        return false;
    }
    if (is_wow64) {
        wchar_t exe_dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        const auto p32 = std::filesystem::path(exe_dir).parent_path() / L"depth_hook32.dll";
        if (!std::filesystem::exists(p32)) {
            CloseHandle(proc);
            return false;
        }
    }

    HMODULE mods[256]{};
    DWORD needed = 0;
    bool has_gfx = false;
    if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 256);
        wchar_t mod_name[MAX_PATH]{};
        for (DWORD i = 0; i < count; ++i) {
            if (GetModuleBaseNameW(proc, mods[i], mod_name, MAX_PATH)) {
                if (_wcsicmp(mod_name, L"d3d11.dll") == 0 ||
                    _wcsicmp(mod_name, L"d3d12.dll") == 0 ||
                    _wcsicmp(mod_name, L"dxgi.dll") == 0 ||
                    _wcsicmp(mod_name, L"opengl32.dll") == 0 ||
                    _wcsicmp(mod_name, L"vulkan-1.dll") == 0) {
                    has_gfx = true;
                    break;
                }
            }
        }
    }
    CloseHandle(proc);
    return has_gfx;
}

} // namespace

std::vector<ProcessEntry> list_processes() {
    std::unordered_map<DWORD, std::wstring> pid_titles;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[128]{};
        if (GetWindowTextW(hwnd, title, 128) <= 0) return TRUE;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        auto* map = reinterpret_cast<std::unordered_map<DWORD, std::wstring>*>(lp);
        if (wpid && map->find(wpid) == map->end()) {
            (*map)[wpid] = title;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&pid_titles));

    std::vector<ProcessEntry> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    const DWORD self_pid = GetCurrentProcessId();
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (is_compatible_process(pe.th32ProcessID, self_pid)) {
                std::wstring label = pe.szExeFile;
                auto it = pid_titles.find(pe.th32ProcessID);
                if (it != pid_titles.end() && !it->second.empty()) {
                    label += L" - " + it->second;
                }
                list.push_back({pe.th32ProcessID, label});
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(list.begin(), list.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return list;
}

bool process_has_vulkan_without_layer(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;
    HMODULE mods[512]{};
    DWORD needed = 0;
    bool has_vk = false, has_layer = false;
    if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 512);
        wchar_t mod_name[MAX_PATH]{};
        for (DWORD i = 0; i < count; ++i) {
            if (GetModuleBaseNameW(proc, mods[i], mod_name, MAX_PATH)) {
                if (_wcsicmp(mod_name, L"vulkan-1.dll") == 0) has_vk = true;
                else if (_wcsicmp(mod_name, L"VkLayer_depth_recorder.dll") == 0) has_layer = true;
            }
        }
    }
    CloseHandle(proc);
    return has_vk && !has_layer;
}

bool inject_dll(DWORD pid, const std::wstring& dll_path, std::wstring* out_error) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        if (out_error) *out_error = format_win32_error(L"OpenProcess", GetLastError());
        return false;
    }

    BOOL is_wow64 = FALSE;
    if (IsWow64Process(proc, &is_wow64) && is_wow64) {
        CloseHandle(proc);
        const auto dir = std::filesystem::path(dll_path).parent_path();
        const auto inj32 = dir / L"inject32.exe";
        const auto dll32 = dir / L"depth_hook32.dll";
        if (!std::filesystem::exists(inj32) || !std::filesystem::exists(dll32)) {
            if (out_error) *out_error = L"This package supports 64-bit targets. A build with 32-bit capture support is needed for this process.";
            return false;
        }
        std::wstring cmd = L"\"" + inj32.wstring() + L"\" " + std::to_wstring(pid) + L" \"" + dll32.wstring() + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            if (out_error) *out_error = format_win32_error(L"inject32.exe", GetLastError());
            return false;
        }
        WaitForSingleObject(pi.hProcess, 6000);
        DWORD rc = 1;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (rc != 0 && out_error) *out_error = format_win32_error(L"inject32", rc);
        return rc == 0;
    }

    const size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        if (out_error) *out_error = format_win32_error(L"VirtualAllocEx", GetLastError());
        CloseHandle(proc);
        return false;
    }

    bool ok = WriteProcessMemory(proc, remote_mem, dll_path.c_str(), bytes, nullptr) != FALSE;
    if (!ok) {
        if (out_error) *out_error = format_win32_error(L"WriteProcessMemory", GetLastError());
    } else {
        auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, load_lib, remote_mem, 0, nullptr);
        if (thread) {
            const DWORD wait = WaitForSingleObject(thread, 5000);
            if (wait != WAIT_OBJECT_0) {
                const DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                ok = false;
                // The loader thread may still be reading the remote path.
                remote_mem = nullptr;
                if (out_error) *out_error = format_win32_error(L"Waiting for the capture DLL to load", error);
            } else {
                DWORD remote_ret = 0;
                if (!GetExitCodeThread(thread, &remote_ret)) {
                    ok = false;
                    if (out_error) *out_error = format_win32_error(L"GetExitCodeThread", GetLastError());
                } else if (remote_ret == 0) {
                    ok = false;
                    if (out_error) *out_error = L"LoadLibraryW returned NULL inside target process (architecture mismatch or blocked DLL).";
                }
            }
            CloseHandle(thread);
        } else {
            ok = false;
            if (out_error) *out_error = format_win32_error(L"CreateRemoteThread", GetLastError());
        }
    }

    if (remote_mem) VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

bool launch_and_inject(const std::wstring& exe_path, const std::wstring& dll_path, DWORD& out_pid, std::wstring* out_error) {
    const std::wstring dir = std::filesystem::path(exe_path).parent_path().wstring();
    const auto recorder_dir = std::filesystem::path(dll_path).parent_path();
    DWORD binary_type = SCS_64BIT_BINARY;
    GetBinaryTypeW(exe_path.c_str(), &binary_type);
    if (binary_type == SCS_32BIT_BINARY &&
        (!std::filesystem::exists(recorder_dir / L"inject32.exe") ||
         !std::filesystem::exists(recorder_dir / L"depth_hook32.dll"))) {
        if (out_error) *out_error = L"This package supports 64-bit targets. A build with 32-bit capture support is needed for this process.";
        return false;
    }
    const auto layer_dir = recorder_dir / (binary_type == SCS_32BIT_BINARY ? L"vulkan/x86" : L"vulkan/x64");
    auto layer_manifest = layer_dir / L"VkLayer_depth_recorder.json";
    if (!std::filesystem::exists(layer_manifest)) layer_manifest = recorder_dir / layer_manifest.filename();
    const bool add_capture_layer = std::filesystem::exists(layer_manifest);

    // Add the bundled explicit layer for this child only.
    std::vector<std::wstring> entries;
    std::wstring layers = L"VK_LAYER_DEPTH_RECORDER";
    // A manifest path lets the Windows loader skip directory enumeration, which can fail for Unicode paths.
    std::wstring layer_paths = layer_manifest.wstring();
    std::wstring override_paths;
    if (const auto env = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = env; *entry; entry += std::wcslen(entry) + 1) {
            std::wstring value(entry);
            const auto separator = value.find(L'=', 1);
            if (add_capture_layer && separator != std::wstring::npos) {
                const auto key = value.substr(0, separator);
                if (_wcsicmp(key.c_str(), L"VK_INSTANCE_LAYERS") == 0) {
                    layers += L";" + value.substr(separator + 1);
                    continue;
                }
                if (_wcsicmp(key.c_str(), L"VK_ADD_LAYER_PATH") == 0) {
                    layer_paths += L";" + value.substr(separator + 1);
                    continue;
                }
                if (_wcsicmp(key.c_str(), L"VK_LAYER_PATH") == 0) {
                    override_paths = value.substr(separator + 1);
                    continue;
                }
            }
            entries.push_back(std::move(value));
        }
        FreeEnvironmentStringsW(env);
    }
    if (add_capture_layer) {
        entries.push_back(L"VK_INSTANCE_LAYERS=" + layers);
        const bool override_set = !override_paths.empty();
        entries.push_back((override_set ? L"VK_LAYER_PATH=" : L"VK_ADD_LAYER_PATH=") +
                          layer_paths + (override_set ? L";" + override_paths : L""));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    std::vector<wchar_t> environment;
    for (const auto& entry : entries) environment.insert(environment.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
    environment.push_back(L'\0');

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe_path + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, environment.data(), dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        if (out_error) *out_error = format_win32_error(L"CreateProcessW", GetLastError());
        return false;
    }

    // Wait for process message loop / loader initialization before injecting
    WaitForInputIdle(pi.hProcess, 3000);
    Sleep(150);
    const bool ok = inject_dll(pi.dwProcessId, dll_path, out_error);
    out_pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

} // namespace dr
