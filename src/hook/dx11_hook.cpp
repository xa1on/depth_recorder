#include "dx11_hook.h"
#include "dx12_hook.h"
#include "common/config.h"
#include "common/ipc_ring.h"
#include "depth_heuristic.h"
#include "fps_overlay_font.h"

#include "depth_convert_cs.h"
#include "fps_overlay_ps.h"
#include "fps_overlay_vs.h"

#include <MinHook.h>
#include <d3d11.h>
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

struct OverlayRectCB {
    float ndc_rect[4];
};

// Original function pointers
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_CreateTexture2D_Real = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using PFN_CreateDSV = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

PFN_Present oPresent = nullptr;
PFN_ResizeBuffers oResizeBuffers = nullptr;
PFN_CreateTexture2D_Real oCreateTexture2D = nullptr;
PFN_CreateDSV oCreateDSV = nullptr;
PFN_OMSetRenderTargets oOMSetRenderTargets = nullptr;
PFN_ClearDSV oClearDSV = nullptr;

std::recursive_mutex g_mutex;
std::unordered_map<ID3D11Texture2D*, DXGI_FORMAT> g_orig_depth_formats;

struct Dx11State {
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

    // Depth conversion compute pipeline
    ComPtr<ID3D11ComputeShader> cs_depth;
    ComPtr<ID3D11Buffer> cb_depth;
    ComPtr<ID3D11Texture2D> out_gray_tex;
    ComPtr<ID3D11UnorderedAccessView> out_gray_uav;
    ComPtr<ID3D11Texture2D> out_rgba_tex;
    ComPtr<ID3D11UnorderedAccessView> out_rgba_uav;

    // Fallback copy target if depth texture wasn't created with BIND_SHADER_RESOURCE
    ComPtr<ID3D11Texture2D> srv_copy_tex;
    uint32_t srv_copy_w = 0, srv_copy_h = 0;
    DXGI_FORMAT srv_copy_fmt = DXGI_FORMAT_UNKNOWN;

    // Copy-before-clear snapshot
    ComPtr<ID3D11Texture2D> clear_snap_tex;
    uintptr_t clear_snap_key = 0;
    bool clear_snap_valid_this_frame = false;

    // 3-slot staging ring
    ComPtr<ID3D11Texture2D> color_staging[kStagingSlots];
    ComPtr<ID3D11Texture2D> depth_staging[kStagingSlots];
    uint32_t depth_staging_bpp[kStagingSlots]{};
    bool slot_pending[kStagingSlots]{};
    int64_t slot_qpc[kStagingSlots]{};
    uint32_t slot_mode[kStagingSlots]{};
    bool slot_has_depth[kStagingSlots]{};

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

    DepthHeuristic heuristic;
    std::vector<uint8_t> color_pack_buf;
    std::vector<uint8_t> depth_pack_buf;
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
        g_dx11.slot_pending[i] = false;
    }
    g_dx11.out_gray_tex.Reset();
    g_dx11.out_gray_uav.Reset();
    g_dx11.out_rgba_tex.Reset();
    g_dx11.out_rgba_uav.Reset();
    g_dx11.srv_copy_tex.Reset();
    g_dx11.clear_snap_tex.Reset();
    g_dx11.width = 0;
    g_dx11.height = 0;
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

    if (g_dx11.width != w || g_dx11.height != h) {
        release_size_dependent();
        g_dx11.width = w;
        g_dx11.height = h;

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
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        for (uint32_t i = 0; i < kStagingSlots; ++i) {
            g_dx11.device->CreateTexture2D(&td, nullptr, g_dx11.color_staging[i].GetAddressOf());
        }
    }
    return true;
}

void copy_staging_tight(ID3D11Texture2D* tex, uint32_t w, uint32_t h, uint32_t bpp, std::vector<uint8_t>& dst) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_dx11.ctx->Map(tex, 0, D3D11_MAP_READ, 0, &mapped))) return;
    const uint32_t row_bytes = w * bpp;
    dst.resize(static_cast<size_t>(row_bytes) * h);
    if (mapped.RowPitch == row_bytes) {
        std::memcpy(dst.data(), mapped.pData, dst.size());
    } else {
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
        }
    }
    g_dx11.ctx->Unmap(tex, 0);
}

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

void draw_fps_overlay(IDXGISwapChain* swap, double fps, bool recording, OverlayCorner corner) {
    ComPtr<ID3D11Texture2D> backbuf;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuf.GetAddressOf())))) return;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_dx11.device->CreateRenderTargetView(backbuf.Get(), nullptr, rtv.GetAddressOf()))) return;

    char text[32]{};
    std::snprintf(text, sizeof(text), "%.0f", fps);
    std::vector<uint8_t> rgba;
    uint32_t bw = 0, bh = 0;
    render_fps_bitmap(text, recording, rgba, bw, bh);

    if (!g_dx11.font_tex || g_dx11.font_w != bw || g_dx11.font_h != bh) {
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
    if (SUCCEEDED(g_dx11.ctx->Map(g_dx11.font_tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (uint32_t y = 0; y < bh; ++y) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + y * m.RowPitch, rgba.data() + y * bw * 4, bw * 4);
        }
        g_dx11.ctx->Unmap(g_dx11.font_tex.Get(), 0);
    }

    const float pad_px = 12.0f;
    const float w_ndc = (2.0f * bw) / static_cast<float>(g_dx11.width);
    const float h_ndc = (2.0f * bh) / static_cast<float>(g_dx11.height);
    const float px_ndc = (2.0f * pad_px) / static_cast<float>(g_dx11.width);
    const float py_ndc = (2.0f * pad_px) / static_cast<float>(g_dx11.height);

    float x0 = -1.0f + px_ndc, y1 = 1.0f - py_ndc;
    if (corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight) {
        x0 = 1.0f - px_ndc - w_ndc;
    }
    if (corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight) {
        y1 = -1.0f + py_ndc + h_ndc;
    }

    if (SUCCEEDED(g_dx11.ctx->Map(g_dx11.cb_overlay.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* cb = static_cast<OverlayRectCB*>(m.pData);
        cb->ndc_rect[0] = x0;
        cb->ndc_rect[1] = y1 - h_ndc;
        cb->ndc_rect[2] = x0 + w_ndc;
        cb->ndc_rect[3] = y1;
        g_dx11.ctx->Unmap(g_dx11.cb_overlay.Get(), 0);
    }

    // Minimal state save/restore
    ComPtr<ID3D11RenderTargetView> prev_rtv;
    ComPtr<ID3D11DepthStencilView> prev_dsv;
    g_dx11.ctx->OMGetRenderTargets(1, prev_rtv.GetAddressOf(), prev_dsv.GetAddressOf());

    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(g_dx11.width), static_cast<float>(g_dx11.height), 0.0f, 1.0f};
    ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
    oOMSetRenderTargets(g_dx11.ctx.Get(), 1, rtvs, nullptr);
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
    oOMSetRenderTargets(g_dx11.ctx.Get(), 1, prev_rtvs, prev_dsv.Get());
}

void capture_frame(IDXGISwapChain* swap, SharedConfig* cfg, int64_t now_qpc) {
    const uint32_t read_slot = static_cast<uint32_t>((g_dx11.frame_idx + 1) % kStagingSlots);
    if (g_dx11.slot_pending[read_slot] && g_dx11.color_staging[read_slot]) {
        copy_staging_tight(g_dx11.color_staging[read_slot].Get(), g_dx11.width, g_dx11.height, 4, g_dx11.color_pack_buf);
        uint32_t depth_bytes = 0;
        if (g_dx11.slot_has_depth[read_slot] && g_dx11.depth_staging[read_slot]) {
            const uint32_t bpp = g_dx11.depth_staging_bpp[read_slot];
            copy_staging_tight(g_dx11.depth_staging[read_slot].Get(), g_dx11.width, g_dx11.height, bpp, g_dx11.depth_pack_buf);
            depth_bytes = static_cast<uint32_t>(g_dx11.depth_pack_buf.size());
        }
        FrameHeader hdr{};
        hdr.frame_index = g_dx11.frame_idx;
        hdr.qpc_timestamp = g_dx11.slot_qpc[read_slot];
        hdr.qpc_freq = g_dx11.qpc_freq;
        hdr.width = g_dx11.width;
        hdr.height = g_dx11.height;
        hdr.color_bytes = static_cast<uint32_t>(g_dx11.color_pack_buf.size());
        hdr.depth_bytes = depth_bytes;
        hdr.depth_mode = g_dx11.slot_mode[read_slot];
        hdr.has_depth = g_dx11.slot_has_depth[read_slot] ? 1 : 0;

        if (!g_ipc_ring.try_push(hdr, g_dx11.color_pack_buf.data(), depth_bytes ? g_dx11.depth_pack_buf.data() : nullptr)) {
            if (cfg) InterlockedIncrement(&cfg->dropped_frames);
        }
        g_dx11.slot_pending[read_slot] = false;
    }

    const uint32_t write_slot = static_cast<uint32_t>(g_dx11.frame_idx % kStagingSlots);
    ComPtr<ID3D11Texture2D> backbuf;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuf.GetAddressOf())))) {
        g_dx11.ctx->CopyResource(g_dx11.color_staging[write_slot].Get(), backbuf.Get());
    }

    const bool want_depth = (cfg && cfg->depth_enabled != 0);
    const uint32_t mode = cfg ? static_cast<uint32_t>(cfg->depth_mode) : static_cast<uint32_t>(DepthMode::Rgb24Lossless);
    bool captured_depth = false;

    const auto ranked = g_dx11.heuristic.ranked_candidates();
    if (cfg) {
        InterlockedExchange(&cfg->candidate_depth_count, static_cast<LONG>(ranked.size()));
        if (!ranked.empty()) {
            g_dx11.selected_depth_idx %= static_cast<uint32_t>(ranked.size());
            InterlockedExchange(&cfg->selected_depth_index, static_cast<LONG>(g_dx11.selected_depth_idx));
        }
    }

    if (want_depth && !ranked.empty() && g_dx11.cs_depth) {
        const auto& cand = ranked[g_dx11.selected_depth_idx % ranked.size()];
        ID3D11Texture2D* src_tex = reinterpret_cast<ID3D11Texture2D*>(cand.key);
        if (g_dx11.clear_snap_valid_this_frame && g_dx11.clear_snap_key == cand.key && g_dx11.clear_snap_tex) {
            src_tex = g_dx11.clear_snap_tex.Get();
        }

        D3D11_TEXTURE2D_DESC td{};
        src_tex->GetDesc(&td);
        const DXGI_FORMAT srv_fmt = to_srv_depth_format(td.Format);
        if (srv_fmt != DXGI_FORMAT_UNKNOWN) {
            ID3D11Texture2D* srv_tex = src_tex;
            if ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
                if (!g_dx11.srv_copy_tex || g_dx11.srv_copy_w != td.Width ||
                    g_dx11.srv_copy_h != td.Height || g_dx11.srv_copy_fmt != td.Format) {
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
                    }
                }
                if (g_dx11.srv_copy_tex) {
                    g_dx11.ctx->CopyResource(g_dx11.srv_copy_tex.Get(), src_tex);
                    srv_tex = g_dx11.srv_copy_tex.Get();
                }
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
            srvd.Format = srv_fmt;
            srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> depth_srv;
            if (SUCCEEDED(g_dx11.device->CreateShaderResourceView(srv_tex, &srvd, depth_srv.GetAddressOf()))) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(g_dx11.ctx->Map(g_dx11.cb_depth.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    auto* p = static_cast<DepthConvertParams*>(m.pData);
                    p->width = g_dx11.width;
                    p->height = g_dx11.height;
                    p->mode = mode;
                    p->linearize = cfg ? static_cast<uint32_t>(cfg->linearize) : 1;
                    p->reversed_z = cfg ? static_cast<uint32_t>(cfg->reversed_z) : 1;
                    p->z_near = cfg ? cfg->z_near : 0.1f;
                    p->z_far = cfg ? cfg->z_far : 500.0f;
                    g_dx11.ctx->Unmap(g_dx11.cb_depth.Get(), 0);
                }

                g_dx11.ctx->CSSetShader(g_dx11.cs_depth.Get(), nullptr, 0);
                g_dx11.ctx->CSSetConstantBuffers(0, 1, g_dx11.cb_depth.GetAddressOf());
                g_dx11.ctx->CSSetShaderResources(0, 1, depth_srv.GetAddressOf());
                ID3D11UnorderedAccessView* uavs[] = {g_dx11.out_gray_uav.Get(), g_dx11.out_rgba_uav.Get()};
                g_dx11.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                g_dx11.ctx->Dispatch((g_dx11.width + 15) / 16, (g_dx11.height + 15) / 16, 1);

                ID3D11UnorderedAccessView* null_uavs[2]{};
                ID3D11ShaderResourceView* null_srvs[1]{};
                g_dx11.ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
                g_dx11.ctx->CSSetShaderResources(0, 1, null_srvs);

                const uint32_t bpp = (mode == static_cast<uint32_t>(DepthMode::Gray8)) ? 1 : 4;
                ensure_depth_staging(write_slot, bpp);
                if (g_dx11.depth_staging[write_slot]) {
                    g_dx11.ctx->CopyResource(g_dx11.depth_staging[write_slot].Get(),
                                             (bpp == 1) ? g_dx11.out_gray_tex.Get() : g_dx11.out_rgba_tex.Get());
                    captured_depth = true;
                }
            }
        }
    }

    g_dx11.slot_qpc[write_slot] = now_qpc;
    g_dx11.slot_mode[write_slot] = mode;
    g_dx11.slot_has_depth[write_slot] = captured_depth;
    g_dx11.slot_pending[write_slot] = true;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
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
        DXGI_SWAP_CHAIN_DESC scd{};
        const bool desc_ok = SUCCEEDED(swap->GetDesc(&scd));
        const bool selected = !g_config_view.get() || (desc_ok &&
            register_and_is_selected_swapchain(g_config_view.get(), reinterpret_cast<uint64_t>(swap),
                                               scd.BufferDesc.Width, scd.BufferDesc.Height, scd.OutputWindow));
        if (selected && ensure_pipeline(swap)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            double fps = 60.0;
            if (g_dx11.last_qpc != 0 && g_dx11.qpc_freq > 0) {
                const double dt = static_cast<double>(now.QuadPart - g_dx11.last_qpc) / static_cast<double>(g_dx11.qpc_freq);
                if (dt > 1e-5) fps = 1.0 / dt;
            }
            g_dx11.last_qpc = now.QuadPart;

            if (!g_config_view.get()) g_config_view.open_or_create(false);
            if (!g_ipc_ring.event_handle()) {
                if (!g_ipc_ring.open_or_create(false) && g_config_view.get()) {
                    InterlockedExchange(&g_config_view.get()->dropped_frames, static_cast<LONG>(GetLastError()) + 100000);
                }
            }

            SharedConfig* cfg = g_config_view.get();
            if (cfg) {
                InterlockedExchange(&cfg->hooked_pid, static_cast<LONG>(GetCurrentProcessId()));
                InterlockedExchange(&cfg->active_backend, 1);
                InterlockedExchange(&cfg->current_fps_x10, static_cast<LONG>(fps * 10.0));
                if (cfg->cycle_depth_seq != g_dx11.last_cycle_seq) {
                    g_dx11.last_cycle_seq = cfg->cycle_depth_seq;
                    g_dx11.selected_depth_idx++;
                }
            }

            if (cfg && (cfg->recording != 0 || cfg->preview_enabled != 0) && g_ipc_ring.event_handle()) {
                capture_frame(swap, cfg, now.QuadPart);
            } else {
                for (bool& p : g_dx11.slot_pending) p = false;
            }

            if (cfg && cfg->overlay_enabled != 0) {
                draw_fps_overlay(swap, fps, cfg->recording != 0,
                                 static_cast<OverlayCorner>(cfg->overlay_corner));
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
    dx12_on_resize();
    {
        std::lock_guard lock(g_mutex);
        release_size_dependent();
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

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT num_rtv,
                                          ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (dsv && g_dx11.width > 0) {
        ComPtr<ID3D11Resource> res;
        dsv->GetResource(res.GetAddressOf());
        ComPtr<ID3D11Texture2D> tex;
        if (res && SUCCEEDED(res.As(&tex))) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            std::lock_guard lock(g_mutex);
            g_dx11.heuristic.record_draw(reinterpret_cast<uintptr_t>(tex.Get()), td.Width, td.Height, g_dx11.width, g_dx11.height);
        }
    }
    oOMSetRenderTargets(ctx, num_rtv, rtvs, dsv);
}

void STDMETHODCALLTYPE hkClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                UINT flags, FLOAT depth, UINT8 stencil) {
    if (dsv && (flags & D3D11_CLEAR_DEPTH) && g_dx11.width > 0) {
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
    oClearDSV(ctx, dsv, flags, depth, stencil);
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
    MH_CreateHook(dev_vt[9], reinterpret_cast<void*>(hkCreateDSV), reinterpret_cast<void**>(&oCreateDSV));
    MH_CreateHook(ctx_vt[33], reinterpret_cast<void*>(hkOMSetRenderTargets), reinterpret_cast<void**>(&oOMSetRenderTargets));
    MH_CreateHook(ctx_vt[53], reinterpret_cast<void*>(hkClearDSV), reinterpret_cast<void**>(&oClearDSV));
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}

void remove_dx11_hooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    std::lock_guard lock(g_mutex);
    release_size_dependent();
    g_dx11 = Dx11State{};
}

} // namespace dr
