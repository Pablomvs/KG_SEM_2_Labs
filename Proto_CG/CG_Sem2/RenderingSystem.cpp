#include "RenderingSystem.h"

#include <array>
#include <cstring>
#include <cwchar>
#include <random>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    struct DebugOverlayConstants
    {
        XMFLOAT4 OverlayRect;
        XMFLOAT4 SceneCenter;
        XMFLOAT4 SceneExtents;
        UINT DebugMode = 0;
        UINT Padding[3] = {};
    };

    bool ResolveShaderPath(const wchar_t* fileName, wchar_t* outPath, size_t outPathCount)
    {
        wchar_t exeDir[MAX_PATH];
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);

        wchar_t* lastSlash = wcsrchr(exeDir, L'\\');
        if (lastSlash == nullptr)
        {
            return false;
        }
        *(lastSlash + 1) = 0;

        wcscpy_s(outPath, outPathCount, exeDir);
        wcscat_s(outPath, outPathCount, L"..\\..\\CG_Sem2\\");
        wcscat_s(outPath, outPathCount, fileName);

        if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(outPath, outPathCount, exeDir);
            wcscat_s(outPath, outPathCount, fileName);
        }

        return GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES;
    }
}

bool RenderingSystem::Initialize(HWND hwnd, UINT width, UINT height)
{
    m_width = width;
    m_height = height;

    if (!m_context.Initialize(hwnd, width, height))
    {
        return false;
    }

    if (!m_gbuffer.Initialize(m_context.GetDevice(), width, height))
    {
        return false;
    }

    return InitializeDeferredResources();
}
// корректное завершение рендера и очистка памяти
void RenderingSystem::Shutdown()
{
    if (m_deferredLightCBMappedData != nullptr && m_deferredLightConstantBuffer != nullptr)
    {
        m_deferredLightConstantBuffer->Unmap(0, nullptr);
        m_deferredLightCBMappedData = nullptr;
    }

    if (m_instanceMappedData && m_instanceBuffer)
    {
        m_instanceBuffer->Unmap(0, nullptr);
        m_instanceMappedData = nullptr;
    }
    if (m_instancedFrameCBMapped && m_instancedFrameCB)
    {
        m_instancedFrameCB->Unmap(0, nullptr);
        m_instancedFrameCBMapped = nullptr;
    }
    m_instanceBuffer.Reset();
    m_instancedFrameCB.Reset();
    m_cubeVB.Reset();
    m_cubeIB.Reset();
    m_instancedPSO.Reset();
    m_instancedRootSig.Reset();
    m_instancedVS.Reset();
    m_instancedPS.Reset();
    m_octree.reset();

    if (m_shadowPassCBMapped && m_shadowPassCB)
    {
        m_shadowPassCB->Unmap(0, nullptr);
        m_shadowPassCBMapped = nullptr;
    }
    if (m_cascadeShadowCBMapped && m_cascadeShadowCB)
    {
        m_cascadeShadowCB->Unmap(0, nullptr);
        m_cascadeShadowCBMapped = nullptr;
    }
    m_shadowPassCB.Reset();
    m_cascadeShadowCB.Reset();
    for (UINT c = 0; c < NumCascades; c++) m_shadowMaps[c].Reset();
    m_shadowDSVHeap.Reset();
    m_lightingSRVHeap.Reset();
    m_shadowTerrainPSO.Reset();
    m_shadowInstancedPSO.Reset();
    m_shadowRootSig.Reset();
    m_shadowTerrainVS.Reset();
    m_shadowInstancedVS.Reset();

    m_deferredLightConstantBuffer.Reset();
    m_deferredGeometryPSO.Reset();
    m_deferredLightingPSO.Reset();
    m_debugOverlayPSO.Reset();
    m_debugOverlayRootSignature.Reset();
    m_deferredLightingRootSignature.Reset();
    m_deferredGeometryVS.Reset();
    m_deferredGeometryHS.Reset();
    m_deferredGeometryDS.Reset();
    m_deferredGeometryPS.Reset();
    m_deferredLightingVS.Reset();
    m_deferredLightingPS.Reset();
    m_debugOverlayVS.Reset();
    m_debugOverlayPS.Reset();
    m_gbuffer.Shutdown();
    m_context.Shutdown();
}

void RenderingSystem::SetTechnique(Technique technique)
{
    m_technique = technique;
}

RenderingSystem::Technique RenderingSystem::GetTechnique() const
{
    return m_technique;
}

void RenderingSystem::SetClearColor(float r, float g, float b, float a)
{
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void RenderingSystem::SetTime(float timeSeconds)
{
    m_context.SetTime(timeSeconds);
}

void RenderingSystem::SetUVTiling(float x, float y)
{
    m_context.SetUVTiling(x, y);
}

void RenderingSystem::SetUVScrollSpeed(float uSpeed, float vSpeed)
{
    m_context.SetUVScrollSpeed(uSpeed, vSpeed);
}

void RenderingSystem::UpdateCameraOrbit(
    float deltaTime,
    float rotateSpeed,
    float dollySpeed,
    bool orbitRotate,
    bool dolly,
    float mouseDeltaX,
    float mouseDeltaY)
{
    m_context.UpdateCameraOrbit(
        deltaTime,
        rotateSpeed,
        dollySpeed,
        orbitRotate,
        dolly,
        mouseDeltaX,
        mouseDeltaY);
}

// выбирает способ отрисовки
void RenderingSystem::RenderFrame()
{
    switch (m_technique)
    {
    case Technique::Deferred:
        RenderDeferredFrame();
        break;
    case Technique::Forward:
    default:
        RenderForwardFrame();
        break;
    }
}

void RenderingSystem::RenderForwardFrame()
{
    m_context.Render(
        m_clearColor[0],
        m_clearColor[1],
        m_clearColor[2],
        m_clearColor[3]);
}

void RenderingSystem::RenderDeferredFrame()
{
    if (!m_deferredGeometryPSO || !m_deferredLightingPSO)
    {
        RenderForwardFrame();
        return;
    }

    m_context.BeginFrame();
    CullAndUpdateInstances();
    ComputeCascades();
    RenderShadowPass();
    RenderOpaqueStage();
    RenderLightingStage();
    RenderGBufferDebugOverlay();
    RenderTransparentStage();
    m_context.EndFrame();
}
// свойства поверхности для GBuffer
void RenderingSystem::RenderOpaqueStage()
{
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_context.GetDepthStencilView();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_gbuffer.BeginGeometryPass(commandList, dsv);

    // Рисуем тесселированный ландшафт
    commandList->SetPipelineState(m_deferredGeometryPSO.Get());
    commandList->SetGraphicsRootSignature(m_context.GetSceneRootSignature());

    ID3D12DescriptorHeap* heaps[] = { m_context.GetSceneSRVHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    m_context.UpdateSceneConstants();
    commandList->SetGraphicsRootConstantBufferView(0, m_context.GetSceneConstantBufferAddress());
    m_context.DrawSceneGeometry(commandList, 1, 2);

    // Рисуем инстансинговые объекты (в те же GBuffer-цели)
    if (m_instancedPSO && !m_visibleObjects.empty())
        RenderInstances(commandList);

    m_gbuffer.EndGeometryPass(commandList);
}

// рачсет освещения
void RenderingSystem::RenderLightingStage()
{
    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_VIEWPORT vp = m_context.GetViewport();
    D3D12_RECT sc = m_context.GetScissorRect();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();

    UpdateLightingConstants();

    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sc);
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    commandList->ClearRenderTargetView(backBufferRtv, m_clearColor, 0, nullptr);

    commandList->SetPipelineState(m_deferredLightingPSO.Get());
    commandList->SetGraphicsRootSignature(m_deferredLightingRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_lightingSRVHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE heapStart = m_lightingSRVHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetGraphicsRootDescriptorTable(0, heapStart);
    commandList->SetGraphicsRootConstantBufferView(1, m_deferredLightConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE shadowStart = heapStart;
    shadowStart.ptr += GBuffer::TargetCount * m_lightingSRVDescSize;
    commandList->SetGraphicsRootDescriptorTable(2, shadowStart);
    commandList->SetGraphicsRootConstantBufferView(3, m_cascadeShadowCB->GetGPUVirtualAddress());

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::RenderTransparentStage()
{
}

// инициализация Deferred Resources
bool RenderingSystem::InitializeDeferredResources()
{
    return CompileDeferredShaders() &&
        CreateDeferredLightingRootSignature() &&
        CreateDeferredGeometryPipeline() &&
        CreateDeferredLightingPipeline() &&
        CreateDebugOverlayRootSignature() &&
        CreateDebugOverlayPipeline() &&
        CreateLightingConstantBuffer() &&
        InitializeInstancedObjects() &&
        InitializeShadowResources();
}

bool RenderingSystem::CompileDeferredShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    wchar_t geometryPath[MAX_PATH];
    wchar_t lightingPath[MAX_PATH];
    wchar_t debugPath[MAX_PATH];
    if (!ResolveShaderPath(L"DeferredGeometry.hlsl", geometryPath, MAX_PATH) ||
        !ResolveShaderPath(L"DeferredLighting.hlsl", lightingPath, MAX_PATH) ||
        !ResolveShaderPath(L"GBufferDebug.hlsl", debugPath, MAX_PATH))
    {
        return false;
    }

    HRESULT hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_deferredGeometryVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "HSMain",
        "hs_5_0",
        flags,
        0,
        &m_deferredGeometryHS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "DSMain",
        "ds_5_0",
        flags,
        0,
        &m_deferredGeometryDS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        geometryPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_deferredGeometryPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        lightingPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_deferredLightingVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        lightingPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_deferredLightingPS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        debugPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain",
        "vs_5_0",
        flags,
        0,
        &m_debugOverlayVS,
        nullptr);
    if (FAILED(hr))
    {
        return false;
    }

    hr = D3DCompileFromFile(
        debugPath,
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain",
        "ps_5_0",
        flags,
        0,
        &m_debugOverlayPS,
        nullptr);
    return SUCCEEDED(hr);
}

// что может читать шейдер
bool RenderingSystem::CreateDeferredLightingRootSignature()
{
    // t0-t3: GBuffer
    D3D12_DESCRIPTOR_RANGE gbufRange{};
    gbufRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufRange.NumDescriptors = GBuffer::TargetCount;
    gbufRange.BaseShaderRegister = 0;
    gbufRange.OffsetInDescriptorsFromTableStart = 0;

    // t4-t6: shadow maps
    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = NumCascades;
    shadowRange.BaseShaderRegister = 4;
    shadowRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[4]{};
    // 0: GBuffer SRV table
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &gbufRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 1: DeferredLightCB (b0)
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 2: shadow SRV table
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &shadowRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // 3: CascadeShadowCB (b1)
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 1;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Статический сэмплер сравнения (PCF)
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.ShaderRegister   = 0;
    shadowSampler.RegisterSpace    = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 4;
    desc.pParameters       = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &shadowSampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr))
        return false;

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_deferredLightingRootSignature)));
}

bool RenderingSystem::CreateDebugOverlayRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = GBuffer::TargetCount;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[1].Constants.ShaderRegister = 0;
    rootParams[1].Constants.Num32BitValues = sizeof(DebugOverlayConstants) / sizeof(UINT);
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(rootParams);
    desc.pParameters = rootParams;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &error);
    if (FAILED(hr))
    {
        return false;
    }

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_debugOverlayRootSignature)));
}

//сцена рисуется не в один цветовой буфер, а сразу в несколько текстур GBuffer
bool RenderingSystem::CreateDeferredGeometryPipeline()
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { layout, _countof(layout) };
    pso.pRootSignature = m_context.GetSceneRootSignature();
    pso.VS = { m_deferredGeometryVS->GetBufferPointer(), m_deferredGeometryVS->GetBufferSize() };
    pso.HS = { m_deferredGeometryHS->GetBufferPointer(), m_deferredGeometryHS->GetBufferSize() };
    pso.DS = { m_deferredGeometryDS->GetBufferPointer(), m_deferredGeometryDS->GetBufferSize() };
    pso.PS = { m_deferredGeometryPS->GetBufferPointer(), m_deferredGeometryPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = TRUE;

    for (UINT i = 0; i < 8; ++i)
    {
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable = FALSE;

    pso.NumRenderTargets = GBuffer::TargetCount;
    pso.RTVFormats[0] = m_gbuffer.GetFormat(GBuffer::Slot::AlbedoSpec);
    pso.RTVFormats[1] = m_gbuffer.GetFormat(GBuffer::Slot::WorldPosition);
    pso.RTVFormats[2] = m_gbuffer.GetFormat(GBuffer::Slot::Normal);
    pso.RTVFormats[3] = m_gbuffer.GetFormat(GBuffer::Slot::Depth);
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_deferredGeometryPSO)));
}

// считывание освещения из буфера
bool RenderingSystem::CreateDeferredLightingPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_deferredLightingRootSignature.Get();
    pso.VS = { m_deferredLightingVS->GetBufferPointer(), m_deferredLightingVS->GetBufferSize() };
    pso.PS = { m_deferredLightingPS->GetBufferPointer(), m_deferredLightingPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_deferredLightingPSO)));
}

bool RenderingSystem::CreateDebugOverlayPipeline()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_debugOverlayRootSignature.Get();
    pso.VS = { m_debugOverlayVS->GetBufferPointer(), m_debugOverlayVS->GetBufferSize() };
    pso.PS = { m_debugOverlayPS->GetBufferPointer(), m_debugOverlayPS->GetBufferSize() };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.AlphaToCoverageEnable = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_debugOverlayPSO)));
}

void RenderingSystem::RenderGBufferDebugOverlay()
{
    if (!m_debugOverlayPSO || !m_debugOverlayRootSignature)
    {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_context.GetCommandList();
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = m_context.GetCurrentBackBufferRTV();

    ID3D12DescriptorHeap* heaps[] = { m_gbuffer.GetSRVHeap() };
    commandList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    commandList->SetPipelineState(m_debugOverlayPSO.Get());
    commandList->SetGraphicsRootSignature(m_debugOverlayRootSignature.Get());
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootDescriptorTable(0, m_gbuffer.GetSRVGPU(GBuffer::Slot::AlbedoSpec));
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float width = static_cast<float>(m_width);
    const float height = static_cast<float>(m_height);
    const float padding = (std::max)(12.0f, width * 0.0125f);
    const float tileWidth = (std::max)(width * 0.22f, 180.0f);
    const float tileHeight = tileWidth * 0.58f;

    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();

    const std::array<D3D12_VIEWPORT, GBuffer::TargetCount> viewports =
    {
        D3D12_VIEWPORT{ padding, padding, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ width - padding - tileWidth, padding, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ padding, height - padding - tileHeight, tileWidth, tileHeight, 0.0f, 1.0f },
        D3D12_VIEWPORT{ width - padding - tileWidth, height - padding - tileHeight, tileWidth, tileHeight, 0.0f, 1.0f }
    };

    for (UINT i = 0; i < GBuffer::TargetCount; ++i)
    {
        const D3D12_VIEWPORT& viewport = viewports[i];
        D3D12_RECT scissor{};
        scissor.left = static_cast<LONG>(viewport.TopLeftX);
        scissor.top = static_cast<LONG>(viewport.TopLeftY);
        scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissor.bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height);

        DebugOverlayConstants constants{};
        constants.OverlayRect = XMFLOAT4(
            viewport.TopLeftX,
            viewport.TopLeftY,
            viewport.Width,
            viewport.Height);
        constants.SceneCenter = XMFLOAT4(sceneCenter.x, sceneCenter.y, sceneCenter.z, 1.0f);
        constants.SceneExtents = XMFLOAT4(sceneExtents.x, sceneExtents.y, sceneExtents.z, 1.0f);
        constants.DebugMode = i;

        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->SetGraphicsRoot32BitConstants(
            1,
            sizeof(DebugOverlayConstants) / sizeof(UINT),
            &constants,
            0);
        commandList->DrawInstanced(3, 1, 0, 0);
    }
}
// передача источников света в буфер света
bool RenderingSystem::CreateLightingConstantBuffer()
{
    const UINT cbSize = (sizeof(DeferredLightCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cbSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &upload,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_deferredLightConstantBuffer))))
    {
        return false;
    }

    D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(m_deferredLightConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_deferredLightCBMappedData))))
    {
        return false;
    }

    UpdateLightingConstants();

    return true;
}

void RenderingSystem::UpdateLightingConstants()
{
    if (m_deferredLightCBMappedData == nullptr)
    {
        return;
    }

    DeferredLightCB cb{};
    const XMFLOAT3 sceneCenter = m_context.GetSceneCenter();
    const XMFLOAT3 sceneExtents = m_context.GetSceneExtents();
    const float dominantExtent = (std::max)(sceneExtents.x, (std::max)(sceneExtents.y, sceneExtents.z));
    const float pointRange = (std::max)(dominantExtent * 0.95f, 900.0f);
    const float spotRange = (std::max)(dominantExtent * 0.90f, 900.0f);

    cb.LightDirection = XMFLOAT4(0.88f, -1.0f, -0.34f, 0.0f);
    cb.LightColor = XMFLOAT4(1.00f, 0.95f, 0.88f, 3.40f);
    cb.AmbientColor = XMFLOAT4(0.09f, 0.10f, 0.12f, 1.0f);
    cb.LightCounts = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.0f);

    cb.PointLightPositionRange[0] = XMFLOAT4(
        sceneCenter.x + sceneExtents.x * 1.80f,
        sceneCenter.y + sceneExtents.y * 1.25f,
        sceneCenter.z - sceneExtents.z * 1.35f,
        pointRange);
    cb.PointLightColorIntensity[0] = XMFLOAT4(1.00f, 0.94f, 0.86f, 2.10f);
    cb.ScreenSize = XMFLOAT4(
        static_cast<float>(m_width),
        static_cast<float>(m_height),
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    const XMFLOAT3 cameraPosValue = m_context.GetCameraPosition();
    const XMFLOAT3 cameraTargetValue = m_context.GetCameraTarget();

    XMVECTOR cameraPos = XMLoadFloat3(&cameraPosValue);
    XMVECTOR cameraTarget = XMLoadFloat3(&cameraTargetValue);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(cameraPos, cameraTarget, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        static_cast<float>(m_width) / static_cast<float>(m_height),
        1.0f,
        20000.0f);

    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    XMStoreFloat4x4(&cb.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&cb.InvProj, XMMatrixTranspose(invProj));

    std::memcpy(m_deferredLightCBMappedData, &cb, sizeof(cb));
}

// ============================================================
// Инстансинг + фрустум-отсечение + октодерево
// ============================================================

bool RenderingSystem::InitializeInstancedObjects()
{
    // Генерируем случайные позиции в пределах плоскости (XZ: [-270, 270])
    m_allObjects.clear();
    m_allObjects.reserve(ObjectCount);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> posXZ(-270.f, 270.f);
    std::uniform_real_distribution<float> scaleVar(0.6f, 1.5f);
    std::uniform_real_distribution<float> yawDist(0.f, 6.2832f); // 0..2π

    for (UINT i = 0; i < ObjectCount; ++i)
    {
        InstanceData obj;
        obj.Scale    = ObjectScale * scaleVar(rng);
        obj.Yaw      = yawDist(rng);
        // Y=0: основание лежит на поверхности земли (куб идёт от 0 до +1 по Y)
        obj.WorldPos = { posXZ(rng), 0.0f, posXZ(rng) };
        m_allObjects.push_back(obj);
    }

    // Строим октодерево по центрам объектов
    std::vector<XMFLOAT3> positions(ObjectCount);
    for (UINT i = 0; i < ObjectCount; ++i) positions[i] = m_allObjects[i].WorldPos;

    m_octree = std::make_unique<Octree>();
    m_octree->Build(positions, ObjectRadius, 320.f);

    m_visibleObjects.reserve(ObjectCount);

    return CompileInstancedShader()      &&
           CreateInstancedRootSignature() &&
           CreateInstancedPipeline()      &&
           CreateInstancedGeometry()      &&
           CreateInstanceBuffer()         &&
           CreateInstancedCB();
}

bool RenderingSystem::CompileInstancedShader()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    wchar_t path[MAX_PATH];
    if (!ResolveShaderPath(L"Instanced.hlsl", path, MAX_PATH))
        return false;

    ComPtr<ID3DBlob> err;
    if (FAILED(D3DCompileFromFile(path, nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &m_instancedVS, &err)))
        return false;

    return SUCCEEDED(D3DCompileFromFile(path, nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &m_instancedPS, &err));
}

bool RenderingSystem::CreateInstancedRootSignature()
{
    // Только один CBV (b0) — View и Proj матрицы
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters   = &param;
    desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return false;

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_instancedRootSig)));
}

bool RenderingSystem::CreateInstancedPipeline()
{
    D3D12_INPUT_ELEMENT_DESC layout[] =
    {
        // Поток 0: геометрия куба (PER_VERTEX)
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
        // Поток 1: данные инстанса (PER_INSTANCE, шаг 1)
        { "IPOS",     0, DXGI_FORMAT_R32G32B32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "ISCALE",   0, DXGI_FORMAT_R32_FLOAT,       1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        { "IYAW",     0, DXGI_FORMAT_R32_FLOAT,       1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout               = { layout, _countof(layout) };
    pso.pRootSignature            = m_instancedRootSig.Get();
    pso.VS = { m_instancedVS->GetBufferPointer(), m_instancedVS->GetBufferSize() };
    pso.PS = { m_instancedPS->GetBufferPointer(), m_instancedPS->GetBufferSize() };
    pso.PrimitiveTopologyType     = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.SampleMask                = UINT_MAX;
    pso.RasterizerState.FillMode  = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode  = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;
    pso.BlendState.IndependentBlendEnable     = TRUE;
    for (UINT i = 0; i < 8; ++i)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilEnable  = FALSE;
    pso.NumRenderTargets = GBuffer::TargetCount;
    pso.RTVFormats[0]    = m_gbuffer.GetFormat(GBuffer::Slot::AlbedoSpec);
    pso.RTVFormats[1]    = m_gbuffer.GetFormat(GBuffer::Slot::WorldPosition);
    pso.RTVFormats[2]    = m_gbuffer.GetFormat(GBuffer::Slot::Normal);
    pso.RTVFormats[3]    = m_gbuffer.GetFormat(GBuffer::Slot::Depth);
    pso.DSVFormat        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    return SUCCEEDED(m_context.GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_instancedPSO)));
}

bool RenderingSystem::CreateInstancedGeometry()
{
    // Форма камня: основание (y=0) широкое, верхушка (y=1) уже.
    // b=0.95 — ширина основания, t=0.65 — ширина верхушки.
    // Куб: 24 вершины (4 на грань × 6 граней), 36 индексов
    const float b = 0.95f, t = 0.65f;
    const InstanceVertex cubeVerts[] =
    {
        // Передняя (z наружу, n=(0,0,-1))
        {{-b, 0,-b},{0,0,-1}}, {{ b, 0,-b},{0,0,-1}}, {{ t, 1,-t},{0,0,-1}}, {{-t, 1,-t},{0,0,-1}},
        // Задняя
        {{ b, 0, b},{0,0, 1}}, {{-b, 0, b},{0,0, 1}}, {{-t, 1, t},{0,0, 1}}, {{ t, 1, t},{0,0, 1}},
        // Левая
        {{-b, 0, b},{-1,0,0}}, {{-b, 0,-b},{-1,0,0}}, {{-t, 1,-t},{-1,0,0}}, {{-t, 1, t},{-1,0,0}},
        // Правая
        {{ b, 0,-b},{ 1,0,0}}, {{ b, 0, b},{ 1,0,0}}, {{ t, 1, t},{ 1,0,0}}, {{ t, 1,-t},{ 1,0,0}},
        // Нижняя (y=0, на земле — невидима, но нужна для полноты)
        {{-b, 0, b},{0,-1,0}}, {{ b, 0, b},{0,-1,0}}, {{ b, 0,-b},{0,-1,0}}, {{-b, 0,-b},{0,-1,0}},
        // Верхняя (y=1, плоский верх)
        {{-t, 1,-t},{0, 1,0}}, {{ t, 1,-t},{0, 1,0}}, {{ t, 1, t},{0, 1,0}}, {{-t, 1, t},{0, 1,0}},
    };
    const UINT16 cubeIdx[] =
    {
         0, 1, 2,  0, 2, 3,
         4, 5, 6,  4, 6, 7,
         8, 9,10,  8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };
    m_cubeIndexCount = _countof(cubeIdx);

    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;

    auto makeBuffer = [&](UINT byteSize, ComPtr<ID3D12Resource>& outRes) -> bool
    {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width              = byteSize;
        bd.Height             = 1;
        bd.DepthOrArraySize   = 1;
        bd.MipLevels          = 1;
        bd.SampleDesc.Count   = 1;
        bd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(m_context.GetDevice()->CreateCommittedResource(
            &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outRes)));
    };

    // Vertex buffer
    const UINT vbSize = sizeof(cubeVerts);
    if (!makeBuffer(vbSize, m_cubeVB)) return false;
    UINT8* vbData;
    if (FAILED(m_cubeVB->Map(0, nullptr, (void**)&vbData))) return false;
    memcpy(vbData, cubeVerts, vbSize);
    m_cubeVB->Unmap(0, nullptr);
    // D3D12_VERTEX_BUFFER_VIEW: BufferLocation, SizeInBytes, StrideInBytes
    m_cubeVBView = { m_cubeVB->GetGPUVirtualAddress(), vbSize, sizeof(InstanceVertex) };

    // Index buffer
    const UINT ibSize = sizeof(cubeIdx);
    if (!makeBuffer(ibSize, m_cubeIB)) return false;
    UINT8* ibData;
    if (FAILED(m_cubeIB->Map(0, nullptr, (void**)&ibData))) return false;
    memcpy(ibData, cubeIdx, ibSize);
    m_cubeIB->Unmap(0, nullptr);
    m_cubeIBView = { m_cubeIB->GetGPUVirtualAddress(), ibSize, DXGI_FORMAT_R16_UINT };

    return true;
}

bool RenderingSystem::CreateInstanceBuffer()
{
    const UINT bufSize = ObjectCount * sizeof(InstanceData);

    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = bufSize;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_instanceBuffer)))) return false;

    D3D12_RANGE readRange{0, 0};
    if (FAILED(m_instanceBuffer->Map(0, &readRange, (void**)&m_instanceMappedData))) return false;

    m_instanceVBView = { m_instanceBuffer->GetGPUVirtualAddress(), bufSize, sizeof(InstanceData) };
    return true;
}

bool RenderingSystem::CreateInstancedCB()
{
    const UINT cbSize = (sizeof(PerFrameInstancedCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = cbSize;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_context.GetDevice()->CreateCommittedResource(
        &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_instancedFrameCB)))) return false;

    D3D12_RANGE readRange{0, 0};
    return SUCCEEDED(m_instancedFrameCB->Map(0, &readRange, (void**)&m_instancedFrameCBMapped));
}

void RenderingSystem::CullAndUpdateInstances()
{
    m_visibleObjects.clear();
    if (m_allObjects.empty() || !m_instanceMappedData) return;

    if (!m_frustumCullingEnabled)
    {
        // Отсечение выключено — рисуем все объекты
        m_visibleObjects = m_allObjects;
    }
    else
    {
        // Строим матрицу ViewProj для извлечения плоскостей фрустума
        const XMFLOAT3 camPosF = m_context.GetCameraPosition();
        const XMFLOAT3 camTgtF = m_context.GetCameraTarget();
        XMVECTOR camPos = XMLoadFloat3(&camPosF);
        XMVECTOR camTgt = XMLoadFloat3(&camTgtF);
        XMVECTOR up     = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        XMMATRIX view   = XMMatrixLookAtLH(camPos, camTgt, up);
        XMMATRIX proj   = XMMatrixPerspectiveFovLH(
            XM_PIDIV4, (float)m_width / (float)m_height, 1.f, 20000.f);
        XMMATRIX vp = XMMatrixMultiply(view, proj);

        XMFLOAT4X4 vpF;
        XMStoreFloat4x4(&vpF, vp);

        FrustumPlane planes[6];
        ExtractFrustumPlanes(vpF, planes);

        if (m_octreeEnabled && m_octree)
        {
            // Отсечение через октодерево
            std::vector<int> indices;
            m_octree->QueryFrustum(planes, indices);
            for (int idx : indices)
                m_visibleObjects.push_back(m_allObjects[idx]);
        }
        else
        {
            // Грубая сила: проверяем каждый объект
            for (const auto& obj : m_allObjects)
                if (SphereInFrustum(obj.WorldPos, obj.Scale * 1.733f, planes))
                    m_visibleObjects.push_back(obj);
        }
    }

    if (!m_visibleObjects.empty())
        memcpy(m_instanceMappedData, m_visibleObjects.data(),
               m_visibleObjects.size() * sizeof(InstanceData));
}

void RenderingSystem::RenderInstances(ID3D12GraphicsCommandList* commandList)
{
    if (m_visibleObjects.empty() || !m_instancedFrameCBMapped) return;

    // Обновляем View/Proj в CB инстансинга
    const XMFLOAT3 camPosF = m_context.GetCameraPosition();
    const XMFLOAT3 camTgtF = m_context.GetCameraTarget();
    XMVECTOR camPos = XMLoadFloat3(&camPosF);
    XMVECTOR camTgt = XMLoadFloat3(&camTgtF);
    XMVECTOR up     = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMMATRIX view   = XMMatrixLookAtLH(camPos, camTgt, up);
    XMMATRIX proj   = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, (float)m_width / (float)m_height, 1.f, 20000.f);

    PerFrameInstancedCB cb;
    XMStoreFloat4x4(&cb.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&cb.Proj, XMMatrixTranspose(proj));
    memcpy(m_instancedFrameCBMapped, &cb, sizeof(cb));

    commandList->SetPipelineState(m_instancedPSO.Get());
    commandList->SetGraphicsRootSignature(m_instancedRootSig.Get());
    commandList->SetGraphicsRootConstantBufferView(0, m_instancedFrameCB->GetGPUVirtualAddress());

    D3D12_VERTEX_BUFFER_VIEW vbViews[2] = { m_cubeVBView, m_instanceVBView };
    commandList->IASetVertexBuffers(0, 2, vbViews);
    commandList->IASetIndexBuffer(&m_cubeIBView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawIndexedInstanced(m_cubeIndexCount, (UINT)m_visibleObjects.size(), 0, 0, 0);
}

// ============================================================
// Каскадные карты теней (CSM + PCF)
// ============================================================

bool RenderingSystem::InitializeShadowResources()
{
    auto* device = m_context.GetDevice();
    m_lightingSRVDescSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 1. Объединённый SRV heap: 4 GBuffer + 3 shadow
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = GBuffer::TargetCount + NumCascades;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_lightingSRVHeap))))
            return false;
    }

    // 2. Копируем GBuffer SRV в позиции 0-3
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dst =
            m_lightingSRVHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < GBuffer::TargetCount; ++i)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE src =
                m_gbuffer.GetSRV(static_cast<GBuffer::Slot>(i));
            device->CopyDescriptorsSimple(1, dst, src,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            dst.ptr += m_lightingSRVDescSize;
        }
    }

    // 3. Создаём текстуры shadow map (R32_TYPELESS)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = ShadowMapSize;
        rd.Height           = ShadowMapSize;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_R32_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv{};
        cv.Format               = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth   = 1.0f;

        for (UINT c = 0; c < NumCascades; ++c)
        {
            if (FAILED(device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                IID_PPV_ARGS(&m_shadowMaps[c]))))
                return false;
        }
    }

    // 4. DSV heap (3 слота)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = NumCascades;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_shadowDSVHeap))))
            return false;

        UINT dsvSize = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT c = 0; c < NumCascades; ++c)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
            dsvd.Format        = DXGI_FORMAT_D32_FLOAT;
            dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(m_shadowMaps[c].Get(), &dsvd, dsvHandle);
            dsvHandle.ptr += dsvSize;
        }
    }

    // 5. SRV для shadow map в позиции 4-6 объединённого heap
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvBase =
            m_lightingSRVHeap->GetCPUDescriptorHandleForHeapStart();
        srvBase.ptr += GBuffer::TargetCount * m_lightingSRVDescSize;

        for (UINT c = 0; c < NumCascades; ++c)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
            srvd.Format                    = DXGI_FORMAT_R32_FLOAT;
            srvd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvd.Texture2D.MipLevels       = 1;

            D3D12_CPU_DESCRIPTOR_HANDLE h = srvBase;
            h.ptr += c * m_lightingSRVDescSize;
            device->CreateShaderResourceView(m_shadowMaps[c].Get(), &srvd, h);
        }
    }

    // 6. Shadow pass CB: NumCascades слотов по 256 байт
    {
        const UINT slotSize  = 256u; // sizeof(XMFLOAT4X4)=64 выровнено до 256
        const UINT totalSize = slotSize * NumCascades;

        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = totalSize;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_shadowPassCB))))
            return false;

        m_shadowPassCB->Map(0, nullptr,
            reinterpret_cast<void**>(&m_shadowPassCBMapped));
    }

    // 7. Cascade shadow CB для lighting pass
    {
        const UINT cbSize = (sizeof(CascadeShadowCB) + 255) & ~255u;

        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = cbSize;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_cascadeShadowCB))))
            return false;

        m_cascadeShadowCB->Map(0, nullptr,
            reinterpret_cast<void**>(&m_cascadeShadowCBMapped));
    }

    return CompileShadowShaders() &&
           CreateShadowRootSignature() &&
           CreateShadowPipelines();
}

bool RenderingSystem::CompileShadowShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    wchar_t path[MAX_PATH];
    if (!ResolveShaderPath(L"ShadowMap.hlsl", path, MAX_PATH))
        return false;

    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(path, nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain", "vs_5_0", flags, 0, &m_shadowTerrainVS, &err);
    if (FAILED(hr))
    {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }

    hr = D3DCompileFromFile(path, nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMainInstanced", "vs_5_0", flags, 0, &m_shadowInstancedVS, &err);
    if (FAILED(hr))
    {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }
    return true;
}

bool RenderingSystem::CreateShadowRootSignature()
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters   = &param;
    desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) return false;

    return SUCCEEDED(m_context.GetDevice()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_shadowRootSig)));
}

bool RenderingSystem::CreateShadowPipelines()
{
    auto* device = m_context.GetDevice();

    // Общий rasterizer для shadow pass (bias чтобы убрать acne)
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode              = D3D12_FILL_MODE_SOLID;
    raster.CullMode              = D3D12_CULL_MODE_BACK;
    raster.DepthBias             = 5000;
    raster.SlopeScaledDepthBias  = 2.0f;
    raster.DepthClipEnable       = TRUE;

    D3D12_DEPTH_STENCIL_DESC dsState{};
    dsState.DepthEnable    = TRUE;
    dsState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    // --- PSO для ландшафта (POSITION, NORMAL, TEXCOORD) ---
    {
        D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout            = { layout, _countof(layout) };
        pso.pRootSignature         = m_shadowRootSig.Get();
        pso.VS                     = { m_shadowTerrainVS->GetBufferPointer(),
                                       m_shadowTerrainVS->GetBufferSize() };
        pso.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.SampleMask             = UINT_MAX;
        pso.RasterizerState        = raster;
        pso.DepthStencilState      = dsState;
        pso.NumRenderTargets       = 0;
        pso.DSVFormat              = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count       = 1;

        if (FAILED(device->CreateGraphicsPipelineState(
                &pso, IID_PPV_ARGS(&m_shadowTerrainPSO))))
            return false;
    }

    // --- PSO для инстансинга (stream 0 + stream 1) ---
    {
        D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
            { "IPOS",     0, DXGI_FORMAT_R32G32B32_FLOAT, 1,  0,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "ISCALE",   0, DXGI_FORMAT_R32_FLOAT,       1, 12,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "IYAW",     0, DXGI_FORMAT_R32_FLOAT,       1, 16,
              D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout            = { layout, _countof(layout) };
        pso.pRootSignature         = m_shadowRootSig.Get();
        pso.VS                     = { m_shadowInstancedVS->GetBufferPointer(),
                                       m_shadowInstancedVS->GetBufferSize() };
        pso.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.SampleMask             = UINT_MAX;
        pso.RasterizerState        = raster;
        pso.DepthStencilState      = dsState;
        pso.NumRenderTargets       = 0;
        pso.DSVFormat              = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count       = 1;

        if (FAILED(device->CreateGraphicsPipelineState(
                &pso, IID_PPV_ARGS(&m_shadowInstancedPSO))))
            return false;
    }
    return true;
}

void RenderingSystem::ComputeCascades()
{
    const XMFLOAT3 camPosF  = m_context.GetCameraPosition();
    const XMFLOAT3 camTgtF  = m_context.GetCameraTarget();
    XMVECTOR eye    = XMLoadFloat3(&camPosF);
    XMVECTOR target = XMLoadFloat3(&camTgtF);
    XMVECTOR up     = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    XMMATRIX view    = XMMatrixLookAtLH(eye, target, up);
    XMMATRIX invView = XMMatrixInverse(nullptr, view);

    const float aspect    = (float)m_width / (float)m_height;
    const float fovY      = XM_PIDIV4;
    const float nearZ     = 1.0f;
    const float shadowFar = 1500.0f;
    const float lambda    = 0.85f;

    // Граница каскадов (log-linear mix)
    float splits[NumCascades];
    float ratio = shadowFar / nearZ;
    for (UINT c = 0; c < NumCascades; ++c)
    {
        float p   = (float)(c + 1) / NumCascades;
        float log = nearZ * powf(ratio, p);
        float uni = nearZ + (shadowFar - nearZ) * p;
        splits[c] = lambda * log + (1.f - lambda) * uni;
    }

    // Направление источника света (совпадает с UpdateLightingConstants)
    XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.88f, -1.0f, -0.34f, 0.f));

    float tanHalfY = tanf(fovY * 0.5f);
    float tanHalfX = tanHalfY * aspect;

    float prevZ = nearZ;
    for (UINT c = 0; c < NumCascades; ++c)
    {
        float curZ = splits[c];

        // 8 углов frustum-среза в view space
        XMVECTOR corners[8];
        float depths[2] = { prevZ, curZ };
        for (int d = 0; d < 2; ++d)
        {
            float z = depths[d];
            float x = z * tanHalfX;
            float y = z * tanHalfY;
            corners[d * 4 + 0] = XMVectorSet(-x,  y, z, 1.f);
            corners[d * 4 + 1] = XMVectorSet( x,  y, z, 1.f);
            corners[d * 4 + 2] = XMVectorSet(-x, -y, z, 1.f);
            corners[d * 4 + 3] = XMVectorSet( x, -y, z, 1.f);
        }

        // Переводим в world space
        for (auto& v : corners)
            v = XMVector4Transform(v, invView);

        // Центр frustum-среза
        XMVECTOR center = XMVectorZero();
        for (auto& v : corners) center = XMVectorAdd(center, v);
        center = XMVectorScale(center, 1.f / 8.f);

        // Радиус ограничивающей сферы
        float radius = 0.f;
        for (auto& v : corners)
        {
            float d = XMVectorGetX(XMVector3Length(XMVectorSubtract(v, center)));
            if (d > radius) radius = d;
        }
        // Округляем до размера текселя чтобы уменьшить мерцание
        float texelSize = (radius * 2.f) / (float)ShadowMapSize;
        radius = ceilf(radius / texelSize) * texelSize;

        // Строим light view: камера смотрит из позиции за сценой вдоль lightDir
        XMVECTOR lightPos = XMVectorSubtract(
            center, XMVectorScale(lightDir, radius + 100.f));
        XMMATRIX lightView = XMMatrixLookAtLH(lightPos, center, up);

        // Ортографическая проекция плотно облегает сферу
        XMMATRIX lightProj = XMMatrixOrthographicLH(
            radius * 2.f, radius * 2.f, 0.f, radius * 2.f + 200.f);

        XMMATRIX lightVP = XMMatrixMultiply(lightView, lightProj);

        // Транспонируем для GPU (row-vector convention)
        XMStoreFloat4x4(&m_cascadeData.LightViewProj[c], XMMatrixTranspose(lightVP));

        prevZ = curZ;
    }

    m_cascadeData.CascadeSplits = XMFLOAT4(splits[0], splits[1], splits[2], shadowFar);

    // Загружаем в CB для lighting pass
    memcpy(m_cascadeShadowCBMapped, &m_cascadeData, sizeof(CascadeShadowCB));
}

void RenderingSystem::RenderShadowPass()
{
    if (!m_shadowTerrainPSO || !m_shadowPassCBMapped) return;

    auto* cmd = m_context.GetCommandList();

    // Переходим в DEPTH_WRITE (в первом кадре уже в этом состоянии)
    if (m_shadowMapsReadable)
    {
        D3D12_RESOURCE_BARRIER barriers[NumCascades]{};
        for (UINT c = 0; c < NumCascades; ++c)
        {
            barriers[c].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[c].Transition.pResource   = m_shadowMaps[c].Get();
            barriers[c].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[c].Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[c].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(NumCascades, barriers);
    }

    D3D12_VIEWPORT svp{ 0.f, 0.f, (float)ShadowMapSize, (float)ShadowMapSize, 0.f, 1.f };
    D3D12_RECT     ssc{ 0, 0, (LONG)ShadowMapSize, (LONG)ShadowMapSize };
    cmd->RSSetViewports(1, &svp);
    cmd->RSSetScissorRects(1, &ssc);
    cmd->SetGraphicsRootSignature(m_shadowRootSig.Get());

    UINT dsvDescSize = m_context.GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    constexpr UINT cbSlot = 256u; // stride между слотами shadow pass CB

    for (UINT c = 0; c < NumCascades; ++c)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            m_shadowDSVHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += c * dsvDescSize;

        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

        // Кладём LightViewProj каскада c в нужный слот CB
        memcpy(m_shadowPassCBMapped + c * cbSlot,
               &m_cascadeData.LightViewProj[c], sizeof(XMFLOAT4X4));

        D3D12_GPU_VIRTUAL_ADDRESS cbAddr =
            m_shadowPassCB->GetGPUVirtualAddress() + c * cbSlot;
        cmd->SetGraphicsRootConstantBufferView(0, cbAddr);

        // Рисуем ландшафт
        cmd->SetPipelineState(m_shadowTerrainPSO.Get());
        m_context.DrawMeshForShadow(cmd);

        // Рисуем инстансинговые камни
        if (!m_visibleObjects.empty() && m_shadowInstancedPSO)
        {
            cmd->SetPipelineState(m_shadowInstancedPSO.Get());
            D3D12_VERTEX_BUFFER_VIEW vbViews[2] = { m_cubeVBView, m_instanceVBView };
            cmd->IASetVertexBuffers(0, 2, vbViews);
            cmd->IASetIndexBuffer(&m_cubeIBView);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawIndexedInstanced(
                m_cubeIndexCount, (UINT)m_visibleObjects.size(), 0, 0, 0);
        }
    }

    // Переходим в PIXEL_SHADER_RESOURCE для lighting pass
    {
        D3D12_RESOURCE_BARRIER barriers[NumCascades]{};
        for (UINT c = 0; c < NumCascades; ++c)
        {
            barriers[c].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[c].Transition.pResource   = m_shadowMaps[c].Get();
            barriers[c].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barriers[c].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[c].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(NumCascades, barriers);
    }
    m_shadowMapsReadable = true;
}

