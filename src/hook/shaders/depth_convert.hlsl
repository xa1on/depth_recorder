#if DEPTH_MSAA
Texture2DMS<float> DepthTex : register(t0);
#else
Texture2D<float> DepthTex : register(t0);
#endif
RWTexture2D<unorm float> OutGray : register(u0);
RWTexture2D<unorm float4> OutRgba : register(u1);

cbuffer Params : register(b0) {
    uint g_width;
    uint g_height;
    uint g_mode;       // 0 = Gray8, 1 = ColorPhase, 2 = Rgb24Lossless
    uint g_linearize;
    uint g_reversed_z;
    float g_z_near;
    float g_z_far;
    float g_pad;
    float2 g_source_scale;
    float2 g_source_offset;
};

float LinearizeDepth(float d) {
    d = saturate(d);
    if (g_reversed_z != 0) d = 1.0f - d;
    float denom = g_z_far - d * (g_z_far - g_z_near);
    if (denom <= 1e-6f) return 1.0f;
    float z_view = (g_z_near * g_z_far) / denom;
    return saturate((z_view - g_z_near) / max(g_z_far - g_z_near, 1e-6f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g_width || dtid.y >= g_height) return;

    // Map pixel centers onto the depth texture, including reduced render resolutions.
    const int2 source = int2((float2(dtid.xy) + 0.5f) * g_source_scale + g_source_offset);
#if DEPTH_MSAA
    // Preserve a real depth sample at geometry edges instead of blending surfaces.
    float d_raw = saturate(DepthTex.Load(source, 0));
#else
    float d_raw = saturate(DepthTex.Load(int3(source, 0)));
#endif

    if (g_mode == 0) {
        float d = (g_linearize != 0) ? LinearizeDepth(d_raw) : d_raw;
        OutGray[dtid.xy] = d;
        return;
    }

    if (g_mode == 1) {
        float d = (g_linearize != 0) ? LinearizeDepth(d_raw) : d_raw;
        float theta = 6.28318530718f * 8.0f * d;
        float g = 0.5f + 0.5f * cos(theta);
        float b = 0.5f + 0.5f * sin(theta);
        OutRgba[dtid.xy] = float4(d, g, b, 1.0f);
        return;
    }

    // Mode 2: Exact 24-bit integer packing across RGB bytes (float has 24-bit exact significand)
    uint u24 = (uint)clamp(round(d_raw * 16777215.0f), 0.0f, 16777215.0f);
    float r = (float)((u24 >> 16) & 0xFF) / 255.0f;
    float g = (float)((u24 >> 8) & 0xFF) / 255.0f;
    float b = (float)(u24 & 0xFF) / 255.0f;
    OutRgba[dtid.xy] = float4(r, g, b, 1.0f);
}
