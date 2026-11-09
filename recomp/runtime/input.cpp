// Controller input. XInput pads map directly, since the guest uses the same button layout. The
// keyboard acts as player one's pad while the game window has focus, and works alongside a pad.
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "guest.h"

namespace
{
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
    constexpr uint32_t USERS = 4;
    constexpr uint32_t PRESS_FRAMES = 12;

    struct ScriptedPress { uint64_t frame; uint16_t buttons; };
    std::vector<ScriptedPress> gScript;
    std::atomic<bool> gScriptLoaded{ false };
    std::mutex gScriptMutex;

    struct PadCache { DWORD lastPoll = 0, lastMissing = 0; bool connected = false; XINPUT_STATE state{}; };
    PadCache gPads[USERS];
    std::mutex gPadMutex;
    std::atomic<uint32_t> gPacket{ 1 };

    void LoadScript()
    {
        std::lock_guard lock(gScriptMutex);
        if (gScriptLoaded.load()) return;
        char* list = nullptr;
        size_t length = 0;
        _dupenv_s(&list, &length, "NIER_AUTO_PRESS");   // "frame:buttons,frame:buttons"; buttons in hex
        if (list)
        {
            char* context = nullptr;
            for (char* token = strtok_s(list, ",", &context); token; token = strtok_s(nullptr, ",", &context))
            {
                char* separator = strchr(token, ':');
                if (separator) gScript.push_back({ strtoull(token, nullptr, 10), uint16_t(strtoul(separator + 1, nullptr, 16)) });
            }
            Log("[input] %zu scripted presses loaded\n", gScript.size());
        }
        gScriptLoaded = true;
    }

    uint16_t ScriptedButtons()
    {
        if (!gScriptLoaded.load()) LoadScript();
        uint64_t frame = GpuFrameCount();
        uint16_t buttons = 0;
        for (const auto& press : gScript)
            if (frame >= press.frame && frame < press.frame + PRESS_FRAMES) buttons |= press.buttons;
        return buttons;
    }

    // A polled empty slot is slow in XInput, so missing pads are only rechecked once a second.
    bool ReadPad(uint32_t user, XINPUT_STATE& out)
    {
        std::lock_guard lock(gPadMutex);
        PadCache& pad = gPads[user];
        DWORD now = GetTickCount();
        if (!pad.connected && pad.lastMissing && now - pad.lastMissing < 1000) return false;
        if (pad.connected && now == pad.lastPoll) { out = pad.state; return true; }
        DWORD result = XInputGetState(user, &pad.state);
        pad.lastPoll = now;
        bool wasConnected = pad.connected;
        pad.connected = result == ERROR_SUCCESS;
        if (pad.connected != wasConnected) Log("[input] controller %u %s\n", user, pad.connected ? "connected" : "disconnected");
        if (!pad.connected) { pad.lastMissing = now; return false; }
        out = pad.state;
        return true;
    }

    bool Key(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

    void ApplyKeyboard(uint16_t& buttons, uint8_t& leftTrigger, uint8_t& rightTrigger, int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry)
    {
        if (!PresenterHasFocus()) return;
        struct Binding { int key; uint16_t button; };
        static const Binding kBindings[] = {
            { VK_UP, 0x0001 }, { VK_DOWN, 0x0002 }, { VK_LEFT, 0x0004 }, { VK_RIGHT, 0x0008 },
            { VK_RETURN, 0x0010 }, { VK_TAB, 0x0020 }, { 'V', 0x0040 }, { 'B', 0x0080 },
            { 'Q', 0x0100 }, { 'F', 0x0200 }, { VK_SPACE, 0x1000 }, { VK_LSHIFT, 0x2000 },
            { 'E', 0x4000 }, { 'R', 0x8000 },
        };
        for (const auto& binding : kBindings) if (Key(binding.key)) buttons |= binding.button;
        if (Key('Z')) leftTrigger = 255;
        if (Key('C')) rightTrigger = 255;
        if (Key('A')) lx = -32767;
        if (Key('D')) lx = 32767;
        if (Key('W')) ly = 32767;
        if (Key('S')) ly = -32767;
        if (Key('J')) rx = -32767;
        if (Key('L')) rx = 32767;
        if (Key('I')) ry = 32767;
        if (Key('K')) ry = -32767;
    }
}

GUEST_IMPORT(XamInputGetCapabilities)
{
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= USERS || (user != 0 && !ReadPad(user, state))) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    W8(caps + 0, 1);              // gamepad
    W8(caps + 1, 1);
    W16(caps + 2, 0);
    W16(caps + 4, 0xFFFF);        // buttons
    W8(caps + 6, 255);
    W8(caps + 7, 255);
    W16(caps + 8, 0x7FFF);
    W16(caps + 10, 0x7FFF);
    W16(caps + 12, 0x7FFF);
    W16(caps + 14, 0x7FFF);
    W16(caps + 16, 0xFFFF);       // vibration motors
    W16(caps + 18, 0xFFFF);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputGetState)
{
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    if (user >= USERS) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

    XINPUT_STATE pad{};
    bool connected = ReadPad(user, pad);
    if (!connected && user != 0) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

    uint16_t buttons = pad.Gamepad.wButtons;
    uint8_t leftTrigger = pad.Gamepad.bLeftTrigger, rightTrigger = pad.Gamepad.bRightTrigger;
    int16_t lx = pad.Gamepad.sThumbLX, ly = pad.Gamepad.sThumbLY, rx = pad.Gamepad.sThumbRX, ry = pad.Gamepad.sThumbRY;
    if (user == 0)
    {
        ApplyKeyboard(buttons, leftTrigger, rightTrigger, lx, ly, rx, ry);
        buttons |= ScriptedButtons();
    }

    if (state)
    {
        W32(state, gPacket.fetch_add(1));
        W16(state + 4, buttons);
        W8(state + 6, leftTrigger);
        W8(state + 7, rightTrigger);
        W16(state + 8, uint16_t(lx));
        W16(state + 10, uint16_t(ly));
        W16(state + 12, uint16_t(rx));
        W16(state + 14, uint16_t(ry));
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputSetState)
{
    uint32_t user = ctx.r3.u32, vibration = ctx.r5.u32;
    XINPUT_STATE state;
    if (user >= USERS || !ReadPad(user, state))
    {
        ctx.r3.u64 = user == 0 ? 0 : X_ERROR_DEVICE_NOT_CONNECTED;
        return;
    }
    XINPUT_VIBRATION motors{ R16(vibration), R16(vibration + 2) };
    XInputSetState(user, &motors);
    ctx.r3.u64 = 0;
}
