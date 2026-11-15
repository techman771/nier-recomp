// Game window. Windows-specific; drawing belongs to the renderer backend.
#include <windows.h>
#include <atomic>
#include <thread>
#include "guest.h"
#include "renderer.h"
#include "settings.h"
#include "input.h"

namespace
{
    HWND gWindow = nullptr;
    bool gFullscreen = false;
    WINDOWPLACEMENT gWindowedPlacement{ sizeof(WINDOWPLACEMENT) };

    constexpr UINT WM_SET_FULLSCREEN = WM_APP + 1;   // wParam: on
    std::atomic<int> gMouseX{ 0 }, gMouseY{ 0 };

    // The mouse drives the camera while the game window is focused and Mouse Sensitivity isn't 0
    // (test runs never take focus): the cursor is hidden and kept inside the window.
    bool gSizeMove = false;   // the user is dragging or resizing the window: the cursor must stay free
    bool MouseCaptured() { return gWindow && !gSizeMove && GetForegroundWindow() == gWindow && Settings().mouseSensitivity > 0 && !InputPointerMode(); }
    std::atomic<int> gLeftClicks{ 0 }, gRightClicks{ 0 }, gWheel{ 0 };
    POINT gLastPointer{ -1, -1 };

    void UpdateMouseClip()
    {
        if (!MouseCaptured()) { ClipCursor(nullptr); return; }
        RECT client;
        GetClientRect(gWindow, &client);
        MapWindowPoints(gWindow, nullptr, reinterpret_cast<POINT*>(&client), 2);
        ClipCursor(&client);
    }

    // Borderless fullscreen on the monitor the window is on (no display mode change; the renderer
    // letterboxes to the game's aspect ratio). The PC Settings page's Display Mode chooses it (saved in
    // settings.ini; NIER_FULLSCREEN overrides); Alt+Enter or F11 toggles it and updates the setting.
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
        case WM_CHAR:
            OskChar(wchar_t(w));   // typing into the on-screen keyboard, when it is open
            return 0;
        case WM_KEYDOWN:
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) OskKey(int(w));
            if (w == VK_F12) GpuRequestCapture();   // capture the next frame for debugging
            if (w == VK_F9 && !(l & (1 << 30))) GpuRequestRecording();   // record 10 seconds (not on key repeat)
            if (w == VK_F11) { SetFullscreen(!gFullscreen); Settings().borderless = gFullscreen; SettingsSave(); }
            return DefWindowProcA(window, message, w, l);
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))   // Alt+Enter
            {
                SetFullscreen(!gFullscreen);
                Settings().borderless = gFullscreen;
                SettingsSave();
                return 0;
            }
            return DefWindowProcA(window, message, w, l);
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;   // no beep for Alt+Enter
            return DefWindowProcA(window, message, w, l);
        case WM_SETCURSOR:
            // No cursor over the game in fullscreen, or while the mouse drives the camera.
            // (A menu the mouse can point at shows it.)
            if (!InputPointerMode() && (gFullscreen || MouseCaptured()) && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            return DefWindowProcA(window, message, w, l);
        case WM_INPUT:
        {
            // Raw mouse movement (unaffected by pointer speed or the screen edge), for the camera.
            RAWINPUT input;
            UINT size = sizeof(input);
            if (MouseCaptured() && GetRawInputData(HRAWINPUT(l), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            {
                gMouseX += input.data.mouse.lLastX;
                gMouseY += input.data.mouse.lLastY;
            }
            return DefWindowProcA(window, message, w, l);
        }
        case WM_LBUTTONDOWN:
            if (OskActive())
            {
                RECT client;
                GetClientRect(window, &client);
                OskClick(short(LOWORD(l)), short(HIWORD(l)), client.right, client.bottom);
                return 0;
            }
            gLeftClicks++;
            return 0;
        case WM_RBUTTONDOWN: gRightClicks++; return 0;
        case WM_MOUSEWHEEL: gWheel += GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA; return 0;
        case WM_TIMER:
        {
            // Switching between the camera mouse (hidden, confined to the window) and the menu
            // pointer (shown, free; it starts in the middle, since the hidden cursor drifts to an edge).
            static bool wasCaptured = false, wasPointer = false;
            const bool captured = MouseCaptured(), pointer = InputPointerMode() && GetForegroundWindow() == gWindow;
            if (captured != wasCaptured) UpdateMouseClip();
            if (pointer && !wasPointer)
            {
                RECT client;
                GetClientRect(gWindow, &client);
                POINT centre{ client.right / 2, client.bottom / 2 };
                ClientToScreen(gWindow, &centre);
                SetCursorPos(centre.x, centre.y);
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
            }
            if (captured && !wasCaptured) SetCursor(nullptr);
            wasCaptured = captured;
            wasPointer = pointer;
            return 0;
        }
        case WM_ENTERSIZEMOVE: gSizeMove = true; ClipCursor(nullptr); return 0;
        case WM_EXITSIZEMOVE: gSizeMove = false; UpdateMouseClip(); return 0;
        case WM_NCLBUTTONDOWN: ClipCursor(nullptr); return DefWindowProcA(window, message, w, l);   // title bar and borders
        case WM_ACTIVATE:
        case WM_MOVE:
            UpdateMouseClip();
            return DefWindowProcA(window, message, w, l);
        case WM_SET_FULLSCREEN:
            SetFullscreen(w != 0);
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
        RAWINPUTDEVICE mouse{ 0x01, 0x02, 0, gWindow };   // generic desktop, mouse
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        SetTimer(gWindow, 1, 30, nullptr);   // mouse mode changes (WM_TIMER)
        char fullscreen[8];
        const bool testRun = GetEnvironmentVariableA("NIER_FULLSCREEN", fullscreen, sizeof(fullscreen)) && fullscreen[0] == '0';
        if (testRun) ShowWindow(gWindow, SW_SHOWNOACTIVATE);   // test runs (NIER_FULLSCREEN=0) don't take keyboard focus
        else
        {
            ShowWindow(gWindow, SW_SHOW);
            if (Settings().borderless) SetFullscreen(true);
            // A process started from the background may not take the foreground, and without focus
            // Mute in Background would keep it silent (a controller still works). Windows lets a
            // process that just received input take it, so press and release Alt first.
            INPUT alt[2] = {};
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(gWindow);
            if (GetForegroundWindow() != gWindow) Log("[present] couldn't take the foreground; click the window for keyboard and sound\n");
        }

        MSG message;
        while (GetMessageA(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
}

void PresenterSetFullscreen(bool on)
{
    if (gWindow) PostMessageA(gWindow, WM_SET_FULLSCREEN, on ? 1 : 0, 0);
}

// NIER_FAKE_MOUSE=<frame>,<x>,<y>[,<click frame>] stands in for the pointer in test runs (game
// coordinates; one left click at the click frame).
bool PresenterTakeMenuMouse(MenuMouse& mouse)
{
    static const struct Fake { uint64_t frame = ~0ull, click = ~0ull; float x = 0, y = 0; bool moved = false, clicked = false; } *fake = [] {
        static Fake f;
        char value[64];
        if (GetEnvironmentVariableA("NIER_FAKE_MOUSE", value, sizeof(value)))
        {
            unsigned long long frame = 0, click = ~0ull;
            if (sscanf(value, "%llu,%f,%f,%llu", &frame, &f.x, &f.y, &click) >= 3) { f.frame = frame; f.click = click; }
        }
        return &f;
    }();
    if (GpuFrameCount() >= fake->frame)
    {
        Fake* f = const_cast<Fake*>(fake);
        mouse.x = f->x; mouse.y = f->y;
        mouse.moved = !f->moved; f->moved = true;
        if (GpuFrameCount() >= f->click && !f->clicked) { f->clicked = true; mouse.leftClicks = 1; }
        return true;
    }
    mouse.leftClicks = gLeftClicks.exchange(0);
    mouse.rightClicks = gRightClicks.exchange(0);
    mouse.wheel = gWheel.exchange(0);
    if (!gWindow || GetForegroundWindow() != gWindow) return false;
    POINT point;
    RECT client;
    if (!GetCursorPos(&point) || !ScreenToClient(gWindow, &point) || !GetClientRect(gWindow, &client) || client.right <= 0 || client.bottom <= 0) return false;
    mouse.moved = point.x != gLastPointer.x || point.y != gLastPointer.y;
    gLastPointer = point;
    // The picture is letterboxed to 16:9 inside the window.
    float width = float(client.right), height = float(client.bottom), w = width, h = height;
    if (width / height > 16.0f / 9.0f) w = h * 16.0f / 9.0f; else h = w * 9.0f / 16.0f;
    mouse.x = (float(point.x) - (width - w) * 0.5f) * 1280.0f / w;
    mouse.y = (float(point.y) - (height - h) * 0.5f) * 720.0f / h;
    return mouse.x >= 0 && mouse.x < 1280 && mouse.y >= 0 && mouse.y < 720;
}

void PresenterTakeMouseMotion(int& dx, int& dy)
{
    dx = gMouseX.exchange(0);
    dy = gMouseY.exchange(0);
}

bool PresenterHasFocus() { return gWindow && GetForegroundWindow() == gWindow; }
void PresenterDumpStats() { RendererDumpStats(); }
void PresenterInit() { std::thread(WindowThread).detach(); }
