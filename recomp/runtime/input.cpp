// Controller input, through SDL3: Xbox pads, DualShock 4, DualSense, Switch Pro and anything else
// SDL knows, over USB or Bluetooth, with rumble, mapped onto the Xbox 360 layout the guest expects.
// The game is single-player, so every connected controller (up to four) drives player one, merged,
// and the guest sees no other players. The keyboard also acts as player one's pad while the game
// window has focus.
//
// SDL runs on its own thread (it owns the device list and pumps device events); guest threads read
// the latest state under SDL's own joystick lock. The kind of device used last (Xbox, PlayStation,
// Nintendo, keyboard) is tracked for the button glyphs.
#include <windows.h>
#define SDL_MAIN_HANDLED   // the program has its own main()
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "guest.h"
#include "input.h"

namespace
{
    constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
    constexpr uint32_t USERS = 4;
    constexpr uint32_t PRESS_FRAMES = 12;

    struct ScriptedPress { uint64_t frame; uint16_t buttons; };
    std::vector<ScriptedPress> gScript;
    std::atomic<bool> gScriptLoaded{ false };
    std::mutex gScriptMutex;
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

    // ---- controllers ----------------------------------------------------------------------------
    struct Slot
    {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        InputStyle style = InputStyle::Xbox;
        uint16_t rumbleLow = 0, rumbleHigh = 0;
        uint64_t rumbleSent = 0;
    };
    Slot gSlots[USERS];
    std::mutex gSlotMutex;            // the slot table; SDL_LockJoysticks covers the devices themselves
    std::atomic<bool> gSdlReady{ false };
    std::atomic<int> gLastStyle{ int(InputStyle::Xbox) };

    InputStyle StyleOf(SDL_GamepadType type)
    {
        switch (type)
        {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: return InputStyle::PS4;
        case SDL_GAMEPAD_TYPE_PS5: return InputStyle::PS5;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return InputStyle::Nintendo;
        default: return InputStyle::Xbox;
        }
    }

    const char* StyleName(InputStyle style)
    {
        switch (style)
        {
        case InputStyle::PS4: return "DualShock 4";
        case InputStyle::PS5: return "DualSense";
        case InputStyle::Nintendo: return "Nintendo";
        case InputStyle::Keyboard: return "keyboard";
        default: return "Xbox";
        }
    }

    void SetLastStyle(InputStyle style)
    {
        if (gLastStyle.exchange(int(style)) != int(style)) Log("[input] glyph style: %s\n", StyleName(style));
    }

    void AddPad(SDL_JoystickID id)
    {
        std::lock_guard lock(gSlotMutex);
        for (const Slot& slot : gSlots) if (slot.pad && slot.id == id) return;
        for (uint32_t user = 0; user < USERS; user++)
        {
            Slot& slot = gSlots[user];
            if (slot.pad) continue;
            slot.pad = SDL_OpenGamepad(id);
            if (!slot.pad) { Log("[input] couldn't open controller: %s\n", SDL_GetError()); return; }
            slot.id = id;
            slot.style = StyleOf(SDL_GetGamepadType(slot.pad));
            slot.rumbleLow = slot.rumbleHigh = 0;
            gLastStyle = int(slot.style);   // show its glyphs straight away
            Log("[input] controller %u connected: %s (%s layout)\n", user, SDL_GetGamepadName(slot.pad), StyleName(slot.style));
            return;
        }
        Log("[input] a fifth controller was ignored\n");
    }

    void RemovePad(SDL_JoystickID id)
    {
        std::lock_guard lock(gSlotMutex);
        for (uint32_t user = 0; user < USERS; user++)
        {
            Slot& slot = gSlots[user];
            if (!slot.pad || slot.id != id) continue;
            SDL_CloseGamepad(slot.pad);
            slot = Slot{};
            Log("[input] controller %u disconnected\n", user);
        }
    }

    // The Xbox 360 keeps a rumble level until it is changed; SDL's has a duration, so it is renewed.
    void RenewRumble()
    {
        std::lock_guard lock(gSlotMutex);
        const uint64_t now = SDL_GetTicks();
        for (Slot& slot : gSlots)
            if (slot.pad && (slot.rumbleLow || slot.rumbleHigh) && now - slot.rumbleSent > 700)
            {
                SDL_RumbleGamepad(slot.pad, slot.rumbleLow, slot.rumbleHigh, 1500);
                slot.rumbleSent = now;
            }
    }

    void SdlThread()
    {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // SDL has no window of its own here
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");                    // device messages don't depend on our message loop
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");          // DualShock 4 / DualSense rumble over Bluetooth
        if (!SDL_Init(SDL_INIT_GAMEPAD))
        {
            Log("[input] SDL couldn't start (%s); keyboard only\n", SDL_GetError());
            return;
        }
        gSdlReady = true;
        Log("[input] SDL %d.%d.%d controller support ready\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
        for (;;)
        {
            SDL_Event event;
            while (SDL_WaitEventTimeout(&event, 2))   // also refreshes every controller's state
            {
                if (event.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(event.gdevice.which);
                else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(event.gdevice.which);
            }
            RenewRumble();
        }
    }

    void EnsureSdl()
    {
        static std::once_flag once;
        std::call_once(once, [] { std::thread(SdlThread).detach(); });
    }

    struct PadState { uint16_t buttons = 0; uint8_t leftTrigger = 0, rightTrigger = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

    int16_t Flip(int16_t value) { return value == INT16_MIN ? INT16_MAX : int16_t(-value); }   // SDL's Y axes point down

    // Reads slot `user` in the Xbox 360 layout. False when no controller is in that slot.
    bool ReadPad(uint32_t user, PadState& out, InputStyle& style)
    {
        if (!gSdlReady) return false;
        std::lock_guard lock(gSlotMutex);
        SDL_Gamepad* pad = gSlots[user].pad;
        if (!pad) return false;
        style = gSlots[user].style;
        struct Map { SDL_GamepadButton button; uint16_t bit; };
        static const Map kButtons[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001 }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002 }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004 },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008 }, { SDL_GAMEPAD_BUTTON_START, 0x0010 }, { SDL_GAMEPAD_BUTTON_BACK, 0x0020 },
            { SDL_GAMEPAD_BUTTON_TOUCHPAD, 0x0020 },   // the touchpad click doubles as Back, as PlayStation ports usually do
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040 }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080 },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100 }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200 },
            { SDL_GAMEPAD_BUTTON_SOUTH, 0x1000 }, { SDL_GAMEPAD_BUTTON_EAST, 0x2000 }, { SDL_GAMEPAD_BUTTON_WEST, 0x4000 }, { SDL_GAMEPAD_BUTTON_NORTH, 0x8000 },
        };
        SDL_LockJoysticks();
        for (const Map& map : kButtons) if (SDL_GetGamepadButton(pad, map.button)) out.buttons |= map.bit;
        out.leftTrigger = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
        out.rightTrigger = uint8_t(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
        out.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        out.ly = Flip(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        out.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        out.ry = Flip(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        SDL_UnlockJoysticks();
        return true;
    }

    bool Active(const PadState& s)
    {
        constexpr int kStick = 12000;
        return s.buttons || s.leftTrigger > 40 || s.rightTrigger > 40 || abs(s.lx) > kStick || abs(s.ly) > kStick || abs(s.rx) > kStick || abs(s.ry) > kStick;
    }

    bool Key(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

    // Returns true when a bound key is down.
    bool ApplyKeyboard(PadState& s)
    {
        if (!PresenterHasFocus()) return false;
        bool any = false;
        struct Binding { int key; uint16_t button; };
        static const Binding kBindings[] = {
            { VK_UP, 0x0001 }, { VK_DOWN, 0x0002 }, { VK_LEFT, 0x0004 }, { VK_RIGHT, 0x0008 },
            { VK_RETURN, 0x0010 }, { VK_TAB, 0x0020 }, { 'V', 0x0040 }, { 'B', 0x0080 },
            { 'Q', 0x0100 }, { 'F', 0x0200 }, { VK_SPACE, 0x1000 }, { VK_LSHIFT, 0x2000 },
            { 'E', 0x4000 }, { 'R', 0x8000 },
        };
        for (const auto& binding : kBindings) if (Key(binding.key)) { s.buttons |= binding.button; any = true; }
        if (Key('Z')) { s.leftTrigger = 255; any = true; }
        if (Key('C')) { s.rightTrigger = 255; any = true; }
        if (Key('A')) { s.lx = -32767; any = true; }
        if (Key('D')) { s.lx = 32767; any = true; }
        if (Key('W')) { s.ly = 32767; any = true; }
        if (Key('S')) { s.ly = -32767; any = true; }
        if (Key('J')) { s.rx = -32767; any = true; }
        if (Key('L')) { s.rx = 32767; any = true; }
        if (Key('I')) { s.ry = 32767; any = true; }
        if (Key('K')) { s.ry = -32767; any = true; }
        return any;
    }
}

InputStyle InputLastStyle() { return InputStyle(gLastStyle.load()); }

GUEST_IMPORT(XamInputGetCapabilities)
{
    EnsureSdl();
    uint32_t user = ctx.r3.u32, caps = ctx.r5.u32;
    if (user != 0) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
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
    EnsureSdl();
    uint32_t user = ctx.r3.u32, state = ctx.r5.u32;
    if (user >= USERS) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }

    if (user != 0) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }   // every controller drives player one

    PadState pad;
    for (uint32_t slot = 0; slot < USERS; slot++)
    {
        PadState one;
        InputStyle style = InputStyle::Xbox;
        if (!ReadPad(slot, one, style)) continue;
        if (Active(one)) SetLastStyle(style);
        pad.buttons |= one.buttons;
        pad.leftTrigger = (std::max)(pad.leftTrigger, one.leftTrigger);
        pad.rightTrigger = (std::max)(pad.rightTrigger, one.rightTrigger);
        if (abs(one.lx) > abs(pad.lx)) pad.lx = one.lx;
        if (abs(one.ly) > abs(pad.ly)) pad.ly = one.ly;
        if (abs(one.rx) > abs(pad.rx)) pad.rx = one.rx;
        if (abs(one.ry) > abs(pad.ry)) pad.ry = one.ry;
    }
    if (ApplyKeyboard(pad)) SetLastStyle(InputStyle::Keyboard);
    pad.buttons |= ScriptedButtons();

    if (state)
    {
        W32(state, gPacket.fetch_add(1));
        W16(state + 4, pad.buttons);
        W8(state + 6, pad.leftTrigger);
        W8(state + 7, pad.rightTrigger);
        W16(state + 8, uint16_t(pad.lx));
        W16(state + 10, uint16_t(pad.ly));
        W16(state + 12, uint16_t(pad.rx));
        W16(state + 14, uint16_t(pad.ry));
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamInputSetState)
{
    EnsureSdl();
    uint32_t user = ctx.r3.u32, vibration = ctx.r5.u32;
    if (user != 0) { ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED; return; }
    std::lock_guard lock(gSlotMutex);
    for (Slot& slot : gSlots)   // player one's rumble goes to every controller
    {
        if (!slot.pad) continue;
        slot.rumbleLow = R16(vibration);        // left motor: low frequency
        slot.rumbleHigh = R16(vibration + 2);   // right motor: high frequency
        SDL_RumbleGamepad(slot.pad, slot.rumbleLow, slot.rumbleHigh, 1500);
        slot.rumbleSent = SDL_GetTicks();
    }
    ctx.r3.u64 = 0;
}
