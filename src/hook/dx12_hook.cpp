#include "dx12_hook.h"
#include "common/config.h"
#include "common/ipc_ring.h"
#include "depth_heuristic.h"
#include "fps_overlay_font.h"

#include "depth_convert_cs.h"
#include "fps_overlay_ps.h"
#include "fps_overlay_vs.h"

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dr {

// Defined in dllmain.cpp
extern SharedConfigView g_config_view;
extern IpcRing g_ipc_ring;

namespace {

constexpr uint32_t kStagingSlots = 3;

struct DepthConvertParams {
    uint32_t width;
    uint32_t height;
    uint32_t mode;
    uint32_t linearize;
    uint32_t reversed_z;
    float z_near;
    float z_far;
    float pad;
};

struct DsvEntry {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Original function pointers
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_CreateDescriptorHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using PFN_CreateDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_ClearDepthStencilView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

PFN_ExecuteCommandLists oExecuteCommandLists = nullptr;
PFN_CreateDescriptorHeap oCreateDescriptorHeap = nullptr;
PFN_CreateDepthStencilView oCreateDepthStencilView = nullptr;
PFN_CopyDescriptors oCopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple oCopyDescriptorsSimple = nullptr;
PFN_OMSetRenderTargets oOMSetRenderTargets = nullptr;
PFN_ClearDepthStencilView oClearDepthStencilView = nullptr;
PFN_ResourceBarrier oResourceBarrier = nullptr;

struct PendingBarrier {
    ID3D12Resource* res = nullptr;
    D3D12_RESOURCE_STATES state_after = D3D12_RESOURCE_STATE_COMMON;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_depth_write = false;
};

std::recursive_mutex g_mutex;
std::mutex g_track_mutex;
std::unordered_map<SIZE_T, DsvEntry> g_dsv_map;
std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_res_state;
std::unordered_map<uintptr_t, ComPtr<ID3D12Resource>> g_known_depth_res;
std::unordered_map<ID3D12CommandList*, std::vector<PendingBarrier>> g_cmdlist_barriers;

struct Dx12State {
    bool initialized = false;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT bb_format = DXGI_FORMAT_UNKNOWN;
    uint64_t frame_idx = 0;
    int64_t qpc_freq = 0;
    int64_t last_qpc = 0;
    LONG last_cycle_seq = 0;
    uint32_t selected_depth_idx = 0;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> cmd_queue;
    ComPtr<ID3D12CommandAllocator> cmd_alloc[kStagingSlots];
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t next_fence_val = 1;
    uint64_t slot_fence_val[kStagingSlots]{};

    // Compute pipeline (depth conversion)
    ComPtr<ID3D12RootSignature> cs_root_sig;
    ComPtr<ID3D12PipelineState> cs_pso;
    ComPtr<ID3D12DescriptorHeap> cs_heap; // 3 descriptors: t0, u0, u1
    uint32_t srv_uav_inc = 0;

    ComPtr<ID3D12Resource> out_gray_tex;
    ComPtr<ID3D12Resource> out_rgba_tex;

    // Copy target for depth texture per slot (avoids DENY_SHADER_RESOURCE / in-flight descriptor races)
    ComPtr<ID3D12Resource> srv_copy_tex[kStagingSlots];
    uint32_t srv_copy_w[kStagingSlots]{};
    uint32_t srv_copy_h[kStagingSlots]{};
    DXGI_FORMAT srv_copy_fmt[kStagingSlots]{};

    // Copy-before-clear snapshot
    ComPtr<ID3D12Resource> clear_snap_tex;
    uintptr_t clear_snap_key = 0;
    bool clear_snap_valid_this_frame = false;

    // 3-slot readback ring
    ComPtr<ID3D12Resource> color_readback[kStagingSlots];
    uint32_t color_row_pitch = 0;
    ComPtr<ID3D12Resource> depth_readback[kStagingSlots];
    uint32_t depth_readback_bpp[kStagingSlots]{};
    uint32_t depth_row_pitch[kStagingSlots]{};
    bool slot_pending[kStagingSlots]{};
    int64_t slot_qpc[kStagingSlots]{};
    uint32_t slot_mode[kStagingSlots]{};
    bool slot_has_depth[kStagingSlots]{};

    // FPS overlay graphics pipeline
    ComPtr<ID3D12RootSignature> gfx_root_sig;
    ComPtr<ID3D12PipelineState> gfx_pso;
    DXGI_FORMAT gfx_pso_format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> gfx_srv_heap; // 1 descriptor: t0 (font)
    ComPtr<ID3D12DescriptorHeap> rtv_heap;     // 1 descriptor: backbuffer RTV
    ComPtr<ID3D12Resource> font_tex;
    ComPtr<ID3D12Resource> font_upload_buf;
    uint32_t font_w = 0, font_h = 0;
    uint32_t font_row_pitch = 0;

    DepthHeuristic heuristic;
    std::vector<uint8_t> color_pack_buf;
    std::vector<uint8_t> depth_pack_buf;
};

Dx12State g_dx12;

DXGI_FORMAT to_typeless_depth(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        default: return fmt;
    }
}

DXGI_FORMAT to_srv_depth_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT strip_srgb(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return fmt;
    }
}

void wait_fence_value(uint64_t val) {
    if (!g_dx12.fence || !g_dx12.fence_event || val == 0) return;
    if (g_dx12.fence->GetCompletedValue() < val) {
        g_dx12.fence->SetEventOnCompletion(val, g_dx12.fence_event);
        WaitForSingleObject(g_dx12.fence_event, INFINITE);
    }
}

void wait_gpu_idle() {
    if (!g_dx12.cmd_queue || !g_dx12.fence) return;
    const uint64_t val = g_dx12.next_fence_val++;
    if (SUCCEEDED(g_dx12.cmd_queue->Signal(g_dx12.fence.Get(), val))) {
        wait_fence_value(val);
    }
}

void release_size_dependent() {
    wait_gpu_idle();
    for (uint32_t i = 0; i < kStagingSlots; ++i) {
        g_dx12.color_readback[i].Reset();
        g_dx12.depth_readback[i].Reset();
        g_dx12.slot_pending[i] = false;
        g_dx12.slot_fence_val[i] = 0;
        g_dx12.depth_readback_bpp[i] = 0;
        g_dx12.depth_row_pitch[i] = 0;
        g_dx12.srv_copy_tex[i].Reset();
        g_dx12.srv_copy_w[i] = 0;
        g_dx12.srv_copy_h[i] = 0;
        g_dx12.srv_copy_fmt[i] = DXGI_FORMAT_UNKNOWN;
    }
    g_dx12.out_gray_tex.Reset();
    g_dx12.out_rgba_tex.Reset();
    g_dx12.clear_snap_tex.Reset();
    g_dx12.width = 0;
    g_dx12.height = 0;
}

D3D12_RESOURCE_BARRIER make_transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3D12Resource> create_default_tex(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* dev, uint64_t bytes, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES init_state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> res;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res));
    return res;
}

bool create_root_sigs_and_psos(DXGI_FORMAT bb_fmt) {
    auto* dev = g_dx12.device.Get();
    if (!g_dx12.cs_root_sig) {
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 2;
        uav_range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(DepthConvertParams) / 4;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srv_range;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uav_range;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
        if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_dx12.cs_root_sig)))) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = g_dx12.cs_root_sig.Get();
        cpd.CS = {g_depth_convert_cs, sizeof(g_depth_convert_cs)};
        if (FAILED(dev->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&g_dx12.cs_pso)))) return false;
    }

    if (!g_dx12.gfx_root_sig) {
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 1;
        srv_range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srv_range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
        if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_dx12.gfx_root_sig)))) return false;
    }

    if (!g_dx12.gfx_pso || g_dx12.gfx_pso_format != bb_fmt) {
        g_dx12.gfx_pso.Reset();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
        gpd.pRootSignature = g_dx12.gfx_root_sig.Get();
        gpd.VS = {g_fps_overlay_vs, sizeof(g_fps_overlay_vs)};
        gpd.PS = {g_fps_overlay_ps, sizeof(g_fps_overlay_ps)};
        gpd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        gpd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        gpd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        gpd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        gpd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        gpd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        gpd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gpd.SampleMask = UINT_MAX;
        gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gpd.DepthStencilState.DepthEnable = FALSE;
        gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gpd.NumRenderTargets = 1;
        gpd.RTVFormats[0] = bb_fmt;
        gpd.SampleDesc.Count = 1;
        if (SUCCEEDED(dev->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&g_dx12.gfx_pso)))) {
            g_dx12.gfx_pso_format = bb_fmt;
        }
    }
    return g_dx12.cs_pso && g_dx12.gfx_pso;
}

bool ensure_pipeline(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(swap->GetDesc(&scd))) return false;
    const uint32_t w = scd.BufferDesc.Width;
    const uint32_t h = scd.BufferDesc.Height;
    if (w == 0 || h == 0) return false;
    const DXGI_FORMAT bb_fmt = strip_srgb(scd.BufferDesc.Format);

    if (!g_dx12.device) {
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&g_dx12.device)))) return false;
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        g_dx12.qpc_freq = freq.QuadPart;
        g_dx12.srv_uav_inc = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            if (FAILED(g_dx12.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dx12.cmd_alloc[i])))) return false;
        }
        if (FAILED(g_dx12.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx12.cmd_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_dx12.cmd_list)))) return false;
        g_dx12.cmd_list->Close();

        if (FAILED(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.fence)))) return false;
        g_dx12.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kStagingSlots * 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx12.cs_heap)))) return false;

        hd.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx12.gfx_srv_heap)))) return false;

        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_dx12.rtv_heap)))) return false;

        g_dx12.initialized = true;
    }

    if (!create_root_sigs_and_psos(bb_fmt)) return false;

    if (g_dx12.width != w || g_dx12.height != h || g_dx12.bb_format != bb_fmt) {
        release_size_dependent();
        g_dx12.width = w;
        g_dx12.height = h;
        g_dx12.bb_format = bb_fmt;

        g_dx12.out_gray_tex = create_default_tex(g_dx12.device.Get(), w, h, DXGI_FORMAT_R8_UNORM,
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_dx12.out_rgba_tex = create_default_tex(g_dx12.device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const auto base_cpu = g_dx12.cs_heap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t s = 0; s < kStagingSlots; ++s) {
            if (g_dx12.out_gray_tex) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
                uavd.Format = DXGI_FORMAT_R8_UNORM;
                uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h_uav{base_cpu.ptr + (s * 3 + 1) * g_dx12.srv_uav_inc};
                g_dx12.device->CreateUnorderedAccessView(g_dx12.out_gray_tex.Get(), nullptr, &uavd, h_uav);
            }
            if (g_dx12.out_rgba_tex) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
                uavd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h_uav{base_cpu.ptr + (s * 3 + 2) * g_dx12.srv_uav_inc};
                g_dx12.device->CreateUnorderedAccessView(g_dx12.out_rgba_tex.Get(), nullptr, &uavd, h_uav);
            }
        }

        D3D12_RESOURCE_DESC bbd{};
        bbd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        bbd.Width = w;
        bbd.Height = h;
        bbd.DepthOrArraySize = 1;
        bbd.MipLevels = 1;
        bbd.Format = bb_fmt;
        bbd.SampleDesc.Count = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total_bytes = 0;
        g_dx12.device->GetCopyableFootprints(&bbd, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);
        g_dx12.color_row_pitch = fp.Footprint.RowPitch;
        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            g_dx12.color_readback[i] = create_buffer(g_dx12.device.Get(), total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    return true;
}

void ensure_depth_readback(uint32_t slot, uint32_t bpp) {
    if (g_dx12.depth_readback[slot] && g_dx12.depth_readback_bpp[slot] == bpp) return;
    g_dx12.depth_readback[slot].Reset();
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = g_dx12.width;
    rd.Height = g_dx12.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = (bpp == 1) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total_bytes = 0;
    g_dx12.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);
    g_dx12.depth_readback[slot] = create_buffer(g_dx12.device.Get(), total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g_dx12.depth_readback_bpp[slot] = bpp;
    g_dx12.depth_row_pitch[slot] = fp.Footprint.RowPitch;
}

void copy_readback_tight(ID3D12Resource* buf, uint32_t w, uint32_t h, uint32_t bpp, uint32_t row_pitch, std::vector<uint8_t>& dst) {
    void* mapped = nullptr;
    if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) return;
    const uint32_t row_bytes = w * bpp;
    dst.resize(static_cast<size_t>(row_bytes) * h);
    if (row_pitch == row_bytes) {
        std::memcpy(dst.data(), mapped, dst.size());
    } else {
        const auto* src = static_cast<const uint8_t*>(mapped);
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * row_pitch, row_bytes);
        }
    }
    D3D12_RANGE empty{0, 0};
    buf->Unmap(0, &empty);
}

void copy_tex_to_readback(ID3D12GraphicsCommandList* cl, ID3D12Resource* src_tex, ID3D12Resource* dst_buf,
                          uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t row_pitch) {
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = src_tex;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = dst_buf;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Footprint.Format = fmt;
    dst_loc.PlacedFootprint.Footprint.Width = w;
    dst_loc.PlacedFootprint.Footprint.Height = h;
    dst_loc.PlacedFootprint.Footprint.Depth = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
}

void record_capture_commands(ID3D12GraphicsCommandList* cl, ID3D12Resource* backbuf, uint32_t write_slot,
                             SharedConfig* cfg, int64_t now_qpc) {
    // Copy backbuffer -> color_readback[write_slot]
    D3D12_RESOURCE_BARRIER b = make_transition(backbuf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b);
    copy_tex_to_readback(cl, backbuf, g_dx12.color_readback[write_slot].Get(),
                         g_dx12.width, g_dx12.height, g_dx12.bb_format, g_dx12.color_row_pitch);
    b = make_transition(backbuf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &b);

    const bool want_depth = (cfg && cfg->depth_enabled != 0);
    const uint32_t mode = cfg ? static_cast<uint32_t>(cfg->depth_mode) : static_cast<uint32_t>(DepthMode::Rgb24Lossless);
    bool captured_depth = false;

    std::vector<DepthCandidate> ranked;
    {
        std::lock_guard tlock(g_track_mutex);
        ranked = g_dx12.heuristic.ranked_candidates();
    }
    if (cfg) {
        InterlockedExchange(&cfg->candidate_depth_count, static_cast<LONG>(ranked.size()));
        if (!ranked.empty()) {
            g_dx12.selected_depth_idx %= static_cast<uint32_t>(ranked.size());
            InterlockedExchange(&cfg->selected_depth_index, static_cast<LONG>(g_dx12.selected_depth_idx));
        }
    }

    if (want_depth && !ranked.empty() && g_dx12.cs_pso) {
        const auto& cand = ranked[g_dx12.selected_depth_idx % ranked.size()];
        ComPtr<ID3D12Resource> src_tex_ref;
        {
            std::lock_guard tlock(g_track_mutex);
            auto it = g_known_depth_res.find(cand.key);
            if (it != g_known_depth_res.end()) src_tex_ref = it->second;
        }
        ID3D12Resource* src_tex = src_tex_ref.Get();
        bool using_snap = false;
        if (g_dx12.clear_snap_valid_this_frame && g_dx12.clear_snap_key == cand.key && g_dx12.clear_snap_tex) {
            src_tex = g_dx12.clear_snap_tex.Get();
            using_snap = true;
        }

        if (src_tex) {
            const D3D12_RESOURCE_DESC td = src_tex->GetDesc();
            const DXGI_FORMAT srv_fmt = to_srv_depth_format(td.Format);
            if (srv_fmt != DXGI_FORMAT_UNKNOWN) {
                auto& slot_copy = g_dx12.srv_copy_tex[write_slot];
                if (!slot_copy || g_dx12.srv_copy_w[write_slot] != static_cast<uint32_t>(td.Width) ||
                    g_dx12.srv_copy_h[write_slot] != td.Height || g_dx12.srv_copy_fmt[write_slot] != td.Format) {
                    slot_copy = create_default_tex(
                        g_dx12.device.Get(), static_cast<uint32_t>(td.Width), td.Height,
                        to_typeless_depth(td.Format), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                    if (slot_copy) {
                        g_dx12.srv_copy_w[write_slot] = static_cast<uint32_t>(td.Width);
                        g_dx12.srv_copy_h[write_slot] = td.Height;
                        g_dx12.srv_copy_fmt[write_slot] = td.Format;
                    }
                } else {
                    D3D12_RESOURCE_BARRIER b2 = make_transition(slot_copy.Get(),
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                D3D12_RESOURCE_STATE_COPY_DEST);
                    cl->ResourceBarrier(1, &b2);
                }

                if (slot_copy) {
                    D3D12_RESOURCE_STATES src_orig_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    if (using_snap) {
                        src_orig_state = D3D12_RESOURCE_STATE_COPY_DEST;
                    } else {
                        std::lock_guard tlock(g_track_mutex);
                        auto it_s = g_res_state.find(src_tex);
                        if (it_s != g_res_state.end()) src_orig_state = it_s->second;
                    }
                    if (!using_snap) {
                        if (src_orig_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                            D3D12_RESOURCE_BARRIER b_src = make_transition(src_tex, src_orig_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            cl->ResourceBarrier(1, &b_src);
                        }
                    } else {
                        D3D12_RESOURCE_BARRIER b_src = make_transition(src_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        cl->ResourceBarrier(1, &b_src);
                    }

                    D3D12_TEXTURE_COPY_LOCATION copy_dst{};
                    copy_dst.pResource = slot_copy.Get();
                    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    copy_dst.SubresourceIndex = 0;

                    D3D12_TEXTURE_COPY_LOCATION copy_src{};
                    copy_src.pResource = src_tex;
                    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    copy_src.SubresourceIndex = 0;

                    cl->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);

                    if (!using_snap && src_orig_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                        D3D12_RESOURCE_BARRIER restore[2]{
                            make_transition(src_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, src_orig_state),
                            make_transition(slot_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                        };
                        cl->ResourceBarrier(2, restore);
                    } else if (using_snap) {
                        D3D12_RESOURCE_BARRIER restore[2]{
                            make_transition(src_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                            make_transition(slot_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                        };
                        cl->ResourceBarrier(2, restore);
                    } else {
                        D3D12_RESOURCE_BARRIER b_srv = make_transition(slot_copy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        cl->ResourceBarrier(1, &b_srv);
                    }

                    const auto cpu_base = g_dx12.cs_heap->GetCPUDescriptorHandleForHeapStart();
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
                    srvd.Format = srv_fmt;
                    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvd.Texture2D.MipLevels = 1;
                    g_dx12.device->CreateShaderResourceView(slot_copy.Get(), &srvd,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE{cpu_base.ptr + (write_slot * 3) * g_dx12.srv_uav_inc});

                    DepthConvertParams p{};
                    p.width = g_dx12.width;
                    p.height = g_dx12.height;
                    p.mode = mode;
                    p.linearize = cfg ? static_cast<uint32_t>(cfg->linearize) : 1;
                    p.reversed_z = cfg ? static_cast<uint32_t>(cfg->reversed_z) : 1;
                    p.z_near = cfg ? cfg->z_near : 0.1f;
                    p.z_far = cfg ? cfg->z_far : 500.0f;

                    ID3D12DescriptorHeap* heaps[] = {g_dx12.cs_heap.Get()};
                    cl->SetDescriptorHeaps(1, heaps);
                    cl->SetComputeRootSignature(g_dx12.cs_root_sig.Get());
                    cl->SetPipelineState(g_dx12.cs_pso.Get());
                    cl->SetComputeRoot32BitConstants(0, sizeof(p) / 4, &p, 0);
                    const auto gpu_base = g_dx12.cs_heap->GetGPUDescriptorHandleForHeapStart();
                    cl->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu_base.ptr + (write_slot * 3) * g_dx12.srv_uav_inc});
                    cl->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu_base.ptr + (write_slot * 3 + 1) * g_dx12.srv_uav_inc});
                    cl->Dispatch((g_dx12.width + 15) / 16, (g_dx12.height + 15) / 16, 1);

                    const uint32_t bpp = (mode == static_cast<uint32_t>(DepthMode::Gray8)) ? 1 : 4;
                    ensure_depth_readback(write_slot, bpp);
                    ID3D12Resource* out_tex = (bpp == 1) ? g_dx12.out_gray_tex.Get() : g_dx12.out_rgba_tex.Get();
                    const DXGI_FORMAT out_fmt = (bpp == 1) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
                    if (g_dx12.depth_readback[write_slot] && out_tex) {
                        D3D12_RESOURCE_BARRIER b_out = make_transition(out_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        cl->ResourceBarrier(1, &b_out);
                        copy_tex_to_readback(cl, out_tex, g_dx12.depth_readback[write_slot].Get(),
                                             g_dx12.width, g_dx12.height, out_fmt, g_dx12.depth_row_pitch[write_slot]);
                        b_out = make_transition(out_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                        cl->ResourceBarrier(1, &b_out);
                        captured_depth = true;
                    }
                }
            }
        }
    }

    g_dx12.slot_qpc[write_slot] = now_qpc;
    g_dx12.slot_mode[write_slot] = mode;
    g_dx12.slot_has_depth[write_slot] = captured_depth;
    g_dx12.slot_pending[write_slot] = true;
}

void record_overlay_commands(ID3D12GraphicsCommandList* cl, ID3D12Resource* backbuf, double fps, bool recording, OverlayCorner corner) {
    if (!g_dx12.gfx_pso) return;

    char text[32]{};
    std::snprintf(text, sizeof(text), "%.0f", fps);
    std::vector<uint8_t> rgba;
    uint32_t bw = 0, bh = 0;
    render_fps_bitmap(text, recording, rgba, bw, bh);

    if (!g_dx12.font_tex || g_dx12.font_w != bw || g_dx12.font_h != bh) {
        g_dx12.font_tex = create_default_tex(g_dx12.device.Get(), bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!g_dx12.font_tex) return;

        D3D12_RESOURCE_DESC fd = g_dx12.font_tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total_bytes = 0;
        g_dx12.device->GetCopyableFootprints(&fd, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);
        g_dx12.font_row_pitch = fp.Footprint.RowPitch;
        g_dx12.font_upload_buf = create_buffer(g_dx12.device.Get(), total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_dx12.font_upload_buf) return;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(g_dx12.font_tex.Get(), &srvd,
                                                g_dx12.gfx_srv_heap->GetCPUDescriptorHandleForHeapStart());
        g_dx12.font_w = bw;
        g_dx12.font_h = bh;
    }

    void* mapped = nullptr;
    D3D12_RANGE empty{0, 0};
    if (SUCCEEDED(g_dx12.font_upload_buf->Map(0, &empty, &mapped)) && mapped) {
        auto* dst = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < bh; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * g_dx12.font_row_pitch, rgba.data() + static_cast<size_t>(y) * bw * 4, bw * 4);
        }
        g_dx12.font_upload_buf->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_BARRIER b_font = make_transition(g_dx12.font_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(1, &b_font);

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = g_dx12.font_upload_buf.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = bw;
    src_loc.PlacedFootprint.Footprint.Height = bh;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = g_dx12.font_row_pitch;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = g_dx12.font_tex.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    b_font = make_transition(g_dx12.font_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl->ResourceBarrier(1, &b_font);

    const float pad_px = 12.0f;
    const float w_ndc = (2.0f * bw) / static_cast<float>(g_dx12.width);
    const float h_ndc = (2.0f * bh) / static_cast<float>(g_dx12.height);
    const float px_ndc = (2.0f * pad_px) / static_cast<float>(g_dx12.width);
    const float py_ndc = (2.0f * pad_px) / static_cast<float>(g_dx12.height);

    float x0 = -1.0f + px_ndc, y1 = 1.0f - py_ndc;
    if (corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight) {
        x0 = 1.0f - px_ndc - w_ndc;
    }
    if (corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight) {
        y1 = -1.0f + py_ndc + h_ndc;
    }
    const float ndc_rect[4] = {x0, y1 - h_ndc, x0 + w_ndc, y1};

    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = g_dx12.bb_format;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtv_h = g_dx12.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_dx12.device->CreateRenderTargetView(backbuf, &rtvd, rtv_h);

    D3D12_RESOURCE_BARRIER b_rt = make_transition(backbuf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->ResourceBarrier(1, &b_rt);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(g_dx12.width), static_cast<float>(g_dx12.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_dx12.width), static_cast<LONG>(g_dx12.height)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &scissor);
    cl->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {g_dx12.gfx_srv_heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootSignature(g_dx12.gfx_root_sig.Get());
    cl->SetPipelineState(g_dx12.gfx_pso.Get());
    cl->SetGraphicsRoot32BitConstants(0, 4, ndc_rect, 0);
    cl->SetGraphicsRootDescriptorTable(1, g_dx12.gfx_srv_heap->GetGPUDescriptorHandleForHeapStart());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(6, 1, 0, 0);

    b_rt = make_transition(backbuf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cl->ResourceBarrier(1, &b_rt);
}

void flush_readback_slot(uint32_t read_slot, SharedConfig* cfg) {
    if (!g_dx12.slot_pending[read_slot] || !g_dx12.color_readback[read_slot]) return;
    wait_fence_value(g_dx12.slot_fence_val[read_slot]);

    copy_readback_tight(g_dx12.color_readback[read_slot].Get(), g_dx12.width, g_dx12.height, 4,
                        g_dx12.color_row_pitch, g_dx12.color_pack_buf);

    uint32_t depth_bytes = 0;
    if (g_dx12.slot_has_depth[read_slot] && g_dx12.depth_readback[read_slot]) {
        const uint32_t bpp = g_dx12.depth_readback_bpp[read_slot];
        copy_readback_tight(g_dx12.depth_readback[read_slot].Get(), g_dx12.width, g_dx12.height, bpp,
                            g_dx12.depth_row_pitch[read_slot], g_dx12.depth_pack_buf);
        depth_bytes = static_cast<uint32_t>(g_dx12.depth_pack_buf.size());
    }

    FrameHeader hdr{};
    hdr.frame_index = g_dx12.frame_idx;
    hdr.qpc_timestamp = g_dx12.slot_qpc[read_slot];
    hdr.qpc_freq = g_dx12.qpc_freq;
    hdr.width = g_dx12.width;
    hdr.height = g_dx12.height;
    hdr.color_bytes = static_cast<uint32_t>(g_dx12.color_pack_buf.size());
    hdr.depth_bytes = depth_bytes;
    hdr.depth_mode = g_dx12.slot_mode[read_slot];
    hdr.has_depth = g_dx12.slot_has_depth[read_slot] ? 1 : 0;

    if (!g_ipc_ring.try_push(hdr, g_dx12.color_pack_buf.data(), depth_bytes ? g_dx12.depth_pack_buf.data() : nullptr)) {
        if (cfg) InterlockedIncrement(&cfg->dropped_frames);
    }
    g_dx12.slot_pending[read_slot] = false;
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT num, ID3D12CommandList* const* lists) {
    if (queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        {
            std::lock_guard lock(g_mutex);
            if (!g_dx12.cmd_queue) {
                ComPtr<ID3D12Device> qdev;
                if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&qdev)))) {
                    if (!g_dx12.device || qdev.Get() == g_dx12.device.Get()) {
                        g_dx12.cmd_queue = queue;
                    }
                }
            }
        }
        if (lists && num > 0 && g_dx12.width > 0) {
            std::lock_guard tlock(g_track_mutex);
            for (UINT i = 0; i < num; ++i) {
                auto it = g_cmdlist_barriers.find(lists[i]);
                if (it != g_cmdlist_barriers.end()) {
                    for (const auto& pb : it->second) {
                        g_res_state[pb.res] = pb.state_after;
                        if (pb.is_depth_write) {
                            const uintptr_t key = reinterpret_cast<uintptr_t>(pb.res);
                            g_known_depth_res[key] = pb.res;
                            g_dx12.heuristic.record_draw(key, pb.width, pb.height, g_dx12.width, g_dx12.height);
                        }
                    }
                    it->second.clear();
                }
            }
        }
    }
    oExecuteCommandLists(queue, num, lists);
}

HRESULT STDMETHODCALLTYPE hkCreateDescriptorHeap(ID3D12Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    return oCreateDescriptorHeap(dev, desc, riid, out);
}

void STDMETHODCALLTYPE hkCreateDepthStencilView(ID3D12Device* dev, ID3D12Resource* res,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    oCreateDepthStencilView(dev, res, desc, dest);
    if (res && dest.ptr) {
        DXGI_FORMAT fmt = (desc && desc->Format != DXGI_FORMAT_UNKNOWN) ? desc->Format : res->GetDesc().Format;
        std::lock_guard lock(g_mutex);
        g_dsv_map[dest.ptr] = {res, fmt};
        g_known_depth_res[reinterpret_cast<uintptr_t>(res)] = res;
    }
}

void STDMETHODCALLTYPE hkCopyDescriptors(ID3D12Device* dev, UINT num_dst_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* dst_starts,
                                         const UINT* dst_sizes, UINT num_src_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                         const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    oCopyDescriptors(dev, num_dst_ranges, dst_starts, dst_sizes, num_src_ranges, src_starts, src_sizes, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || !dst_starts || !src_starts) return;

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    std::lock_guard lock(g_mutex);
    UINT di = 0, d_off = 0, si = 0, s_off = 0;
    while (di < num_dst_ranges && si < num_src_ranges) {
        const UINT d_len = dst_sizes ? dst_sizes[di] : 1;
        const UINT s_len = src_sizes ? src_sizes[si] : 1;
        if (d_off >= d_len) { di++; d_off = 0; continue; }
        if (s_off >= s_len) { si++; s_off = 0; continue; }
        const SIZE_T s_ptr = src_starts[si].ptr + static_cast<SIZE_T>(s_off) * inc;
        const SIZE_T d_ptr = dst_starts[di].ptr + static_cast<SIZE_T>(d_off) * inc;
        auto it = g_dsv_map.find(s_ptr);
        if (it != g_dsv_map.end()) g_dsv_map[d_ptr] = it->second;
        else g_dsv_map.erase(d_ptr);
        d_off++;
        s_off++;
    }
}

void STDMETHODCALLTYPE hkCopyDescriptorsSimple(ID3D12Device* dev, UINT num, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                               D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    oCopyDescriptorsSimple(dev, num, dst, src, type);
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    std::lock_guard lock(g_mutex);
    for (UINT i = 0; i < num; ++i) {
        const SIZE_T s_ptr = src.ptr + static_cast<SIZE_T>(i) * inc;
        const SIZE_T d_ptr = dst.ptr + static_cast<SIZE_T>(i) * inc;
        auto it = g_dsv_map.find(s_ptr);
        if (it != g_dsv_map.end()) g_dsv_map[d_ptr] = it->second;
        else g_dsv_map.erase(d_ptr);
    }
}

void STDMETHODCALLTYPE hkResourceBarrier(ID3D12GraphicsCommandList* cmd, UINT num_barriers, const D3D12_RESOURCE_BARRIER* barriers) {
    if (barriers && num_barriers > 0 && g_dx12.width > 0 && cmd && cmd->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard lock(g_track_mutex);
        auto& vec = g_cmdlist_barriers[cmd];
        for (UINT i = 0; i < num_barriers; ++i) {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barriers[i].Transition.pResource) {
                ID3D12Resource* res = barriers[i].Transition.pResource;
                const auto rd = res->GetDesc();
                if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
                    rd.DepthOrArraySize == 1 &&
                    rd.SampleDesc.Count == 1) {
                    const bool is_dw = (barriers[i].Transition.StateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE ||
                                        barriers[i].Transition.StateAfter == D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    vec.push_back({res, barriers[i].Transition.StateAfter,
                                   static_cast<uint32_t>(rd.Width), rd.Height, is_dw});
                }
            }
        }
    }
    oResourceBarrier(cmd, num_barriers, barriers);
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D12GraphicsCommandList* cmd, UINT num_rtvs,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (dsv && dsv->ptr && g_dx12.width > 0) {
        std::lock_guard lock(g_track_mutex);
        auto it = g_dsv_map.find(dsv->ptr);
        if (it != g_dsv_map.end() && it->second.resource) {
            const auto rd = it->second.resource->GetDesc();
            const uintptr_t key = reinterpret_cast<uintptr_t>(it->second.resource.Get());
            g_dx12.heuristic.record_draw(key, static_cast<uint32_t>(rd.Width), rd.Height, g_dx12.width, g_dx12.height);
        }
    }
    oOMSetRenderTargets(cmd, num_rtvs, rtvs, single_handle, dsv);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                               D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                                               UINT num_rects, const D3D12_RECT* rects) {
    if ((flags & D3D12_CLEAR_FLAG_DEPTH) && dsv.ptr && g_dx12.width > 0) {
        std::lock_guard lock(g_mutex);
        auto it = g_dsv_map.find(dsv.ptr);
        if (it != g_dsv_map.end() && it->second.resource) {
            ID3D12Resource* res = it->second.resource.Get();
            const auto rd = res->GetDesc();
            const uintptr_t key = reinterpret_cast<uintptr_t>(res);
            std::vector<DepthCandidate> ranked;
            {
                std::lock_guard tlock(g_track_mutex);
                ranked = g_dx12.heuristic.ranked_candidates();
            }
            if (!ranked.empty() && ranked[0].key == key && ranked[0].draw_calls >= 5 &&
                g_dx12.device && cmd->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                if (!g_dx12.clear_snap_tex) {
                    g_dx12.clear_snap_tex = create_default_tex(
                        g_dx12.device.Get(), static_cast<uint32_t>(rd.Width), rd.Height,
                        to_typeless_depth(rd.Format), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                if (g_dx12.clear_snap_tex) {
                    D3D12_RESOURCE_BARRIER b1 = make_transition(res, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    cmd->ResourceBarrier(1, &b1);
                    cmd->CopyResource(g_dx12.clear_snap_tex.Get(), res);
                    D3D12_RESOURCE_BARRIER b2 = make_transition(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    cmd->ResourceBarrier(1, &b2);
                    g_dx12.clear_snap_key = key;
                    g_dx12.clear_snap_valid_this_frame = true;
                }
            }
            g_dx12.heuristic.record_clear(key, static_cast<uint32_t>(rd.Width), rd.Height, g_dx12.width, g_dx12.height);
        }
    }
    oClearDepthStencilView(cmd, dsv, flags, depth, stencil, num_rects, rects);
}

} // namespace

void dx12_on_present(IDXGISwapChain* swap) {
    std::lock_guard lock(g_mutex);
    if (!g_config_view.get()) g_config_view.open_or_create(false);
    DXGI_SWAP_CHAIN_DESC scd{};
    const bool desc_ok = SUCCEEDED(swap->GetDesc(&scd));
    if (g_config_view.get() && (!desc_ok ||
        !register_and_is_selected_swapchain(g_config_view.get(), reinterpret_cast<uint64_t>(swap),
                                            scd.BufferDesc.Width, scd.BufferDesc.Height, scd.OutputWindow))) {
        return;
    }
    if (!ensure_pipeline(swap) || !g_dx12.cmd_queue) return;
    if (FAILED(g_dx12.device->GetDeviceRemovedReason())) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double fps = 60.0;
    if (g_dx12.last_qpc != 0 && g_dx12.qpc_freq > 0) {
        const double dt = static_cast<double>(now.QuadPart - g_dx12.last_qpc) / static_cast<double>(g_dx12.qpc_freq);
        if (dt > 1e-5) fps = 1.0 / dt;
    }
    g_dx12.last_qpc = now.QuadPart;

    if (!g_config_view.get()) g_config_view.open_or_create(false);
    if (!g_ipc_ring.event_handle()) {
        if (!g_ipc_ring.open_or_create(false) && g_config_view.get()) {
            InterlockedExchange(&g_config_view.get()->dropped_frames, static_cast<LONG>(GetLastError()) + 100000);
        }
    }

    SharedConfig* cfg = g_config_view.get();
    if (cfg) {
        InterlockedExchange(&cfg->hooked_pid, static_cast<LONG>(GetCurrentProcessId()));
        InterlockedExchange(&cfg->active_backend, 4);
        InterlockedExchange(&cfg->current_fps_x10, static_cast<LONG>(fps * 10.0));
        if (cfg->cycle_depth_seq != g_dx12.last_cycle_seq) {
            g_dx12.last_cycle_seq = cfg->cycle_depth_seq;
            g_dx12.selected_depth_idx++;
        }
    }

    const bool recording = (cfg && (cfg->recording != 0 || cfg->preview_enabled != 0) && g_ipc_ring.event_handle());
    const bool overlay = (cfg && cfg->overlay_enabled != 0);

    if (recording) {
        const uint32_t read_slot = static_cast<uint32_t>((g_dx12.frame_idx + 1) % kStagingSlots);
        flush_readback_slot(read_slot, cfg);
    } else {
        for (bool& p : g_dx12.slot_pending) p = false;
    }

    if (recording || overlay) {
        const uint32_t write_slot = static_cast<uint32_t>(g_dx12.frame_idx % kStagingSlots);
        wait_fence_value(g_dx12.slot_fence_val[write_slot]);

        ComPtr<IDXGISwapChain3> swap3;
        ComPtr<ID3D12Resource> backbuf;
        UINT bb_idx = 0;
        if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap3)))) {
            bb_idx = swap3->GetCurrentBackBufferIndex();
        }
        if (SUCCEEDED(swap->GetBuffer(bb_idx, IID_PPV_ARGS(&backbuf)))) {
            g_dx12.cmd_alloc[write_slot]->Reset();
            g_dx12.cmd_list->Reset(g_dx12.cmd_alloc[write_slot].Get(), nullptr);

            if (recording) {
                record_capture_commands(g_dx12.cmd_list.Get(), backbuf.Get(), write_slot, cfg, now.QuadPart);
            }
            if (overlay) {
                record_overlay_commands(g_dx12.cmd_list.Get(), backbuf.Get(), fps,
                                        cfg && cfg->recording != 0,
                                        static_cast<OverlayCorner>(cfg->overlay_corner));
            }

            g_dx12.cmd_list->Close();
            ID3D12CommandList* lists[] = {g_dx12.cmd_list.Get()};
            oExecuteCommandLists(g_dx12.cmd_queue.Get(), 1, lists);
            const uint64_t fval = g_dx12.next_fence_val++;
            g_dx12.cmd_queue->Signal(g_dx12.fence.Get(), fval);
            g_dx12.slot_fence_val[write_slot] = fval;
        }
    }

    g_dx12.frame_idx++;
    {
        std::lock_guard tlock(g_track_mutex);
        g_dx12.heuristic.end_frame_prune();
        if ((g_dx12.frame_idx % 120) == 0 && g_known_depth_res.size() > 64) {
            const auto active = g_dx12.heuristic.ranked_candidates();
            std::unordered_map<uintptr_t, ComPtr<ID3D12Resource>> kept;
            for (const auto& c : active) {
                auto it = g_known_depth_res.find(c.key);
                if (it != g_known_depth_res.end()) kept[c.key] = it->second;
            }
            g_known_depth_res.swap(kept);
        }
        g_dx12.heuristic.begin_frame(g_dx12.frame_idx);
    }
    g_dx12.clear_snap_valid_this_frame = false;
}

void dx12_on_resize() {
    std::lock_guard lock(g_mutex);
    release_size_dependent();
}

bool install_dx12_hooks() {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;

    bool ok = SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    if (ok) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
             SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
             SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd_list)));
    }
    if (!ok) return false;

    void** queue_vt = *reinterpret_cast<void***>(queue.Get());
    void** dev_vt = *reinterpret_cast<void***>(dev.Get());
    void** cmd_vt = *reinterpret_cast<void***>(cmd_list.Get());

    MH_CreateHook(queue_vt[10], reinterpret_cast<void*>(hkExecuteCommandLists), reinterpret_cast<void**>(&oExecuteCommandLists));
    MH_CreateHook(dev_vt[14], reinterpret_cast<void*>(hkCreateDescriptorHeap), reinterpret_cast<void**>(&oCreateDescriptorHeap));
    MH_CreateHook(dev_vt[21], reinterpret_cast<void*>(hkCreateDepthStencilView), reinterpret_cast<void**>(&oCreateDepthStencilView));
    MH_CreateHook(dev_vt[23], reinterpret_cast<void*>(hkCopyDescriptors), reinterpret_cast<void**>(&oCopyDescriptors));
    MH_CreateHook(dev_vt[24], reinterpret_cast<void*>(hkCopyDescriptorsSimple), reinterpret_cast<void**>(&oCopyDescriptorsSimple));
    MH_CreateHook(cmd_vt[26], reinterpret_cast<void*>(hkResourceBarrier), reinterpret_cast<void**>(&oResourceBarrier));
    MH_CreateHook(cmd_vt[46], reinterpret_cast<void*>(hkOMSetRenderTargets), reinterpret_cast<void**>(&oOMSetRenderTargets));
    MH_CreateHook(cmd_vt[47], reinterpret_cast<void*>(hkClearDepthStencilView), reinterpret_cast<void**>(&oClearDepthStencilView));
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

void remove_dx12_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    std::lock_guard lock(g_mutex);
    release_size_dependent();
    if (g_dx12.fence_event) {
        CloseHandle(g_dx12.fence_event);
        g_dx12.fence_event = nullptr;
    }
    g_dsv_map.clear();
    g_known_depth_res.clear();
    g_res_state.clear();
    g_cmdlist_barriers.clear();
    g_known_depth_res.clear();
    g_dx12 = Dx12State{};
}

} // namespace dr
