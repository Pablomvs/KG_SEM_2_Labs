#pragma once

#include <windows.h>

#include <DirectXMath.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <memory>
#include <vector>

#include "D3D12Context.h"
#include "GBuffer.h"
#include "Octree.h"

class RenderingSystem
{
public:
    enum class Technique
    {
        Forward,
        Deferred
    };

    bool Initialize(HWND hwnd, UINT width, UINT height);
    void Shutdown();

    void SetTechnique(Technique technique);
    Technique GetTechnique() const;

    void SetClearColor(float r, float g, float b, float a);
    void SetTime(float timeSeconds);
    void SetUVTiling(float x, float y);
    void SetUVScrollSpeed(float uSpeed, float vSpeed);

    void UpdateCameraOrbit(
        float deltaTime,
        float rotateSpeed,
        float dollySpeed,
        bool orbitRotate,
        bool dolly,
        float mouseDeltaX,
        float mouseDeltaY);

    void RenderFrame();

    // --- Управление отсечением ---
    void ToggleFrustumCulling() { m_frustumCullingEnabled = !m_frustumCullingEnabled; }
    void ToggleOctree()         { m_octreeEnabled = !m_octreeEnabled; }
    bool IsFrustumCullingEnabled() const { return m_frustumCullingEnabled; }
    bool IsOctreeEnabled()         const { return m_octreeEnabled; }
    UINT GetVisibleCount()         const { return (UINT)m_visibleObjects.size(); }
    UINT GetTotalCount()           const { return (UINT)m_allObjects.size(); }

private:
    void RenderForwardFrame();
    void RenderDeferredFrame();
    void RenderOpaqueStage();
    void RenderLightingStage();
    void RenderGBufferDebugOverlay();
    void RenderTransparentStage();
    bool InitializeDeferredResources();
    bool CompileDeferredShaders();
    bool CreateDeferredLightingRootSignature();
    bool CreateDeferredGeometryPipeline();
    bool CreateDeferredLightingPipeline();
    bool CreateDebugOverlayRootSignature();
    bool CreateDebugOverlayPipeline();
    bool CreateLightingConstantBuffer();
    void UpdateLightingConstants();

    // --- Инстансинг и отсечение ---
    bool InitializeInstancedObjects();
    bool CompileInstancedShader();
    bool CreateInstancedRootSignature();
    bool CreateInstancedPipeline();
    bool CreateInstancedGeometry();
    bool CreateInstanceBuffer();
    bool CreateInstancedCB();
    void CullAndUpdateInstances();
    void RenderInstances(ID3D12GraphicsCommandList* commandList);

    // --- Каскадные карты теней (CSM) ---
    bool InitializeShadowResources();
    bool CompileShadowShaders();
    bool CreateShadowRootSignature();
    bool CreateShadowPipelines();
    void ComputeCascades();
    void RenderShadowPass();

private:
    static constexpr UINT MaxPointLights = 6;
    static constexpr UINT MaxSpotLights  = 4;
    static constexpr UINT ObjectCount    = 2000;
    static constexpr UINT NumCascades    = 3;
    static constexpr UINT ShadowMapSize  = 2048;
    static constexpr float ObjectScale   = 8.0f;
    static constexpr float ObjectRadius  = ObjectScale * 2.0f; // консервативный радиус сферы

    struct DeferredLightCB
    {
        DirectX::XMFLOAT4 LightDirection;
        DirectX::XMFLOAT4 LightColor;
        DirectX::XMFLOAT4 AmbientColor;
        DirectX::XMFLOAT4 LightCounts;
        DirectX::XMFLOAT4 PointLightPositionRange[MaxPointLights];
        DirectX::XMFLOAT4 PointLightColorIntensity[MaxPointLights];
        DirectX::XMFLOAT4 SpotLightPositionRange[MaxSpotLights];
        DirectX::XMFLOAT4 SpotLightDirectionCosine[MaxSpotLights];
        DirectX::XMFLOAT4 SpotLightColorIntensity[MaxSpotLights];
        DirectX::XMFLOAT4 ScreenSize;
        DirectX::XMFLOAT4X4 InvView;
        DirectX::XMFLOAT4X4 InvProj;
    };

    // Данные одного инстанса (поток 1, PER_INSTANCE_DATA)
    struct InstanceData
    {
        DirectX::XMFLOAT3 WorldPos;
        float              Scale;
        float              Yaw;   // случайный поворот вокруг Y
    };

    // Вершина куба (поток 0, PER_VERTEX_DATA)
    struct InstanceVertex
    {
        DirectX::XMFLOAT3 Pos;
        DirectX::XMFLOAT3 Norm;
    };

    // Константный буфер для шейдера инстансинга
    struct PerFrameInstancedCB
    {
        DirectX::XMFLOAT4X4 View;
        DirectX::XMFLOAT4X4 Proj;
    };

    // Константный буфер каскадных теней (для lighting pass)
    struct CascadeShadowCB
    {
        DirectX::XMFLOAT4X4 LightViewProj[NumCascades]; // транспонированные
        DirectX::XMFLOAT4   CascadeSplits;              // view-space Z границы каскадов
    };

private:
    D3D12Context m_context;
    GBuffer      m_gbuffer;
    Technique    m_technique   = Technique::Forward;
    float        m_clearColor[4] = { 0.48f, 0.52f, 0.80f, 1.0f };
    UINT         m_width  = 0;
    UINT         m_height = 0;

    // Шейдеры и PSO для отложенного рендеринга
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryHS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryDS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredGeometryPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredLightingVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_deferredLightingPS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayVS;
    Microsoft::WRL::ComPtr<ID3DBlob> m_debugOverlayPS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_deferredLightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_debugOverlayRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredGeometryPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_deferredLightingPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugOverlayPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_deferredLightConstantBuffer;
    UINT8*                                       m_deferredLightCBMappedData = nullptr;

    // Инстансинг
    Microsoft::WRL::ComPtr<ID3DBlob>            m_instancedVS;
    Microsoft::WRL::ComPtr<ID3DBlob>            m_instancedPS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_instancedRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_instancedPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_cubeVB;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_cubeIB;
    D3D12_VERTEX_BUFFER_VIEW                    m_cubeVBView{};
    D3D12_INDEX_BUFFER_VIEW                     m_cubeIBView{};
    UINT                                         m_cubeIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_instanceBuffer;
    D3D12_VERTEX_BUFFER_VIEW                    m_instanceVBView{};
    UINT8*                                       m_instanceMappedData = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_instancedFrameCB;
    UINT8*                                       m_instancedFrameCBMapped = nullptr;

    // Данные объектов сцены
    std::vector<InstanceData> m_allObjects;
    std::vector<InstanceData> m_visibleObjects;

    // Состояние отсечения
    bool m_frustumCullingEnabled = true;
    bool m_octreeEnabled         = false;

    std::unique_ptr<Octree> m_octree;

    // --- Shadow map resources ---
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_shadowMaps[NumCascades];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_lightingSRVHeap;   // GBuffer(0-3) + shadow(4-6)
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_shadowPassCB;       // per-cascade LightViewProj
    UINT8*                                        m_shadowPassCBMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_cascadeShadowCB;    // all cascades for lighting
    UINT8*                                        m_cascadeShadowCBMapped = nullptr;

    Microsoft::WRL::ComPtr<ID3DBlob>             m_shadowTerrainVS;
    Microsoft::WRL::ComPtr<ID3DBlob>             m_shadowInstancedVS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_shadowRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_shadowTerrainPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_shadowInstancedPSO;

    CascadeShadowCB m_cascadeData{};
    UINT            m_lightingSRVDescSize = 0;
    bool            m_shadowMapsReadable  = false;
};
