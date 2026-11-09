// Game window. Windows-specific; drawing belongs to the renderer backend.
#include <windows.h>
#include <thread>
#include "guest.h"
#include "renderer.h"

namespace
{
    HWND gWindow = nullptr;

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) RendererResize(LOWORD(l), HIWORD(l));
            return 0;
        case WM_CLOSE:
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0);
            return 0;
        default:
            return DefWindowProcA(window, message, w, l);
        }
    }

    void WindowThread()
    {
        WNDCLASSA windowClass{};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "NierRecompWindow";
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassA(&windowClass);

        RECT rect{ 0, 0, 1280, 720 };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        gWindow = CreateWindowA("NierRecompWindow", "NieR (recompiled)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
        RendererInit(gWindow, 1280, 720);
        ShowWindow(gWindow, SW_SHOWNOACTIVATE);  // so test runs don't take keyboard focus

        MSG message;
        while (GetMessageA(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
}

bool PresenterHasFocus() { return gWindow && GetForegroundWindow() == gWindow; }
void PresenterDumpStats() { RendererDumpStats(); }
void PresenterInit() { std::thread(WindowThread).detach(); }
