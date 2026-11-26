cbuffer RectCB : register(b0) {
    float4 g_ndc_rect; // xy = min_ndc, zw = max_ndc
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    // 2 triangles covering [0,1]x[0,1]
    float2 corners[6] = {
        float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
        float2(1.0f, 0.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f)
    };
    float2 c = corners[vid];
    VSOut o;
    o.pos = float4(lerp(g_ndc_rect.x, g_ndc_rect.z, c.x),
                   lerp(g_ndc_rect.w, g_ndc_rect.y, c.y), 0.0f, 1.0f);
    o.uv = c;
    return o;
}

Texture2D<float4> FontTex : register(t0);
SamplerState FontSamp : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    return FontTex.Sample(FontSamp, i.uv);
}

