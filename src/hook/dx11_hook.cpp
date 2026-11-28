#include "dx11_hook.h"
#include "dx12_hook.h"
#include "slot_ring.h"
#include "fps_overlay_font.h"

#include "depth_convert_cs.h"
#include "depth_convert_msaa_cs.h"
#include "fps_overlay_ps.h"
#include "fps_overlay_vs.h"

#include <MinHook.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

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

struct OverlayRectCB {
    float ndc_rect[4];
};

// Original function pointers
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_CreateTexture2D_Real = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateDSV = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

PFN_Present oPresent = nullptr;
PFN_ResizeBuffers oResizeBuffers = nullptr;
PFN_CreateTexture2D_Real oCreateTexture2D = nullptr;
PFN_CreateDSV oCreateDSV = nullptr;
PFN_OMSetRenderTargets oOMSetRenderTargets = nullptr;
PFN_RSSetViewports oRSSetViewports = nullptr;
PFN_ClearDSV oClearDSV = nullptr;
PFN_OMSetRenderTargets oProtectedOMSetRenderTargets = nullptr;
PFN_RSSetViewports oProtectedRSSetViewports = nullptr;
PFN_ClearDSV oProtectedClearDSV = nullptr;

thread_local bool g_forwarding_context_call = false;
struct ForwardContextCall {
    const bool previous = g_forwarding_context_call;
    ForwardContextCall() { g_forwarding_context_call = true; }
    ~ForwardContextCall() { g_forwarding_context_call = previous; }
};

std::recursive_mutex g_mutex;
std::unordered_map<ID3D11Texture2D*, DXGI_FORMAT> g_orig_depth_formats;

struct Dx11State {
    IDXGISwapChain* owner = nullptr;
    HWND window = nullptr;
    bool initialized = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_idx = 0;
    int64_t qpc_freq = 0;
    int64_t last_qpc = 0;
    LONG last_cycle_seq = 0;
    uint32_t selected_depth_idx = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    uintptr_t bound_depth = 0;
    DepthRegion render_region;

    // Depth conversion compute pipeline
    ComPtr<ID3D11ComputeShader> cs_depth;
    ComPtr<ID3D11ComputeShader> cs_depth_msaa;
    ComPtr<ID3D11Buffer> cb_depth;
    ComPtr<ID3DDeviceContextState> capture_context;
    ComPtr<ID3D11Texture2D> out_gray_tex;
    ComPtr<ID3D11UnorderedAccessView> out_gray_uav;
    ComPtr<ID3D11Texture2D> out_rgba_tex;
    ComPtr<ID3D11UnorderedAccessView> out_rgba_uav;

    // Fallback copy target if depth texture wasn't created with BIND_SHADER_RESOURCE
    ComPtr<ID3D11Texture2D> srv_copy_tex;
    uint32_t srv_copy_w = 0, srv_copy_h = 0;
    DXGI_FORMAT srv_copy_fmt = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC srv_copy_samples{};
    ComPtr<ID3D11ShaderResourceView> capture_depth_srv;
    ID3D11Texture2D* capture_depth_srv_source = nullptr;
    DXGI_FORMAT capture_depth_srv_format = DXGI_FORMAT_UNKNOWN;

    // Copy-before-clear snapshot
    ComPtr<ID3D11Texture2D> clear_snap_tex;
    uintptr_t clear_snap_key = 0;
    bool clear_snap_valid_this_frame = false;

    // 3-slot staging ring
    DXGI_FORMAT bb_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D11Texture2D> color_resolve_tex;
    ComPtr<ID3D11Texture2D> color_staging[kStagingSlots];
    ComPtr<ID3D11Texture2D> depth_staging[kStagingSlots];
    uint32_t depth_staging_bpp[kStagingSlots]{};
    CaptureSlotTracker<kStagingSlots> slots;
    LONG last_capture_generation = -1;
    int64_t last_preview_qpc = 0;
    int64_t last_cap_qpc = 0;

    // FPS overlay resources
    ComPtr<ID3D11VertexShader> vs_overlay;
    ComPtr<ID3D11PixelShader> ps_overlay;
    ComPtr<ID3D11Buffer> cb_overlay;
    ComPtr<ID3D11SamplerState> samp_overlay;
    ComPtr<ID3D11BlendState> blend_overlay;
    ComPtr<ID3D11RasterizerState> rs_overlay;
    ComPtr<ID3D11DepthStencilState> dss_overlay;
    ComPtr<ID3D11Texture2D> font_tex;
    ComPtr<ID3D11ShaderResourceView> font_srv;
    uint32_t font_w = 0, font_h = 0;
    OverlayBitmapCache overlay_cache;

    DepthHeuristic heuristic;
};

Dx11State g_dx11;

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

DXGI_FORMAT to_dsv_depth_format(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
        default: return fmt;
    }
}

void release_size_dependent() {
    for (uint32_t i = 0; i < kStagingSlots; ++i) {
        g_dx11.color_staging[i].Reset();
        g_dx11.depth_staging[i].Reset();
    }
    g_dx11.slots.reset_all();
    g_dx11.color_resolve_tex.Reset();
    g_dx11.out_gray_tex.Reset();
    g_dx11.out_gray_uav.Reset();
    g_dx11.out_rgba_tex.Reset();
    g_dx11.out_rgba_uav.Reset();
    g_dx11.srv_copy_tex.Reset();
    g_dx11.capture_depth_srv.Reset();
    g_dx11.capture_depth_srv_source = nullptr;
    g_dx11.clear_snap_tex.Reset();
    g_dx11.width = 0;
    g_dx11.height = 0;
    g_dx11.clear_snap_valid_this_frame = false;
    g_dx11.heuristic = DepthHeuristic{};
    g_dx11.heuristic.begin_frame(g_dx11.frame_idx);
}

bool ensure_pipeline(IDXGISwapChain* swap) {
    if (!g_dx11.device) {
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(g_dx11.device.GetAddressOf())))) {
            return false;
        }
        g_dx11.device->GetImmediateContext(g_dx11.ctx.GetAddressOf());
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        g_dx11.qpc_freq = freq.QuadPart;

        g_dx11.device->CreateComputeShader(g_depth_convert_cs, sizeof(g_depth_convert_cs), nullptr, g_dx11.cs_depth.GetAddressOf());
        g_dx11.device->CreateComputeShader(g_depth_convert_msaa_cs, sizeof(g_depth_convert_msaa_cs), nullptr,
                                          g_dx11.cs_depth_msaa.GetAddressOf());
        g_dx11.device->CreateVertexShader(g_fps_overlay_vs, sizeof(g_fps_overlay_vs), nullptr, g_dx11.vs_overlay.GetAddressOf());
        g_dx11.device->CreatePixelShader(g_fps_overlay_ps, sizeof(g_fps_overlay_ps), nullptr, g_dx11.ps_overlay.GetAddressOf());

        D3D11_BUFFER_DESC cbd{};
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cbd.ByteWidth = sizeof(DepthConvertParams);
        g_dx11.device->CreateBuffer(&cbd, nullptr, g_dx11.cb_depth.GetAddressOf());
        cbd.ByteWidth = sizeof(OverlayRectCB);
        g_dx11.device->CreateBuffer(&cbd, nullptr, g_dx11.cb_overlay.GetAddressOf());

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_dx11.device->CreateSamplerState(&sd, g_dx11.samp_overlay.GetAddressOf());

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dx11.device->CreateBlendState(&bd, g_dx11.blend_overlay.GetAddressOf());

        D3D11_RASTERIZER_DESC rsd{};
        rsd.FillMode = D3D11_FILL_SOLID;
        rsd.CullMode = D3D11_CULL_NONE;
        g_dx11.device->CreateRasterizerState(&rsd, g_dx11.rs_overlay.GetAddressOf());

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = FALSE;
        g_dx11.device->CreateDepthStencilState(&dsd, g_dx11.dss_overlay.GetAddressOf());
        g_dx11.initialized = true;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(swap->GetDesc(&scd))) return false;
    const uint32_t w = scd.BufferDesc.Width;
    const uint32_t h = scd.BufferDesc.Height;
    if (w == 0 || h == 0) return false;
    DXGI_FORMAT bb_fmt = scd.BufferDesc.Format;
    if (bb_fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) bb_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (bb_fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) bb_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (g_dx11.width != w || g_dx11.height != h || g_dx11.bb_format != bb_fmt) {
        release_size_dependent();
        g_dx11.width = w;
        g_dx11.height = h;
        g_dx11.bb_format = bb_fmt;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        td.Format = DXGI_FORMAT_R8_UNORM;
        g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.out_gray_tex.GetAddressOf());
        if (g_dx11.out_gray_tex) {
            g_dx11.device->CreateUnorderedAccessView(g_dx11.out_gray_tex.Get(), nullptr, g_dx11.out_gray_uav.GetAddressOf());
        }

        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.out_rgba_tex.GetAddressOf());
        if (g_dx11.out_rgba_tex) {
            g_dx11.device->CreateUnorderedAccessView(g_dx11.out_rgba_tex.Get(), nullptr, g_dx11.out_rgba_uav.GetAddressOf());
        }

        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.Format = bb_fmt;
        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.color_staging[i].GetAddressOf());
        }
    }
    return true;
}

class StagingMap {
public:
    ~StagingMap() {
        if (texture_) g_dx11.ctx->Unmap(texture_, 0);
    }

    bool try_map(ID3D11Texture2D* texture) {
        if (!texture) return true;
        if (FAILED(g_dx11.ctx->Map(texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped_))) return false;
        texture_ = texture;
        return true;
    }

    void copy_to(uint8_t* dst, uint32_t w, uint32_t h, uint32_t bpp, bool unpack_hdr = false) const {
        const size_t stride = static_cast<size_t>(w) * bpp;
        const auto* src = static_cast<const uint8_t*>(mapped_.pData);
        if (!unpack_hdr && mapped_.RowPitch == stride) {
            std::memcpy(dst, src, stride * h);
            return;
        }
        for (uint32_t y = 0; y < h; ++y) {
            if (unpack_hdr) unpack_r10g10b10a2_to_bgra(src, dst, w, false);
            else std::memcpy(dst, src, stride);
            src += mapped_.RowPitch;
            dst += stride;
        }
    }

private:
    ID3D11Texture2D* texture_ = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped_{};
};


void ensure_depth_staging(uint32_t slot, uint32_t bpp) {
    if (g_dx11.depth_staging[slot] && g_dx11.depth_staging_bpp[slot] == bpp) return;
    g_dx11.depth_staging[slot].Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_dx11.width;
    td.Height = g_dx11.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Format = (bpp == 1) ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.depth_staging[slot].GetAddressOf());
    g_dx11.depth_staging_bpp[slot] = bpp;
}

void draw_fps_overlay(IDXGISwapChain* swap, double fps, double rec_fps, bool recording, OverlayCorner corner,
                      int64_t now, RecordingActivity activity) {
    ComPtr<ID3D11Texture2D> backbuf;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuf.GetAddressOf())))) return;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_dx11.device->CreateRenderTargetView(backbuf.Get(), nullptr, rtv.GetAddressOf()))) return;

    const bool text_changed = g_dx11.overlay_cache.update(fps, rec_fps, recording, activity, now, g_dx11.qpc_freq);
    const uint32_t bw = g_dx11.overlay_cache.bw;
    const uint32_t bh = g_dx11.overlay_cache.bh;

    const bool recreated = (!g_dx11.font_tex || g_dx11.font_w != bw || g_dx11.font_h != bh);
    if (recreated) {
        g_dx11.font_tex.Reset();
        g_dx11.font_srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = bw;
        td.Height = bh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.font_tex.GetAddressOf()))) {
            g_dx11.device->CreateShaderResourceView(g_dx11.font_tex.Get(), nullptr, g_dx11.font_srv.GetAddressOf());
            g_dx11.font_w = bw;
            g_dx11.font_h = bh;
        }
    }
    if (!g_dx11.font_srv) return;

    D3D11_MAPPED_SUBRESOURCE m{};
    if ((text_changed || recreated) && SUCCEEDED(g_dx11.ctx->Map(g_dx11.font_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (uint32_t y = 0; y < bh; ++y) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + y * m.RowPitch,
                        g_dx11.overlay_cache.rgba.data() + y * bw * 4, bw * 4);
        }
        g_dx11.ctx->Unmap(g_dx11.font_tex.Get(), 0);
    }

    float ndc_rect[4]{};
    compute_overlay_ndc_rect(bw, bh, g_dx11.width, g_dx11.height, corner, ndc_rect);

    if (SUCCEEDED(g_dx11.ctx->Map(g_dx11.cb_overlay.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* cb = static_cast<OverlayRectCB*>(m.pData);
        std::memcpy(cb->ndc_rect, ndc_rect, sizeof(ndc_rect));
        g_dx11.ctx->Unmap(g_dx11.cb_overlay.Get(), 0);
    }

    // Minimal state save/restore
    ComPtr<ID3D11RenderTargetView> prev_rtv;
    ComPtr<ID3D11DepthStencilView> prev_dsv;
    g_dx11.ctx->OMGetRenderTargets(1, prev_rtv.GetAddressOf(), prev_dsv.GetAddressOf());

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(g_dx11.width), static_cast<float>(g_dx11.height), 0.0f, 1.0f};
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    ForwardContextCall overlay_context;
    g_dx11.ctx->OMSetRenderTargets(1, rtvs, nullptr);
    g_dx11.ctx->RSSetViewports(1, &vp);
    g_dx11.ctx->RSSetState(g_dx11.rs_overlay.Get());
    g_dx11.ctx->OMSetDepthStencilState(g_dx11.dss_overlay.Get(), 0);
    const float bf[4]{};
    g_dx11.ctx->OMSetBlendState(g_dx11.blend_overlay.Get(), bf, 0xFFFFFFFF);
    g_dx11.ctx->IASetInputLayout(nullptr);
    g_dx11.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx11.ctx->VSSetShader(g_dx11.vs_overlay.Get(), nullptr, 0);
    g_dx11.ctx->VSSetConstantBuffers(0, 1, g_dx11.cb_overlay.GetAddressOf());
    g_dx11.ctx->PSSetShader(g_dx11.ps_overlay.Get(), nullptr, 0);
    g_dx11.ctx->PSSetShaderResources(0, 1, g_dx11.font_srv.GetAddressOf());
    g_dx11.ctx->PSSetSamplers(0, 1, g_dx11.samp_overlay.GetAddressOf());
    g_dx11.ctx->Draw(6, 0);

    ID3D11RenderTargetView* prev_rtvs[] = {prev_rtv.Get()};
    g_dx11.ctx->OMSetRenderTargets(1, prev_rtvs, prev_dsv.Get());
}

void flush_pending_slot(SharedConfig* cfg, LONG generation) {
    const uint32_t slot = g_dx11.slots.pick_oldest_pending(generation);
    if (slot >= kStagingSlots) return;

    const auto& meta = g_dx11.slots.slots[slot];
    auto* color = meta.has_color ? g_dx11.color_staging[slot].Get() : nullptr;
    auto* depth = meta.has_depth ? g_dx11.depth_staging[slot].Get() : nullptr;
    const uint32_t w = g_dx11.width, h = g_dx11.height;
    const uint32_t depth_bpp = meta.depth_bpp;
    const bool unpack_hdr = (g_dx11.bb_format == DXGI_FORMAT_R10G10B10A2_UNORM);

    g_dx11.slots.try_publish_slot(
        slot, g_ipc_ring, cfg, g_dx11.frame_idx, g_dx11.qpc_freq, w, h,
        [&](uint8_t* dst) {
            StagingMap m;
            if (!m.try_map(color)) return false;
            m.copy_to(dst, w, h, 4, unpack_hdr);
            return true;
        },
        [&](uint8_t* dst) {
            StagingMap m;
            if (!m.try_map(depth)) return false;
            m.copy_to(dst, w, h, depth_bpp);
            return true;
        });
}


class ScopedContextState {
public:
    explicit ScopedContextState(ID3D11DeviceContext* ctx) {
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&context_)))) return;
        if (!g_dx11.capture_context) {
            ComPtr<ID3D11Device1> device;
            if (FAILED(g_dx11.device.As(&device))) return;
            const auto level = g_dx11.device->GetFeatureLevel();
            if (FAILED(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                        nullptr, &g_dx11.capture_context))) return;
        }
        context_->SwapDeviceContextState(g_dx11.capture_context.Get(), &saved_);
    }

    ~ScopedContextState() {
        if (saved_) context_->SwapDeviceContextState(saved_.Get(), nullptr);
    }

    explicit operator bool() const { return saved_ != nullptr; }

private:
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<ID3DDeviceContextState> saved_;
};

void capture_frame(IDXGISwapChain* swap, SharedConfig* cfg, uint32_t write_slot, int64_t now_qpc, bool preview_only,
                   LONG generation) {
    if (!g_dx11.color_staging[write_slot]) return;
    ScopedContextState state(g_dx11.ctx.Get());
    if (!state) return;

    const bool want_depth = (cfg && cfg->depth_enabled != 0);
    const uint32_t mode = cfg ? static_cast<uint32_t>(cfg->depth_mode) : static_cast<uint32_t>(DepthMode::Rgb24Lossless);
    bool captured_depth = false;
    uint32_t depth_bpp = (mode == static_cast<uint32_t>(DepthMode::Gray8)) ? 1u : 4u;

    DepthCandidate cand{};
    if (want_depth && g_dx11.cs_depth &&
        select_depth_candidate(g_dx11.heuristic.ranked_candidates(), cfg, g_dx11.selected_depth_idx, cand)) {
        ID3D11Texture2D* src_tex = reinterpret_cast<ID3D11Texture2D*>(cand.key);
        if (g_dx11.clear_snap_valid_this_frame && g_dx11.clear_snap_key == cand.key && g_dx11.clear_snap_tex) {
            src_tex = g_dx11.clear_snap_tex.Get();
        }

        D3D11_TEXTURE2D_DESC td{};
        src_tex->GetDesc(&td);
        const DXGI_FORMAT srv_fmt = to_srv_depth_format(td.Format);
        const bool multisampled = td.SampleDesc.Count > 1;
        auto* shader = multisampled ? g_dx11.cs_depth_msaa.Get() : g_dx11.cs_depth.Get();
        if (srv_fmt != DXGI_FORMAT_UNKNOWN && shader) {
            ID3D11Texture2D* srv_tex = src_tex;
            if ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || td.Format != to_typeless_depth(td.Format)) {
                if (!g_dx11.srv_copy_tex || g_dx11.srv_copy_w != td.Width ||
                    g_dx11.srv_copy_h != td.Height || g_dx11.srv_copy_fmt != td.Format ||
                    g_dx11.srv_copy_samples.Count != td.SampleDesc.Count ||
                    g_dx11.srv_copy_samples.Quality != td.SampleDesc.Quality) {
                    g_dx11.srv_copy_tex.Reset();
                    D3D11_TEXTURE2D_DESC ctd = td;
                    ctd.Format = to_typeless_depth(td.Format);
                    ctd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ctd.Usage = D3D11_USAGE_DEFAULT;
                    ctd.CPUAccessFlags = 0;
                    if (SUCCEEDED(g_dx11.device->CreateTexture2D(&ctd, nullptr, g_dx11.srv_copy_tex.GetAddressOf()))) {
                        g_dx11.srv_copy_w = td.Width;
                        g_dx11.srv_copy_h = td.Height;
                        g_dx11.srv_copy_fmt = td.Format;
                        g_dx11.srv_copy_samples = td.SampleDesc;
                    }
                }
                if (g_dx11.srv_copy_tex) {
                    g_dx11.ctx->CopyResource(g_dx11.srv_copy_tex.Get(), src_tex);
                    srv_tex = g_dx11.srv_copy_tex.Get();
                }
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
            srvd.Format = srv_fmt;
            srvd.ViewDimension = multisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
            if (!multisampled) srvd.Texture2D.MipLevels = 1;
            HRESULT view_result = S_OK;
            if (!g_dx11.capture_depth_srv || g_dx11.capture_depth_srv_source != srv_tex ||
                g_dx11.capture_depth_srv_format != srv_fmt) {
                view_result = g_dx11.device->CreateShaderResourceView(srv_tex, &srvd, g_dx11.capture_depth_srv.ReleaseAndGetAddressOf());
                g_dx11.capture_depth_srv_source = SUCCEEDED(view_result) ? srv_tex : nullptr;
                g_dx11.capture_depth_srv_format = srv_fmt;
            }
            if (SUCCEEDED(view_result)) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (!g_dx11.cb_depth || FAILED(g_dx11.ctx->Map(g_dx11.cb_depth.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
                auto* p = static_cast<DepthConvertParams*>(m.pData);
                *p = make_depth_convert_params(g_dx11.width, g_dx11.height, td.Width, td.Height, mode, cfg, &cand.region);
                g_dx11.ctx->Unmap(g_dx11.cb_depth.Get(), 0);

                g_dx11.ctx->CSSetShader(shader, nullptr, 0);
                g_dx11.ctx->CSSetConstantBuffers(0, 1, g_dx11.cb_depth.GetAddressOf());
                g_dx11.ctx->CSSetShaderResources(0, 1, g_dx11.capture_depth_srv.GetAddressOf());
                ID3D11UnorderedAccessView* uavs[] = {g_dx11.out_gray_uav.Get(), g_dx11.out_rgba_uav.Get()};
                g_dx11.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                g_dx11.ctx->Dispatch((g_dx11.width + 15) / 16, (g_dx11.height + 15) / 16, 1);

                ID3D11UnorderedAccessView* null_uavs[2]{};
                ID3D11ShaderResourceView* null_srvs[1]{};
                g_dx11.ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
                g_dx11.ctx->CSSetShaderResources(0, 1, null_srvs);

                ensure_depth_staging(write_slot, depth_bpp);
                if (g_dx11.depth_staging[write_slot]) {
                    g_dx11.ctx->CopyResource(g_dx11.depth_staging[write_slot].Get(),
                                             (depth_bpp == 1) ? g_dx11.out_gray_tex.Get() : g_dx11.out_rgba_tex.Get());
                    captured_depth = true;
                }
            }
        }
    }

    if (want_depth && !captured_depth) return;
    const bool capture_color = !preview_only || cfg->preview_pair_generation != generation;
    if (capture_color) {
        ComPtr<ID3D11Texture2D> backbuf;
        if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuf.GetAddressOf())))) {
            D3D11_TEXTURE2D_DESC bbd{};
            backbuf->GetDesc(&bbd);
            if (bbd.SampleDesc.Count > 1) {
                if (!g_dx11.color_resolve_tex) {
                    D3D11_TEXTURE2D_DESC rtd = bbd;
                    rtd.SampleDesc.Count = 1;
                    rtd.SampleDesc.Quality = 0;
                    rtd.Format = g_dx11.bb_format;
                    rtd.BindFlags = 0;
                    rtd.Usage = D3D11_USAGE_DEFAULT;
                    g_dx11.device->CreateTexture2D(&rtd, nullptr, g_dx11.color_resolve_tex.GetAddressOf());
                }
                if (g_dx11.color_resolve_tex) {
                    g_dx11.ctx->ResolveSubresource(g_dx11.color_resolve_tex.Get(), 0, backbuf.Get(), 0, g_dx11.bb_format);
                    backbuf = g_dx11.color_resolve_tex;
                } else return;
            }
            g_dx11.ctx->CopyResource(g_dx11.color_staging[write_slot].Get(), backbuf.Get());
        } else return;
    }

    g_dx11.slots.mark_queued(write_slot, now_qpc, generation, preview_only, mode, capture_color,
                             captured_depth, depth_bpp, g_dx11.bb_format == DXGI_FORMAT_R8G8B8A8_UNORM);
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return oPresent(swap, sync, flags);
    {
        ComPtr<ID3D12Device> d12;
        if (SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&d12)))) {
            dx12_on_present(swap);
            return oPresent(swap, sync, flags);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        if (!g_config_view.get()) g_config_view.open_or_create(false);
        const LONG generation = g_config_view.get() ? g_config_view.get()->capture_generation : -1;
        DXGI_SWAP_CHAIN_DESC scd{};
        const bool desc_ok = SUCCEEDED(swap->GetDesc(&scd));
        const bool selected = desc_ok && register_and_is_selected_swapchain(g_config_view.get(), reinterpret_cast<uint64_t>(swap),
                                                                            scd.BufferDesc.Width, scd.BufferDesc.Height, scd.OutputWindow);
        if (selected && g_dx11.owner != swap) {
            g_dx11 = Dx11State{};
            g_dx11.owner = swap;
            g_dx11.window = scd.OutputWindow;
        }
        const auto* selected_cfg = g_config_view.get();
        const bool capture_or_overlay = selected && selected_cfg &&
            (selected_cfg->recording || selected_cfg->preview_enabled || selected_cfg->overlay_enabled);
        if (capture_or_overlay && ensure_pipeline(swap)) {
            ensure_ipc_connected(g_config_view, g_ipc_ring);
            SharedConfig* cfg = g_config_view.get();
            int64_t now_qpc = 0;
            const double fps = tick_fps_and_telemetry(cfg, 1, g_dx11.last_qpc, g_dx11.qpc_freq,
                                                      g_dx11.last_cycle_seq, g_dx11.selected_depth_idx, now_qpc);

            const bool ipc_active = (cfg && (cfg->recording != 0 || cfg->preview_enabled != 0) && g_ipc_ring.event_handle() != nullptr);
            if (ipc_active) {
                flush_pending_slot(cfg, generation);
            } else {
                g_dx11.slots.reset_all();
            }
            if (cfg && g_dx11.last_capture_generation != cfg->capture_generation) {
                g_dx11.last_capture_generation = cfg->capture_generation;
                g_dx11.last_cap_qpc = 0;
            }
            const uint32_t slot = g_dx11.slots.acquire_free_slot();
            const bool have_slot = slot < kStagingSlots;
            const bool is_recording = (have_slot && ipc_active && cfg->recording != 0 &&
                                       should_capture_recording_now(now_qpc, g_dx11.qpc_freq,
                                                                    static_cast<uint32_t>(cfg->target_rec_fps), g_dx11.last_cap_qpc));
            const bool is_preview = (have_slot && ipc_active && cfg->recording == 0 && cfg->preview_enabled != 0 &&
                                     should_capture_preview_now(now_qpc, g_dx11.qpc_freq, g_dx11.last_preview_qpc));
            if (is_recording || is_preview) {
                capture_frame(swap, cfg, slot, now_qpc, is_preview, generation);
            }

            if (cfg && cfg->overlay_enabled != 0) {
                const double rec_fps = static_cast<double>(cfg->actual_rec_fps_x10) / 10.0;
                ScopedContextState state(g_dx11.ctx.Get());
                if (state) draw_fps_overlay(swap, fps, rec_fps, cfg->recording != 0,
                                 static_cast<OverlayCorner>(cfg->overlay_corner), now_qpc,
                                 static_cast<RecordingActivity>(cfg->recording_activity));
            }

            g_dx11.frame_idx++;
            g_dx11.heuristic.end_frame_prune();
            g_dx11.heuristic.begin_frame(g_dx11.frame_idx);
            g_dx11.clear_snap_valid_this_frame = false;
        }
    }
    return oPresent(swap, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* swap, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    dx12_on_resize(swap);
    {
        std::lock_guard lock(g_mutex);
        if (g_dx11.owner == swap) release_size_dependent();
    }
    return oResizeBuffers(swap, count, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE hkCreateTexture2D(ID3D11Device* dev, const D3D11_TEXTURE2D_DESC* desc,
                                            const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC mod{};
    DXGI_FORMAT orig_fmt = DXGI_FORMAT_UNKNOWN;
    if (desc && (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) && desc->SampleDesc.Count == 1) {
        const DXGI_FORMAT typeless = to_typeless_depth(desc->Format);
        if (typeless != desc->Format) {
            mod = *desc;
            orig_fmt = desc->Format;
            mod.Format = typeless;
            mod.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            desc = &mod;
        }
    }
    const HRESULT hr = oCreateTexture2D(dev, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && orig_fmt != DXGI_FORMAT_UNKNOWN) {
        std::lock_guard lock(g_mutex);
        g_orig_depth_formats[*out] = orig_fmt;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDSV(ID3D11Device* dev, ID3D11Resource* res,
                                      const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** out) {
    D3D11_DEPTH_STENCIL_VIEW_DESC mod{};
    if (res && (!desc || desc->Format == DXGI_FORMAT_UNKNOWN)) {
        DXGI_FORMAT orig_fmt = DXGI_FORMAT_UNKNOWN;
        {
            std::lock_guard lock(g_mutex);
            auto it = g_orig_depth_formats.find(reinterpret_cast<ID3D11Texture2D*>(res));
            if (it != g_orig_depth_formats.end()) orig_fmt = it->second;
        }
        if (orig_fmt != DXGI_FORMAT_UNKNOWN) {
            if (desc) mod = *desc;
            else {
                mod.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            }
            mod.Format = to_dsv_depth_format(orig_fmt);
            desc = &mod;
        }
    }
    return oCreateDSV(dev, res, desc, out);
}

template<unsigned variant>
void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT num_rtv,
                                          ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g_forwarding_context_call) {
        std::lock_guard lock(g_mutex);
        if (ctx == g_dx11.ctx.Get()) {
            g_dx11.bound_depth = 0;
            if (dsv && g_dx11.width > 0) {
                ComPtr<ID3D11Resource> res;
                dsv->GetResource(res.GetAddressOf());
                ComPtr<ID3D11Texture2D> tex;
                if (res && SUCCEEDED(res.As(&tex))) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    g_dx11.bound_depth = reinterpret_cast<uintptr_t>(tex.Get());
                    g_dx11.heuristic.record_draw(g_dx11.bound_depth, td.Width, td.Height, g_dx11.width, g_dx11.height);
                    g_dx11.heuristic.set_region(g_dx11.bound_depth, g_dx11.render_region);
                }
            }
        }
    }
    ForwardContextCall forward;
    auto original = variant == 1 ? oProtectedOMSetRenderTargets : oOMSetRenderTargets;
    original(ctx, num_rtv, rtvs, dsv);
}

template<unsigned variant>
void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* ctx, UINT count, const D3D11_VIEWPORT* viewports) {
    if (!g_forwarding_context_call) {
        std::lock_guard lock(g_mutex);
        if (ctx == g_dx11.ctx.Get()) {
            g_dx11.render_region = (count >= 1 && viewports)
                ? DepthRegion{viewports[0].TopLeftX, viewports[0].TopLeftY, viewports[0].Width, viewports[0].Height}
                : DepthRegion{};
            if (g_dx11.bound_depth) g_dx11.heuristic.set_region(g_dx11.bound_depth, g_dx11.render_region);
        }
    }
    ForwardContextCall forward;
    auto original = variant == 1 ? oProtectedRSSetViewports : oRSSetViewports;
    original(ctx, count, viewports);
}

template<unsigned variant>
void STDMETHODCALLTYPE hkClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                UINT flags, FLOAT depth, UINT8 stencil) {
    if (!g_forwarding_context_call && dsv && (flags & D3D11_CLEAR_DEPTH) && g_dx11.width > 0 &&
        g_config_view.get() && g_config_view.get()->depth_enabled) {
        ComPtr<ID3D11Resource> res;
        dsv->GetResource(res.GetAddressOf());
        ComPtr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res.As(&tex))) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            std::lock_guard lock(g_mutex);
            const uintptr_t key = reinterpret_cast<uintptr_t>(tex.Get());
            const auto ranked = g_dx11.heuristic.ranked_candidates();
            if (!ranked.empty() && ranked[0].key == key && ranked[0].draw_calls >= 5 && g_dx11.device) {
                if (g_dx11.clear_snap_tex) {
                    D3D11_TEXTURE2D_DESC snap{};
                    g_dx11.clear_snap_tex->GetDesc(&snap);
                    if (snap.Width != td.Width || snap.Height != td.Height || snap.Format != to_typeless_depth(td.Format) ||
                        snap.MipLevels != td.MipLevels || snap.ArraySize != td.ArraySize ||
                        snap.SampleDesc.Count != td.SampleDesc.Count || snap.SampleDesc.Quality != td.SampleDesc.Quality) {
                        g_dx11.clear_snap_tex.Reset();
                        g_dx11.clear_snap_valid_this_frame = false;
                    }
                }
                if (!g_dx11.clear_snap_tex) {
                    D3D11_TEXTURE2D_DESC std_desc = td;
                    std_desc.Format = to_typeless_depth(td.Format);
                    std_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    std_desc.Usage = D3D11_USAGE_DEFAULT;
                    std_desc.CPUAccessFlags = 0;
                    g_dx11.device->CreateTexture2D(&std_desc, nullptr, g_dx11.clear_snap_tex.GetAddressOf());
                }
                if (g_dx11.clear_snap_tex) {
                    ctx->CopyResource(g_dx11.clear_snap_tex.Get(), tex.Get());
                    g_dx11.clear_snap_key = key;
                    g_dx11.clear_snap_valid_this_frame = true;
                }
            }
            g_dx11.heuristic.record_clear(key, td.Width, td.Height, g_dx11.width, g_dx11.height);
        }
    }
    ForwardContextCall forward;
    auto original = variant == 1 ? oProtectedClearDSV : oClearDSV;
    original(ctx, dsv, flags, depth, stencil);
}

} // namespace

bool install_dx11_hooks() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DepthRecDummyDX11";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &scd, swap.GetAddressOf(), dev.GetAddressOf(), &fl, ctx.GetAddressOf());

    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (FAILED(hr)) return false;

    void** swap_vt = *reinterpret_cast<void***>(swap.Get());
    void** dev_vt = *reinterpret_cast<void***>(dev.Get());
    void** ctx_vt = *reinterpret_cast<void***>(ctx.Get());

    MH_CreateHook(swap_vt[8], reinterpret_cast<void*>(hkPresent), reinterpret_cast<void**>(&oPresent));
    MH_CreateHook(swap_vt[13], reinterpret_cast<void*>(hkResizeBuffers), reinterpret_cast<void**>(&oResizeBuffers));
    MH_CreateHook(dev_vt[5], reinterpret_cast<void*>(hkCreateTexture2D), reinterpret_cast<void**>(&oCreateTexture2D));
    MH_CreateHook(dev_vt[10], reinterpret_cast<void*>(hkCreateDSV), reinterpret_cast<void**>(&oCreateDSV));
    const auto bind = ctx_vt[33];
    const auto viewport = ctx_vt[44];
    const auto clear = ctx_vt[53];
    MH_CreateHook(bind, reinterpret_cast<void*>(hkOMSetRenderTargets<0>), reinterpret_cast<void**>(&oOMSetRenderTargets));
    MH_CreateHook(viewport, reinterpret_cast<void*>(hkRSSetViewports<0>), reinterpret_cast<void**>(&oRSSetViewports));
    MH_CreateHook(clear, reinterpret_cast<void*>(hkClearDSV<0>), reinterpret_cast<void**>(&oClearDSV));
    ComPtr<ID3D11Multithread> threading;
    if (SUCCEEDED(ctx.As(&threading))) {
        // The runtime can replace these dispatch entries in place.
        const BOOL old = threading->SetMultithreadProtected(TRUE);
        const auto protected_vt = *reinterpret_cast<void***>(ctx.Get());
        if (protected_vt[33] != bind) MH_CreateHook(protected_vt[33], reinterpret_cast<void*>(hkOMSetRenderTargets<1>), reinterpret_cast<void**>(&oProtectedOMSetRenderTargets));
        if (protected_vt[44] != viewport) MH_CreateHook(protected_vt[44], reinterpret_cast<void*>(hkRSSetViewports<1>), reinterpret_cast<void**>(&oProtectedRSSetViewports));
        if (protected_vt[53] != clear) MH_CreateHook(protected_vt[53], reinterpret_cast<void*>(hkClearDSV<1>), reinterpret_cast<void**>(&oProtectedClearDSV));
        threading->SetMultithreadProtected(old);
    }
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

void remove_dx11_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    std::lock_guard lock(g_mutex);
    release_size_dependent();
    g_dx11 = Dx11State{};
}

} // namespace dr
