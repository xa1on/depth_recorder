#include <cstdlib>
#include <string>
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 1;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    const std::wstring dll_path = argv[2];

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return static_cast<int>(GetLastError());

    const size_t bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        const DWORD e = GetLastError();
        CloseHandle(proc);
        return static_cast<int>(e);
    }

    int rc = 0;
    if (!WriteProcessMemory(proc, remote_mem, dll_path.c_str(), bytes, nullptr)) {
        rc = static_cast<int>(GetLastError());
    } else {
        auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, load_lib, remote_mem, 0, nullptr);
        if (thread) {
            const DWORD wait = WaitForSingleObject(thread, 5000);
            if (wait == WAIT_OBJECT_0) {
                DWORD module = 0;
                if (!GetExitCodeThread(thread, &module)) {
                    rc = static_cast<int>(GetLastError());
                } else if (module == 0) {
                    rc = ERROR_MOD_NOT_FOUND;
                }
            } else {
                rc = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : static_cast<int>(GetLastError());
                // The target thread may still read the path. Leave it allocated
                // in the target process, which will reclaim it on exit.
                remote_mem = nullptr;
            }
            CloseHandle(thread);
        } else {
            rc = static_cast<int>(GetLastError());
        }
    }

    if (remote_mem) VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return rc;
}
