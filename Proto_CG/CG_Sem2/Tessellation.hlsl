// ============================================================
// Tessellation.hlsl  (improved following reference approach)
// VS -> HS -> DS -> PS
// Displacement + Normal mapping + Distance LOD + PBR lighting
// ============================================================

cbuffer PerObjectCB : register(b0)
{
    float4x4 World;
    float4x4 View;
    float4x4 Proj;
    float4   UVTransform;   // xy=tiling, zw=scroll
    float4   TimeParams;
};

cbuffer TessCB : register(b1)
{
    float4 CameraPos;       // xyz = world pos
    float4 TessParams;      // x=minTess, y=maxTess, z=minDist, w=maxDist
    float4 DisplaceParams;  // x=strength
};

Texture2D    gColor    : register(t0);
Texture2D    gDisplace : register(t1);
Texture2D    gNormal   : register(t2);
SamplerState gSampler  : register(s0);

// ============================================================
// Vertex Shader — pass through to HS in local space
// ============================================================
struct VSInput
{
    float3 Pos  : POSITION;
    float3 Norm : NORMAL;
    float2 UV   : TEXCOORD0;
};

struct HSControlPoint
{
    float3 Pos  : POSITION;
    float3 Norm : NORMAL;
    float2 UV   : TEXCOORD0;
};

HSControlPoint VSMain(VSInput input)
{
    HSControlPoint o;
    o.Pos  = input.Pos;
    o.Norm = input.Norm;
    o.UV   = input.UV;
    return o;
}

// ============================================================
// Hull Shader — distance-based tessellation level
// ============================================================
struct PatchTess
{
    float EdgeTess[3]   : SV_TessFactor;
    float InsideTess[1] : SV_InsideTessFactor;
};

float CalcTessFactor(float3 p0, float3 p1, float3 p2)
{
    float3 center  = (p0 + p1 + p2) / 3.0f;
    float3 centerW = mul(float4(center, 1.0f), World).xyz;
    float3 centerV = mul(float4(centerW, 1.0f), View).xyz;

    float maxT = max(TessParams.y, 1.0f);
    float minT = max(min(TessParams.x, maxT), 1.0f);
    float maxD = max(TessParams.w, 1.0f);
    float fade = saturate(abs(centerV.z) / maxD);

    return lerp(maxT, minT, fade);
}

PatchTess PatchHS(InputPatch<HSControlPoint, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;
    float tess     = CalcTessFactor(patch[0].Pos, patch[1].Pos, patch[2].Pos);
    pt.EdgeTess[0] = tess;
    pt.EdgeTess[1] = tess;
    pt.EdgeTess[2] = tess;
    pt.InsideTess[0] = tess;
    return pt;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchHS")]
[maxtessfactor(32.0)]
HSControlPoint HSMain(InputPatch<HSControlPoint, 3> patch,
                      uint i        : SV_OutputControlPointID,
                      uint patchID  : SV_PrimitiveID)
{
    return patch[i];
}

// ============================================================
// Domain Shader — displacement + proper TBN from UV derivatives
// ============================================================
struct DSOutput
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : TEXCOORD0;
    float3 NormW    : TEXCOORD1;
    float2 UV       : TEXCOORD2;
    float3 TangentW : TEXCOORD3;
    float3 BitangW  : TEXCOORD4;
};

void ComputePatchBasis(const OutputPatch<HSControlPoint, 3> patch,
                       out float3 tangent, out float3 bitangent)
{
    float3 p0 = patch[0].Pos, p1 = patch[1].Pos, p2 = patch[2].Pos;
    float2 uv0 = patch[0].UV,  uv1 = patch[1].UV,  uv2 = patch[2].UV;

    float3 e1  = p1 - p0;
    float3 e2  = p2 - p0;
    float2 d1  = uv1 - uv0;
    float2 d2  = uv2 - uv0;
    float  det = d1.x * d2.y - d1.y * d2.x;

    if (abs(det) < 1e-5f)
    {
        tangent   = normalize(e1);
        bitangent = normalize(e2);
        return;
    }
    float inv = 1.0f / det;
    tangent   = normalize((e1 * d2.y - e2 * d1.y) * inv);
    bitangent = normalize((e2 * d1.x - e1 * d2.x) * inv);
}

[domain("tri")]
DSOutput DSMain(PatchTess patchTess,
                float3 bary : SV_DomainLocation,
                const OutputPatch<HSControlPoint, 3> patch)
{
    DSOutput o;

    // Barycentric interpolation
    float3 pos    = patch[0].Pos  * bary.x + patch[1].Pos  * bary.y + patch[2].Pos  * bary.z;
    float3 norm   = normalize(patch[0].Norm * bary.x + patch[1].Norm * bary.y + patch[2].Norm * bary.z);
    float2 baseUV = patch[0].UV   * bary.x + patch[1].UV   * bary.y + patch[2].UV   * bary.z;
    float2 uv     = baseUV * UVTransform.xy + UVTransform.zw;

    // Displacement — range [0.3..1.0] * strength so all verts rise above base
    float scale  = DisplaceParams.x;
    float height = (gDisplace.SampleLevel(gSampler, uv, 0).r * 0.7f + 0.3f) * scale;
    float3 displacedPos = pos + norm * height;

    // Recompute normal from height-map derivatives (gives much better shading)
    uint dispW, dispH;
    gDisplace.GetDimensions(dispW, dispH);
    float2 texel = float2(1.0f / max((float)dispW, 1.0f),
                          1.0f / max((float)dispH, 1.0f));

    float hU = (gDisplace.SampleLevel(gSampler, uv + float2(texel.x, 0.0f), 0).r * 0.7f + 0.3f) * scale;
    float hV = (gDisplace.SampleLevel(gSampler, uv + float2(0.0f, texel.y), 0).r * 0.7f + 0.3f) * scale;

    float3 tangent, bitangent;
    ComputePatchBasis(patch, tangent, bitangent);

    float3 dU           = pos + tangent   * texel.x + norm * hU;
    float3 dV           = pos + bitangent * texel.y + norm * hV;
    float3 derivedNorm  = normalize(cross(dU - displacedPos, dV - displacedPos));
    if (dot(derivedNorm, norm) < 0.0f) derivedNorm *= -1.0f;

    float4 posW = mul(float4(displacedPos, 1.0f), World);
    o.PosH  = mul(mul(posW, View), Proj);
    o.PosW  = posW.xyz;

    float3x3 w3 = (float3x3)World;
    o.NormW    = normalize(mul(w3, derivedNorm));
    o.TangentW = normalize(mul(w3, tangent));
    o.BitangW  = normalize(mul(w3, bitangent));
    o.UV = baseUV;

    return o;
}

// ============================================================
// Pixel Shader — normal mapping + PBR-ish lighting
// ============================================================
float4 PSMain(DSOutput input) : SV_TARGET
{
    float2 uv = input.UV * UVTransform.xy + UVTransform.zw;

    // Normal map (NormalDX — no Y flip needed for DirectX convention)
    float3 normalTS = gNormal.Sample(gSampler, uv).xyz * 2.0f - 1.0f;

    float3x3 tbn = float3x3(
        normalize(input.TangentW),
        normalize(input.BitangW),
        normalize(input.NormW));
    float3 N = normalize(mul(normalTS, tbn));

    float4 albedo = gColor.Sample(gSampler, uv);
    albedo.rgb = saturate(pow(albedo.rgb, 0.9f) * 1.22f);

    const float roughness = 0.55f;

    float3 Ldir   = normalize(float3(-0.4f, -1.0f, -0.2f));
    float  ndotl  = saturate(dot(N, -Ldir));

    float  upFact = saturate(N.y * 0.5f + 0.5f);
    float3 ambient = lerp(float3(0.12f, 0.13f, 0.14f),
                          float3(0.24f, 0.25f, 0.28f), upFact)
                   * (1.0f - roughness * 0.24f);

    float3 V       = normalize(CameraPos.xyz - input.PosW);
    float3 H       = normalize(V + (-Ldir));
    float  fresnel = pow(1.0f - saturate(dot(N, V)), 5.0f);
    float  specPow = lerp(110.0f, 12.0f, roughness * 0.82f);
    float  spec    = pow(saturate(dot(N, H)), specPow)
                   * lerp(0.16f, 0.03f, roughness) * (1.0f + fresnel * 1.35f);
    float  ambSpec = lerp(0.015f, 0.10f, 1.0f - roughness)
                   * (0.35f + upFact * 0.65f) * (0.3f + fresnel * 0.7f);

    float3 diffuse  = float3(1.00f, 0.95f, 0.85f) * ndotl;
    float3 litLin   = (ambient + diffuse) * albedo.rgb + (spec + ambSpec).xxx;
    float3 litSRGB  = pow(saturate(litLin), 1.0f / 2.2f);

    return float4(litSRGB, 1.0f);
}
