// Game window. Windows-specific; drawing belongs to the renderer backend.
#include <windows.h>
#include <thread>
#include "guest.h"
#include "renderer.h"

namespace
{
    HWND gWindow = nullptr;
    bool gFullscreen = false;
    WINDOWPLACEMENT gWindowedPlacement{ sizeof(WINDOWPLACEMENT) };

    // Borderless fullscreen on the monitor the window is on (no display mode change; the renderer
    // letterboxes to the game's aspect ratio). Alt+Enter or F11 toggles it; NIER_FULLSCREEN=1 starts in it.
    void SetFullscreen(bool on)
    {
        if (on == gFullscreen) return;
        gFullscreen = on;
        if (on)
        {
            GetWindowPlacement(gWindow, &gWindowedPlacement);
            MONITORINFO info{ sizeof(info) };
            GetMonitorInfoA(MonitorFromWindow(gWindow, MONITOR_DEFAULTTOPRIMARY), &info);
            SetWindowLongA(gWindow, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(gWindow, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
        else
        {
            SetWindowLongA(gWindow, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPlacement(gWindow, &gWindowedPlacement);
            SetWindowPos(gWindow, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        switch (message)
        {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) RendererResize(LOWORD(l), HIWORD(l));
            return 0;
        case WM_KEYDOWN:
            if (w == VK_F12) GpuRequestCapture();   // capture the next frame for debugging
            if (w == VK_F11) SetFullscreen(!gFullscreen);
            return DefWindowProcA(window, message, w, l);
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30))) { SetFullscreen(!gFullscreen); return 0; }   // Alt+Enter
            return DefWindowProcA(window, message, w, l);
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;   // no beep for Alt+Enter
            return DefWindowProcA(window, message, w, l);
        case WM_SETCURSOR:
            if (gFullscreen && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }   // no cursor over the game in fullscreen
            return DefWindowProcA(window, message, w, l);
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
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        // NIER_MONITOR centres the window on another monitor: "secondary" picks the first monitor that
        // isn't the primary one, a number picks the n-th in the system's enumeration order (1-based).
        // The test script sets "secondary" so runs stay off the main screen.
        char monitorSetting[16];
        if (GetEnvironmentVariableA("NIER_MONITOR", monitorSetting, sizeof(monitorSetting)))
        {
            struct Search { int wanted, seen = 0; RECT area{}; bool found = false; } search{ _stricmp(monitorSetting, "secondary") == 0 ? 0 : atoi(monitorSetting) };
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                auto* s = reinterpret_cast<Search*>(data);
                MONITORINFO info{ sizeof(info) };
                GetMonitorInfoA(monitor, &info);
                ++s->seen;
                const bool match = s->wanted == 0 ? !(info.dwFlags & MONITORINFOF_PRIMARY) : s->seen == s->wanted;
                if (!match) return TRUE;
                s->area = info.rcWork;
                s->found = true;
                return FALSE;
            }, LPARAM(&search));
            if (search.found)
            {
                const int spareX = int(search.area.right - search.area.left) - width, spareY = int(search.area.bottom - search.area.top) - height;
                x = search.area.left + (spareX > 0 ? spareX / 2 : 0);
                y = search.area.top + (spareY > 0 ? spareY / 2 : 0);
            }
            else Log("[present] NIER_MONITOR=%s: only %d monitor(s); using the default position\n", monitorSetting, search.seen);
        }
        gWindow = CreateWindowA("NierRecompWindow", "NieR (recompiled)", WS_OVERLAPPEDWINDOW, x, y, width, height,
            nullptr, nullptr, windowClass.hInstance, nullptr);
        RendererInit(gWindow, 1280, 720);
        char fullscreen[8];
        if (GetEnvironmentVariableA("NIER_FULLSCREEN", fullscreen, sizeof(fullscreen)) && fullscreen[0] == '1')
        {
            ShowWindow(gWindow, SW_SHOW);
            SetFullscreen(true);
            SetForegroundWindow(gWindow);
        }
        else ShowWindow(gWindow, SW_SHOWNOACTIVATE);  // so test runs don't take keyboard focus

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
