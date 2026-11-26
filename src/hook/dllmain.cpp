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

HMODULE g_self_module = nullptr;
volatile LONG g_detaching = 0;

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

    if (!dr::g_config_view.get()) dr::g_config_view.open_or_create(false);
    const LONG start_unload = dr::g_config_view.get() ? dr::g_config_view.get()->unload_seq : 0;

    while (!g_detaching) {
        if (!dr::g_config_view.get()) dr::g_config_view.open_or_create(false);
        auto* cfg = dr::g_config_view.get();
        if (cfg && cfg->unload_seq != start_unload) {
            dr::remove_dx12_hooks();
            dr::remove_dx11_hooks();
            dr::remove_gl_hooks();
            MH_Uninitialize();
            dr::g_ipc_ring.close();
            dr::g_config_view.close();
            Sleep(60);
            FreeLibraryAndExitThread(g_self_module, 0);
        }
        Sleep(100);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = hModule;
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_detaching, 1);
        dr::remove_dx12_hooks();
        dr::remove_dx11_hooks();
        dr::remove_gl_hooks();
        MH_Uninitialize();
        dr::g_ipc_ring.close();
        dr::g_config_view.close();
    }
    return TRUE;
}

