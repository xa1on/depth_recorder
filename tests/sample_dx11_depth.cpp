#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <cwchar>
#include <cstdint>
#include <cstring>
#ifdef DEPTH_RECORDER_GPU_PROBE
#include "dx11_gpu_probe.h"
#endif

using Microsoft::WRL::ComPtr;

static const char* kShaderSrc = R"(
cbuffer CB : register(b0) { float t; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR0; };
VSOut VSMain(uint vid : SV_VertexID) {
    float2 p[3] = { float2(0.0, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7) };
    float3 c[3] = { float3(1,0.2,0.2), float3(0.2,1,0.2), float3(0.2,0.4,1) };
    float z[3] = { 0.2f + 0.15f * sin(t), 0.5f, 0.85f - 0.15f * cos(t) };
    VSOut o;
    o.pos = float4(p[vid], z[vid], 1.0f);
    o.col = c[vid];
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return float4(i.col, 1.0f); }
)";

static uint32_t sample_option(const wchar_t* name, uint32_t default_value, uint32_t max_value = 3840) {
    wchar_t buffer[16]{};
    if (!GetEnvironmentVariableW(name, buffer, 16)) return default_value;
    const auto number = std::wcstoul(buffer, nullptr, 10);
    return number && number <= max_value ? number : default_value;
}

static ComPtr<IDXGIAdapter> find_adapter(uint32_t vendor_id) {
    ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory)))) return {};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(factory->EnumAdapters(index, &adapter))) return {};
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == vendor_id) return adapter;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SampleDX11DepthWnd";
    RegisterClassExW(&wc);

    auto width = sample_option(L"DEPTH_RECORDER_SAMPLE_WIDTH", 640);
    auto height = sample_option(L"DEPTH_RECORDER_SAMPLE_HEIGHT", 480);
    const auto rate = sample_option(L"DEPTH_RECORDER_SAMPLE_FPS", 0);
    const auto depth_padding = sample_option(L"DEPTH_RECORDER_SAMPLE_DEPTH_PADDING", 0);
    const bool offscreen = sample_option(L"DEPTH_RECORDER_SAMPLE_OFFSCREEN", 0, 1) != 0;
#ifdef DEPTH_RECORDER_GPU_PROBE
    wchar_t probe_output[MAX_PATH]{};
    const auto probe_output_length = GetEnvironmentVariableW(L"DEPTH_RECORDER_SAMPLE_GPU_PROBE", probe_output, MAX_PATH);
    if (probe_output_length >= MAX_PATH) return 5;
    const bool gpu_probe_enabled = probe_output_length != 0;
#endif
    RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const int right = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int bottom = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int x = offscreen ? right - static_cast<int>(3 * width / 4) + rc.left : CW_USEDEFAULT;
    const int y = offscreen ? bottom - static_cast<int>(3 * height / 4) + rc.top : CW_USEDEFAULT;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sample DX11 Depth", WS_OVERLAPPEDWINDOW,
                                x, y, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    if (offscreen) {
        POINT corner{static_cast<LONG>(width), static_cast<LONG>(height)};
        if (!ClientToScreen(hwnd, &corner) || corner.x <= right || corner.y <= bottom) return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = sample_option(L"DEPTH_RECORDER_SAMPLE_MSAA", 1);
    scd.Windowed = TRUE;

    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    const auto vendor_id = sample_option(L"DEPTH_RECORDER_SAMPLE_ADAPTER_VENDOR", 0, 0xffff);
    auto adapter = vendor_id ? find_adapter(vendor_id) : ComPtr<IDXGIAdapter>{};
    if (vendor_id && !adapter) return 1;
    auto driver = sample_option(L"DEPTH_RECORDER_SAMPLE_WARP", 0) ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    if (adapter) driver = D3D_DRIVER_TYPE_UNKNOWN;
    if (FAILED(D3D11CreateDeviceAndSwapChain(adapter.Get(), driver, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx))) {
        return 1;
    }

    ComPtr<ID3D11Texture2D> bb;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &bb);
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    ComPtr<ID3D11DepthStencilState> extent_test_depth;
    if (offscreen || depth_padding) {
        // Give the geometry a fixed silhouette at every background marker value.
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        desc.BackFace = desc.FrontFace;
        if (FAILED(dev->CreateDepthStencilState(&desc, &extent_test_depth))) return 1;
    }

    ComPtr<IDXGISwapChain> second_swap;
    ComPtr<ID3D11RenderTargetView> second_rtv;
    if (sample_option(L"DEPTH_RECORDER_SAMPLE_WINDOWS", 1) == 2) {
        HWND second = CreateWindowExW(0, wc.lpszClassName, L"Sample DX11 Depth - Second Window", WS_OVERLAPPEDWINDOW,
                                      120, 120, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
        ComPtr<IDXGIFactory> factory;
        scd.OutputWindow = second;
        if (!second || FAILED(swap->GetParent(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateSwapChain(dev.Get(), &scd, &second_swap))) return 2;
        ComPtr<ID3D11Texture2D> second_buffer;
        if (FAILED(second_swap->GetBuffer(0, IID_PPV_ARGS(&second_buffer))) ||
            FAILED(dev->CreateRenderTargetView(second_buffer.Get(), nullptr, &second_rtv))) return 2;
        ShowWindow(second, SW_SHOW);
    }
    const auto render_percent = sample_option(L"DEPTH_RECORDER_SAMPLE_RENDER_PERCENT", 100);
    if (render_percent < 50 || render_percent > 100) return 2;
    if (depth_padding && (render_percent != 100 || scd.SampleDesc.Count != 1)) return 2;
    D3D11_TEXTURE2D_DESC dtd{};
    dtd.MipLevels = 1; dtd.ArraySize = 1;
    dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dtd.SampleDesc = scd.SampleDesc;
    dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
#ifdef DEPTH_RECORDER_GPU_PROBE
    if (gpu_probe_enabled) {
        if (render_percent != 100 || depth_padding || dtd.SampleDesc.Count != 1 || second_swap) return 5;
        dtd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dtd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
#endif
    ComPtr<ID3D11Texture2D> depth_tex;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> scaled_rtv;
    const auto create_render_targets = [&] {
        dtd.Width = width * render_percent / 100 + depth_padding;
        dtd.Height = height * render_percent / 100 + depth_padding;
        D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};
        depth_view.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_view.ViewDimension = dtd.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateTexture2D(&dtd, nullptr, &depth_tex)) ||
            FAILED(dev->CreateDepthStencilView(depth_tex.Get(), &depth_view, &dsv))) return false;
        if (render_percent == 100 && !depth_padding) return true;
        auto color_desc = dtd;
        color_desc.Format = scd.BufferDesc.Format;
        color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> scaled_color;
        return SUCCEEDED(dev->CreateTexture2D(&color_desc, nullptr, &scaled_color)) &&
               SUCCEEDED(dev->CreateRenderTargetView(scaled_color.Get(), nullptr, &scaled_rtv));
    };
    if (!create_render_targets()) return 1;

    const auto resize_ms = sample_option(L"DEPTH_RECORDER_SAMPLE_RESIZE_MS", 0);
    const auto second_resize_ms = sample_option(L"DEPTH_RECORDER_SAMPLE_SECOND_RESIZE_MS", 0);
    const auto gap_ms = sample_option(L"DEPTH_RECORDER_SAMPLE_GAP_MS", 0);
    const auto gap_start_ms = sample_option(L"DEPTH_RECORDER_SAMPLE_GAP_START_MS", 1200);
    const auto report_seconds = sample_option(L"DEPTH_RECORDER_SAMPLE_REPORT_SECONDS", 30);
#ifdef DEPTH_RECORDER_GPU_PROBE
    const auto probe_fps = sample_option(L"DEPTH_RECORDER_SAMPLE_GPU_FPS", 120);
    Dx11GpuProbe probe;
    ComPtr<ID3D11ShaderResourceView> probe_depth;
    uint64_t next_probe_slot = 0;
    if (gpu_probe_enabled) {
        if (resize_ms || gap_ms || probe_fps > 240) return 5;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(depth_tex.Get(), &view, &probe_depth)) ||
            !probe.start(dev.Get(), ctx.Get(), width, height, probe_fps, probe_output)) return 5;
    }
#endif
    wchar_t report_path[MAX_PATH]{};
    GetEnvironmentVariableW(L"DEPTH_RECORDER_SAMPLE_REPORT", report_path, MAX_PATH);
    std::vector<int64_t> presents;
    if (report_path[0]) presents.reserve(static_cast<size_t>(report_seconds) * 250);
    int64_t last_present = 0;
    ULONGLONG next_secondary_resize = 800;
    uint32_t secondary_resizes = 0;
    const ULONGLONG started = GetTickCount64();
    bool resized = false;

    ComPtr<ID3DBlob> vsb, psb;
    D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, nullptr);
    D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, nullptr);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> cb;
    dev->CreateBuffer(&cbd, nullptr, &cb);

    MSG msg{};
    float t = 0.0f;
    uint32_t frame = 0;
    LARGE_INTEGER qpc_frequency{}, clock_origin{};
    QueryPerformanceFrequency(&qpc_frequency);
    QueryPerformanceCounter(&clock_origin);
    const auto late_present_every = sample_option(L"DEPTH_RECORDER_SAMPLE_LATE_PRESENT_EVERY", 0);
    const auto late_present_ms = sample_option(L"DEPTH_RECORDER_SAMPLE_LATE_PRESENT_MS", 0);
    const HANDLE timer = rate ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS) : nullptr;
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto elapsed = GetTickCount64() - started;
        if (second_swap && second_resize_ms && elapsed >= next_secondary_resize) {
            next_secondary_resize = elapsed + second_resize_ms;
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            second_rtv.Reset();
            ComPtr<ID3D11Texture2D> buffer;
            const UINT size = (++secondary_resizes & 1) ? 384 : 480;
            if (FAILED(second_swap->ResizeBuffers(0, size, 256, DXGI_FORMAT_UNKNOWN, 0)) ||
                FAILED(second_swap->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
                FAILED(dev->CreateRenderTargetView(buffer.Get(), nullptr, &second_rtv))) return 4;
        }
        if (resize_ms && !resized && elapsed >= resize_ms) {
            resized = true;
            width += 96;
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            rtv.Reset();
            bb.Reset();
            dsv.Reset();
            depth_tex.Reset();
            scaled_rtv.Reset();
            if (FAILED(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) ||
                FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&bb))) ||
                FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv)) ||
                !create_render_targets()) return 3;
        }
        t += 0.05f;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &t, sizeof(t));
            ctx->Unmap(cb.Get(), 0);
        }

        const uint32_t marker = frame % 6 + 1;
        const float level = marker * 32.0f / 255.0f;
        const float clear_col[4] = {level, level, level, 1.0f};
        const bool in_gap = gap_ms && elapsed >= gap_start_ms && elapsed - gap_start_ms < gap_ms;
        if (in_gap) {
            swap->Present(0, DXGI_PRESENT_TEST);
        } else {
            ctx->ClearRenderTargetView(rtv.Get(), clear_col);
            ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, marker / 8.0f, 0);
            D3D11_VIEWPORT vp{0, 0, static_cast<float>(dtd.Width - depth_padding),
                              static_cast<float>(dtd.Height - depth_padding), 0, 1};
            ID3D11RenderTargetView* rtvs[] = {scaled_rtv ? scaled_rtv.Get() : rtv.Get()};
            if (depth_padding) ctx->ClearRenderTargetView(rtvs[0], clear_col);
            ctx->OMSetRenderTargets(1, rtvs, dsv.Get());
            if (extent_test_depth) ctx->OMSetDepthStencilState(extent_test_depth.Get(), 0);
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(3, 0);
            if (depth_padding) {
                ComPtr<ID3D11Resource> padded_color;
                scaled_rtv->GetResource(&padded_color);
                const D3D11_BOX viewport{0, 0, 0, width, height, 1};
                ctx->CopySubresourceRegion(bb.Get(), 0, 0, 0, 0, padded_color.Get(), 0, &viewport);
            }
#ifdef DEPTH_RECORDER_GPU_PROBE
            if (gpu_probe_enabled) {
                probe.poll();
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const auto slot = static_cast<uint64_t>(now.QuadPart - clock_origin.QuadPart) * probe_fps / qpc_frequency.QuadPart;
                if (slot >= next_probe_slot && slot < static_cast<uint64_t>(report_seconds) * probe_fps) {
                    probe.capture(bb.Get(), probe_depth.Get(), frame);
                    next_probe_slot = slot + 1;
                }
            }
#endif
            if (report_path[0]) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                if (last_present) presents.push_back(now.QuadPart - last_present);
                last_present = now.QuadPart;
                if (elapsed >= report_seconds * 1000ULL && !presents.empty()) {
                    const auto p95 = presents.begin() + (presents.size() - 1) * 95 / 100;
                    std::nth_element(presents.begin(), p95, presents.end());
                    FILE* report = nullptr;
                    if (_wfopen_s(&report, report_path, L"w") == 0) {
                        std::fprintf(report, "presents=%zu elapsed_ms=%llu p95_ms=%.3f\n", presents.size(), elapsed,
                                     *p95 * 1000.0 / qpc_frequency.QuadPart);
                        std::fclose(report);
                    }
                    report_path[0] = 0;
                }
            }
            swap->Present(rate ? 0 : 1, 0);
        }
        if (second_swap) {
            ctx->ClearRenderTargetView(second_rtv.Get(), clear_col);
            ID3D11RenderTargetView* second_targets[] = {second_rtv.Get()};
            ctx->OMSetRenderTargets(1, second_targets, nullptr);
            second_swap->Present(0, 0);
        }
        ++frame;
        if (timer) {
            const bool late = late_present_ms && late_present_every && frame % late_present_every == 0;
            const int64_t late_ticks = late ? late_present_ms * qpc_frequency.QuadPart / 1000 : 0;
            const int64_t deadline = clock_origin.QuadPart + frame * qpc_frequency.QuadPart / rate + late_ticks;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (deadline > now.QuadPart) {
                LARGE_INTEGER due{};
                due.QuadPart = -(deadline - now.QuadPart) * 10000000 / qpc_frequency.QuadPart;
                if (due.QuadPart && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, INFINITE);
                }
            }
        }
#ifdef DEPTH_RECORDER_GPU_PROBE
        if (gpu_probe_enabled && elapsed >= report_seconds * 1000ULL) break;
#endif
    }
    if (timer) CloseHandle(timer);
#ifdef DEPTH_RECORDER_GPU_PROBE
    if (gpu_probe_enabled && !probe.finish()) return 6;
#endif
    return 0;
}

