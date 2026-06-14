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
        1200,
        1000,
        L"DX12WindowClass",
        L"KG_Laba4 - Deferred Rendering"))
    {
        MessageBoxW(nullptr, L"Window Create FAILED", L"Error", MB_OK);
        return false;
    }

    m_window.SetInputDevice(&m_input);

    if (!m_renderingSystem.Initialize(m_window.GetHWND(), 1200, 1000))
    {
        MessageBoxW(nullptr, L"RenderingSystem Initialize FAILED (see Output window)", L"Error", MB_OK);
        return false;
    }

    m_renderingSystem.SetTechnique(RenderingSystem::Technique::Deferred);

    m_timer.Reset();
    return true;
}

int Application::Run()
{
    MSG msg{};
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

        m_timer.Tick();
        float deltaTime = m_timer.DeltaTime();

        bool orbitRotate = m_input.IsMouseButtonDown(VK_RBUTTON);
        bool dolly       = m_input.IsMouseButtonDown(VK_LBUTTON);

        int mouseDeltaX = 0, mouseDeltaY = 0;
        if (orbitRotate || dolly)
            m_input.GetMouseDelta(mouseDeltaX, mouseDeltaY);
        m_input.ResetMouseDelta();

        m_renderingSystem.UpdateCameraOrbit(
            deltaTime,
            0.005f,
            0.2f,
            orbitRotate,
            dolly,
            (float)mouseDeltaX,
            (float)mouseDeltaY);

        m_renderingSystem.SetTime(m_timer.TotalTime());
        m_renderingSystem.RenderFrame();
    }
}
