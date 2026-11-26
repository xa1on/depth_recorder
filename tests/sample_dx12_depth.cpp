#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
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

static D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SampleDX12DepthWnd";
    RegisterClassExW(&wc);

    RECT rc{0, 0, 640, 480};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sample DX12 Depth", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    // Drain initial window messages so WaitForInputIdle signals, then allow hook DLL init to finish
    {
        MSG init_msg{};
        while (PeekMessageW(&init_msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&init_msg);
            DispatchMessageW(&init_msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        Sleep(600);
    }

    ComPtr<ID3DBlob> vsb, psb;
    D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, nullptr);
    D3DCompile(kShaderSrc, std::strlen(kShaderSrc), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, nullptr);

    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = 640;
    scd.Height = 480;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap1;
    ComPtr<IDXGISwapChain3> swap;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scd, nullptr, nullptr, &swap1)) ||
        FAILED(swap1.As(&swap))) {
        return 1;
    }

    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));

    const UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> rts[2];
    for (UINT i = 0; i < 2; ++i) {
        swap->GetBuffer(i, IID_PPV_ARGS(&rts[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtv_inc};
        dev->CreateRenderTargetView(rts[i].Get(), nullptr, h);
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC drd{};
    drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    drd.Width = 640;
    drd.Height = 480;
    drd.DepthOrArraySize = 1;
    drd.MipLevels = 1;
    drd.Format = DXGI_FORMAT_D32_FLOAT;
    drd.SampleDesc.Count = 1;
    drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = DXGI_FORMAT_D32_FLOAT;
    dcv.DepthStencil.Depth = 1.0f;
    ComPtr<ID3D12Resource> depth_tex;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &drd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&depth_tex));
    dev->CreateDepthStencilView(depth_tex.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.Num32BitValues = 1;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd{1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr);
    ComPtr<ID3D12RootSignature> root_sig;
    dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_sig));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
    gpd.pRootSignature = root_sig.Get();
    gpd.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
    gpd.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = UINT_MAX;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.DepthStencilState.DepthEnable = TRUE;
    gpd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gpd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpd.NumRenderTargets = 1;
    gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gpd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    dev->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&pso));

    ComPtr<ID3D12CommandAllocator> alloc[2];
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[0]));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[1]));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), pso.Get(), IID_PPV_ARGS(&cmd));
    cmd->Close();

    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fence_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_vals[2]{0, 0};
    UINT64 next_fence = 1;

    MSG msg{};
    float t = 0.0f;
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        t += 0.05f;
        const UINT bb = swap->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < fence_vals[bb]) {
            fence->SetEventOnCompletion(fence_vals[bb], fence_ev);
            WaitForSingleObject(fence_ev, INFINITE);
        }

        alloc[bb]->Reset();
        cmd->Reset(alloc[bb].Get(), pso.Get());

        D3D12_RESOURCE_BARRIER b_rt = barrier(rts[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &b_rt);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + bb * rtv_inc};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear_col[4] = {0.08f, 0.09f, 0.12f, 1.0f};
        cmd->ClearRenderTargetView(rtv, clear_col, 0, nullptr);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp{0, 0, 640, 480, 0, 1};
        D3D12_RECT sc{0, 0, 640, 480};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmd->SetGraphicsRootSignature(root_sig.Get());
        cmd->SetGraphicsRoot32BitConstants(0, 1, &t, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);

        b_rt = barrier(rts[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        cmd->ResourceBarrier(1, &b_rt);
        cmd->Close();

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        swap->Present(1, 0);
        fence_vals[bb] = next_fence++;
        queue->Signal(fence.Get(), fence_vals[bb]);
    }

    const UINT64 final_val = next_fence++;
    queue->Signal(fence.Get(), final_val);
    if (fence->GetCompletedValue() < final_val) {
        fence->SetEventOnCompletion(final_val, fence_ev);
        WaitForSingleObject(fence_ev, INFINITE);
    }
    CloseHandle(fence_ev);
    return 0;
}
