#pragma once

struct IDXGISwapChain;

namespace dr {
bool install_dx12_hooks();
void remove_dx12_hooks();
void dx12_on_present(IDXGISwapChain* swap);
void dx12_on_resize();
} // namespace dr
