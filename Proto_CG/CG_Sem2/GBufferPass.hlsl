// ====================================================
//  Geometry Pass — writes scene data into GBuffer
//  Output:
//    SV_TARGET0  Albedo   R8G8B8A8   diffuse color
//    SV_TARGET1  Normal   R16G16B16A16F  world-space normal (encoded [0,1])
//    SV_TARGET2  WorldPos R32G32B32A32F  world-space position
// ====================================================

cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4   UVTransform;   // xy = tiling, zw = scroll offset
};

Texture2D    gDiffuse : register(t0);
SamplerState gSampler : register(s0);

// ---- Vertex shader input/output ----

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

struct PSIn
{
    float4 clipPos  : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float2 uv       : TEXCOORD2;
};

// ---- GBuffer output structure ----

struct GBufferOut
{
    float4 Albedo   : SV_TARGET0;
    float4 Normal   : SV_TARGET1;
    float4 WorldPos : SV_TARGET2;
};

// ---- Vertex shader ----

PSIn VSMain(VSIn v)
{
    PSIn o;

    float4 worldPos4 = mul(float4(v.pos, 1.0f), World);
    o.worldPos = worldPos4.xyz;

    float4 viewPos = mul(worldPos4, View);
    o.clipPos = mul(viewPos, Proj);

    // Transform normal to world space (no non-uniform scale, so (float3x3)World is fine)
    o.normal = normalize(mul(v.normal, (float3x3)World));

    // Apply UV tiling and animated scroll
    o.uv = v.uv * UVTransform.xy + UVTransform.zw;

    return o;
}

// ---- Pixel shader ----

GBufferOut PSMain(PSIn i)
{
    GBufferOut o;

    // Albedo from texture
    o.Albedo = gDiffuse.Sample(gSampler, i.uv);

    // Normal encoded into [0,1] range so it fits in any float RT
    float3 N = normalize(i.normal);
    o.Normal = float4(N * 0.5f + 0.5f, 1.0f);  // w=1 marks written pixel

    // World-space position
    o.WorldPos = float4(i.worldPos, 1.0f);

    return o;
}
