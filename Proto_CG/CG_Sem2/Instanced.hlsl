// Шейдер для инстансинга: пишет в те же GBuffer-мишени что и DeferredGeometry.hlsl

cbuffer PerFrameCB : register(b0)
{
    float4x4 View;
    float4x4 Proj;
};

// Поток 0: данные вершины (PER_VERTEX)
// Поток 1: данные инстанса (PER_INSTANCE)
struct VSInput
{
    float3 Pos    : POSITION;
    float3 Norm   : NORMAL;
    float3 IPos   : IPOS;      // мировая позиция инстанса
    float  IScale : ISCALE;    // масштаб инстанса
    float  IYaw   : IYAW;      // поворот вокруг Y (радианы)
};

struct PSInput
{
    float4 PosH      : SV_POSITION;
    float3 WorldPos  : TEXCOORD0;
    float3 NormalW   : TEXCOORD1;
    float  ViewDepth : TEXCOORD2;
};

struct GBufferOutput
{
    float4 AlbedoSpec : SV_Target0;
    float4 WorldPos   : SV_Target1;
    float4 Normal     : SV_Target2;
    float4 Depth      : SV_Target3;
};

PSInput VSMain(VSInput input)
{
    PSInput o;

    // Поворот вокруг Y
    float sinY, cosY;
    sincos(input.IYaw, sinY, cosY);
    float3 rPos;
    rPos.x = input.Pos.x * cosY + input.Pos.z * sinY;
    rPos.y = input.Pos.y;
    rPos.z = -input.Pos.x * sinY + input.Pos.z * cosY;
    float3 rNorm;
    rNorm.x = input.Norm.x * cosY + input.Norm.z * sinY;
    rNorm.y = input.Norm.y;
    rNorm.z = -input.Norm.x * sinY + input.Norm.z * cosY;

    float3 worldPos = rPos * input.IScale + input.IPos;
    float4 posV     = mul(float4(worldPos, 1.0f), View);
    o.PosH          = mul(posV, Proj);
    o.WorldPos      = worldPos;
    o.NormalW       = rNorm;
    o.ViewDepth     = posV.z;
    return o;
}

GBufferOutput PSMain(PSInput input)
{
    GBufferOutput o;
    float3 normal = normalize(input.NormalW);
    float  depth  = saturate(input.ViewDepth / 20000.0f);

    // Серо-коричневый цвет, похожий на камень
    float3 albedo = float3(0.50f, 0.45f, 0.38f);

    o.AlbedoSpec = float4(albedo, 0.65f);
    o.WorldPos   = float4(input.WorldPos, 1.0f);
    o.Normal     = float4(normal * 0.5f + 0.5f, 1.0f);
    o.Depth      = float4(depth, depth, depth, 1.0f);
    return o;
}
