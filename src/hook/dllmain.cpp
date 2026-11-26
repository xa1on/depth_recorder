#include "common/config.h"
#include "common/ipc_ring.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "gl_hook.h"
#include <MinHook.h>
#include <windows.h>

namespace dr {
SharedConfigView g_config_view;
IpcRing g_ipc_ring;
} // namespace dr

namespace {

DWORD WINAPI init_thread(LPVOID) {
    if (MH_Initialize() != MH_OK) return 1;

    // Wait briefly for D3D12, D3D11, DXGI, or OpenGL to be loaded
    for (int i = 0; i < 50; ++i) {
        if (GetModuleHandleW(L"d3d12.dll") || GetModuleHandleW(L"d3d11.dll") ||
            GetModuleHandleW(L"dxgi.dll") || GetModuleHandleW(L"opengl32.dll")) {
            break;
        }
        Sleep(50);
    }

    dr::install_dx11_hooks();
    dr::install_dx12_hooks();
    dr::install_gl_hooks();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        dr::remove_dx12_hooks();
        dr::remove_dx11_hooks();
        dr::remove_gl_hooks();
        MH_Uninitialize();
        dr::g_ipc_ring.close();
        dr::g_config_view.close();
    }
    return TRUE;
}

