cbuffer ShadowCB : register(b0)
{
    float4x4 LightViewProj;
};

// Для ландшафта (stream 0: POSITION, NORMAL, TEXCOORD)
float4 VSMain(
    float3 pos  : POSITION,
    float3 norm : NORMAL,
    float2 uv   : TEXCOORD) : SV_POSITION
{
    return mul(float4(pos, 1.0f), LightViewProj);
}

// Для инстансинговых объектов (stream 0 + stream 1)
float4 VSMainInstanced(
    float3 Pos    : POSITION,
    float3 Norm   : NORMAL,
    float3 IPos   : IPOS,
    float  IScale : ISCALE,
    float  IYaw   : IYAW) : SV_POSITION
{
    float sinY, cosY;
    sincos(IYaw, sinY, cosY);
    float3 rPos;
    rPos.x = Pos.x * cosY + Pos.z * sinY;
    rPos.y = Pos.y;
    rPos.z = -Pos.x * sinY + Pos.z * cosY;
    float3 worldPos = rPos * IScale + IPos;
    return mul(float4(worldPos, 1.0f), LightViewProj);
}
