#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <windows.h>
#include <cmath>
#include <cstring>

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

    RECT rc{0, 0, 640, 480};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sample DX11 Depth", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 640;
    scd.BufferDesc.Height = 480;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx))) {
        return 1;
    }

    ComPtr<ID3D11Texture2D> bb;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), &bb);
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);

    D3D11_TEXTURE2D_DESC dtd{};
    dtd.Width = 640; dtd.Height = 480; dtd.MipLevels = 1; dtd.ArraySize = 1;
    dtd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dtd.SampleDesc.Count = 1;
    dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth_tex;
    dev->CreateTexture2D(&dtd, nullptr, &depth_tex);
    ComPtr<ID3D11DepthStencilView> dsv;
    dev->CreateDepthStencilView(depth_tex.Get(), nullptr, &dsv);

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
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        t += 0.05f;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &t, sizeof(t));
            ctx->Unmap(cb.Get(), 0);
        }

        const float clear_col[4] = {0.08f, 0.09f, 0.12f, 1.0f};
        ctx->ClearRenderTargetView(rtv.Get(), clear_col);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        D3D11_VIEWPORT vp{0, 0, 640, 480, 0, 1};
        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, dsv.Get());
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->Draw(3, 0);

        swap->Present(1, 0);
    }
    return 0;
}

