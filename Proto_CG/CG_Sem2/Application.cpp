#include "Application.h"
#include <windows.h>

Application::Application(HINSTANCE hInstance, int nCmdShow)
    : m_hInstance(hInstance), m_nCmdShow(nCmdShow)
{
}

bool Application::Initialize()
{
    if (!m_window.Create(
        m_hInstance,
        m_nCmdShow,
        800,
        600,
        L"DX12WindowClass",
        L"KG_Laba4 - DX12 Final"))
    {
        MessageBoxW(nullptr, L"Window Create FAILED", L"Error", MB_OK);
        return false;
    }

    m_window.SetInputDevice(&m_input);

    if (!m_renderingSystem.Initialize(m_window.GetHWND(), 1200, 1000))
    {
        MessageBoxW(nullptr, L"DX12 Initialize FAILED (see Output window)", L"Error", MB_OK);
        return false;
    }
    
    m_renderingSystem.SetTechnique(RenderingSystem::Technique::Deferred);
    m_renderingSystem.SetUVTiling(1.0f, 1.0f);
    m_renderingSystem.SetUVScrollSpeed(0.0f, 0.0f);
    m_renderingSystem.SetClearColor(0.48f, 0.52f, 0.80f, 1.0f);

    m_timer.Reset();
    return true;
}

int Application::Run()
{
    MSG msg{};
    bool fPrev = false, oPrev = false;

    while (true)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return (int)msg.wParam;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            PostQuitMessage(0);
            continue;
        }

        // Переключение отсечения: F = фрустум-отсечение, O = режим октодерева
        bool fNow = (GetAsyncKeyState('F') & 0x8000) != 0;
        bool oNow = (GetAsyncKeyState('O') & 0x8000) != 0;
        if (fNow && !fPrev) m_renderingSystem.ToggleFrustumCulling();
        if (oNow && !oPrev) m_renderingSystem.ToggleOctree();
        fPrev = fNow;
        oPrev = oNow;

        m_timer.Tick();
        float deltaTime = m_timer.DeltaTime();

        bool orbitRotate = m_input.IsMouseButtonDown(VK_RBUTTON);
        bool dolly = m_input.IsMouseButtonDown(VK_LBUTTON);

        int mouseDeltaX = 0, mouseDeltaY = 0;
        if (orbitRotate || dolly)
        {
            m_input.GetMouseDelta(mouseDeltaX, mouseDeltaY);
        }
        m_input.ResetMouseDelta();

        // скорость для
        const float rotateSpeed = 0.0035f;
        const float dollySpeed  = 1.5f;

        m_renderingSystem.UpdateCameraOrbit(
            deltaTime,
            rotateSpeed,
            dollySpeed,
            orbitRotate,
            dolly,
            (float)mouseDeltaX,
            (float)mouseDeltaY);

        m_renderingSystem.SetTime(m_timer.TotalTime());
        m_renderingSystem.RenderFrame();

        // Показываем статистику отсечения в заголовке окна
        wchar_t titleBuf[256];
        swprintf_s(titleBuf, 256,
            L"KG_Lab4  |  Objects: %u / %u  |  Culling[F]: %s  |  Octree[O]: %s",
            m_renderingSystem.GetVisibleCount(),
            m_renderingSystem.GetTotalCount(),
            m_renderingSystem.IsFrustumCullingEnabled() ? L"ON" : L"OFF",
            m_renderingSystem.IsOctreeEnabled()         ? L"ON" : L"OFF");
        SetWindowTextW(m_window.GetHWND(), titleBuf);
    }
}
