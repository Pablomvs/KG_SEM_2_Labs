// ====================================================
//  Lighting Pass — reads GBuffer, computes deferred lighting
//  Supports: Directional, Point, Spot lights (up to 16)
//  Draws a fullscreen triangle (no vertex buffer)
// ====================================================

Texture2D    gAlbedo   : register(t0);  // RT0: diffuse color
Texture2D    gNormal   : register(t1);  // RT1: encoded world normal
Texture2D    gWorldPos : register(t2);  // RT2: world-space position
SamplerState gSampler  : register(s0);

#define MAX_LIGHTS        16
#define LIGHT_DIRECTIONAL  0
#define LIGHT_POINT        1
#define LIGHT_SPOT         2

struct LightData
{
    float3 Position;   float Range;
    float3 Direction;  float SpotAngle;
    float3 Color;      float Intensity;
    int    Type;       float3 Pad;
};

cbuffer LightingCB : register(b0)
{
    float3    CameraPos;
    uint      LightCount;
    LightData Lights[MAX_LIGHTS];
};

// ---- Vertex shader: generates a fullscreen triangle ----
// id=0 -> (-1,-1)  id=1 -> (3,-1)  id=2 -> (-1,3)
// Together they cover the entire NDC square.

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

PSIn VSMain(uint id : SV_VertexID)
{
    PSIn o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv.y = 1.0f - o.uv.y;  // flip Y: UV (0,0) = top-left in DX
    return o;
}

// ---- Per-light contribution (diffuse + Blinn-Phong specular) ----

float3 CalcLight(LightData L, float3 worldPos, float3 N, float3 V)
{
    float3 lightDir = 0;
    float  atten    = 1.0f;

    if (L.Type == LIGHT_DIRECTIONAL)
    {
        lightDir = normalize(-L.Direction);
    }
    else if (L.Type == LIGHT_POINT)
    {
        float3 delta = L.Position - worldPos;
        float  dist  = length(delta);
        if (dist >= L.Range) return 0;
        lightDir = delta / dist;
        float t = saturate(1.0f - dist / L.Range);
        atten = t * t;  // quadratic falloff
    }
    else if (L.Type == LIGHT_SPOT)
    {
        float3 delta = L.Position - worldPos;
        float  dist  = length(delta);
        if (dist >= L.Range) return 0;
        lightDir = delta / dist;

        float cosAngle = dot(-lightDir, normalize(L.Direction));
        if (cosAngle < L.SpotAngle) return 0;

        float spotT = saturate((cosAngle - L.SpotAngle) / (1.0f - L.SpotAngle));
        float distT = saturate(1.0f - dist / L.Range);
        atten = spotT * distT;
    }

    float  diff = saturate(dot(N, lightDir));
    float3 H    = normalize(lightDir + V);
    float  spec = pow(saturate(dot(N, H)), 32.0f);

    return (diff + spec * 0.2f) * L.Color * L.Intensity * atten;
}

// ---- Pixel shader ----

float4 PSMain(PSIn i) : SV_TARGET
{
    float3 albedo  = gAlbedo.Sample(gSampler, i.uv).rgb;
    float4 nrmSamp = gNormal.Sample(gSampler, i.uv);

    // w=1 was written by geometry pass; w=0 means background (sky)
    if (nrmSamp.a < 0.5f)
        return float4(0.48f, 0.52f, 0.80f, 1.0f);  // sky colour

    float3 worldPos = gWorldPos.Sample(gSampler, i.uv).rgb;

    // Decode normal from [0,1] back to [-1,1]
    float3 N = normalize(nrmSamp.rgb * 2.0f - 1.0f);
    float3 V = normalize(CameraPos - worldPos);

    // Ambient base (small so point lights are clearly visible)
    float3 color = float3(0.03f, 0.03f, 0.04f);

    for (uint li = 0; li < LightCount; li++)
        color += CalcLight(Lights[li], worldPos, N, V);

    return float4(albedo * color, 1.0f);
}
