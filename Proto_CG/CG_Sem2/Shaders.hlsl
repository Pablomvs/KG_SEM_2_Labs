// Shaders.hlsl
// Vertex: POSITION (float3), NORMAL (float3), TEXCOORD (float2)
// CB b0: World, View, Proj, UVTransform (xy=tiling, zw=offset)
// SRV t0: diffuse texture, s0: linear wrap sampler

cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4   UVTransform; // xy = tiling, zw = UV offset (scroll)
};

Texture2D    gDiffuse : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 pos      : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct PSInput
{
    float4 pos      : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(float4(input.pos, 1.0f), World);
    float4 viewPos  = mul(worldPos, View);
    output.pos      = mul(viewPos,  Proj);

    output.normal   = normalize(mul(input.normal, (float3x3)World));

    // Apply tiling and scroll
    output.uv = input.uv * UVTransform.xy + UVTransform.zw;

    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 color = gDiffuse.Sample(gSampler, input.uv);

    // Simple directional light (from above-front)
    float3 lightDir = normalize(float3(0.5f, 1.0f, -0.5f));
    float  diffuse  = saturate(dot(input.normal, lightDir));
    float  ambient  = 0.25f;

    color.rgb *= (ambient + diffuse * 0.75f);

    return color;
}
