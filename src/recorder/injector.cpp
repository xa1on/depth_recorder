#include "injector.h"
#include <algorithm>
#include <filesystem>
#include <psapi.h>
#include <tlhelp32.h>

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
    if (!IsWow64Process(proc, &is_wow64) || is_wow64) {
        CloseHandle(proc);
        return false; // 32-bit process cannot load 64-bit depth_hook.dll
    }

    HMODULE mods[256]{};
    DWORD needed = 0;
    bool has_gfx = false;
    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
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
    std::vector<ProcessEntry> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    const DWORD self_pid = GetCurrentProcessId();
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (is_compatible_process(pe.th32ProcessID, self_pid)) {
                list.push_back({pe.th32ProcessID, pe.szExeFile});
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(list.begin(), list.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return list;
}

bool inject_dll(DWORD pid, const std::wstring& dll_path, std::wstring* out_error) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        if (out_error) *out_error = format_win32_error(L"OpenProcess", GetLastError());
        return false;
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
            WaitForSingleObject(thread, 5000);
            DWORD remote_ret = 0;
            if (GetExitCodeThread(thread, &remote_ret) && remote_ret == 0) {
                ok = false;
                if (out_error) *out_error = L"LoadLibraryW returned NULL inside target process (architecture mismatch or blocked DLL).";
            }
            CloseHandle(thread);
        } else {
            ok = false;
            if (out_error) *out_error = format_win32_error(L"CreateRemoteThread", GetLastError());
        }
    }

    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

bool launch_and_inject(const std::wstring& exe_path, const std::wstring& dll_path, DWORD& out_pid, std::wstring* out_error) {
    const std::wstring dir = std::filesystem::path(exe_path).parent_path().wstring();
    const std::wstring layer_dir = std::filesystem::path(dll_path).parent_path().wstring();

    // Portable Vulkan layer env vars for child process (zero registry mutation)
    SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", layer_dir.c_str());
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", L"VK_LAYER_DEPTH_RECORDER");

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe_path + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
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
