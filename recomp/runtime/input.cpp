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
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include "guest.h"
#include "input.h"
#include "settings.h"

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
        case InputStyle::Steam: return "Steam";
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
            // Valve hardware (Steam Deck, Steam Controller), whatever layout SDL reports for it.
            if (SDL_GetGamepadVendor(slot.pad) == 0x28DE || SDL_GetGamepadType(slot.pad) == SDL_GAMEPAD_TYPE_STEAM) slot.style = InputStyle::Steam;
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

    // Keyboard bindings: each pad control has one key (rebindable from PC Settings > Key Bindings,
    // saved in settings.ini as key_<id>). Order matters: 0-11 are the buttons page, 12-23 the
    // movement page.
    enum Kind { Button, LeftTrigger, RightTrigger, LxMinus, LxPlus, LyPlus, LyMinus, RxMinus, RxPlus, RyPlus, RyMinus };
    struct Action { KeyAction info; Kind kind; uint16_t bit; };
    const Action kActions[kKeyActions] = {
        { { "a", "Jump / Confirm (A)", VK_SPACE }, Button, 0x1000 }, { { "b", "Talk / Cancel (B)", VK_LSHIFT }, Button, 0x2000 },
        { { "x", "Attack (X)", VK_LBUTTON }, Button, 0x4000 }, { { "y", "Heavy Attack (Y)", VK_RBUTTON }, Button, 0x8000 },
        { { "lb", "Ability Slot (LB)", 'Q' }, Button, 0x0100 }, { { "rb", "Ability Slot (RB)", 'F' }, Button, 0x0200 },
        { { "lt", "Ability Slot (LT)", 'Z' }, LeftTrigger, 0 }, { { "rt", "Ability Slot (RT)", 'C' }, RightTrigger, 0 },
        { { "start", "Pause Menu (Start)", VK_ESCAPE }, Button, 0x0010 }, { { "back", "Map (Back)", VK_TAB }, Button, 0x0020 },
        { { "l3", "Left Stick Click (L3)", 0 }, Button, 0x0040 }, { { "r3", "Reset Camera (R3)", VK_MBUTTON }, Button, 0x0080 },
        { { "move_up", "Move Up", 'W' }, LyPlus, 0 }, { { "move_down", "Move Down", 'S' }, LyMinus, 0 },
        { { "move_left", "Move Left", 'A' }, LxMinus, 0 }, { { "move_right", "Move Right", 'D' }, LxPlus, 0 },
        { { "camera_up", "Camera Up", 'I' }, RyPlus, 0 }, { { "camera_down", "Camera Down", 'K' }, RyMinus, 0 },
        { { "camera_left", "Camera Left", 'J' }, RxMinus, 0 }, { { "camera_right", "Camera Right", 'L' }, RxPlus, 0 },
        { { "dpad_up", "Item Menu (D-Pad Up)", VK_UP }, Button, 0x0001 }, { { "dpad_down", "1H Sword Menu (D-Pad Down)", VK_DOWN }, Button, 0x0002 },
        { { "dpad_left", "2H Sword Menu (D-Pad Left)", VK_LEFT }, Button, 0x0004 }, { { "dpad_right", "Spear Menu (D-Pad Right)", VK_RIGHT }, Button, 0x0008 },
    };
    std::atomic<int> gKeys[kKeyActions];
    std::atomic<bool> gKeysReady{ false }, gCapture{ false }, gCaptureArmed{ false };

    void EnsureKeys()
    {
        if (gKeysReady.exchange(true)) return;
        for (int i = 0; i < kKeyActions; i++) if (!gKeys[i].load()) gKeys[i] = kActions[i].info.defaultKey;
    }

    std::atomic<uint64_t> gPointerUntil{ 0 }, gInjectUntil{ 0 };
    std::atomic<uint16_t> gInjected{ 0 };
    bool IsMouseButton(int key) { return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2; }

    // Returns true when a bound key is down.
    bool ApplyKeyboard(PadState& s)
    {
        if (!PresenterHasFocus() || gCapture.load()) return false;   // a key being captured for a binding isn't input
        EnsureKeys();
        bool any = false;
        for (int i = 0; i < kKeyActions; i++)
        {
            const int key = gKeys[i].load();
            if (!key || !Key(key)) continue;
            if (IsMouseButton(key) && InputPointerMode()) continue;   // the mouse is a menu pointer right now
            any = true;
            switch (kActions[i].kind)
            {
            case Button: s.buttons |= kActions[i].bit; break;
            case LeftTrigger: s.leftTrigger = 255; break;
            case RightTrigger: s.rightTrigger = 255; break;
            case LxMinus: s.lx = -32767; break;
            case LxPlus: s.lx = 32767; break;
            case LyPlus: s.ly = 32767; break;
            case LyMinus: s.ly = -32767; break;
            case RxMinus: s.rx = -32767; break;
            case RxPlus: s.rx = 32767; break;
            case RyPlus: s.ry = 32767; break;
            case RyMinus: s.ry = -32767; break;
            }
        }
        return any;
    }
}

InputStyle InputLastStyle() { return InputStyle(gLastStyle.load()); }

void InputKeepPointerMode() { gPointerUntil = GetTickCount64() + 150; }
bool InputPointerMode() { return GetTickCount64() < gPointerUntil.load(); }
void InputInject(uint16_t buttons) { gInjected = buttons; gInjectUntil = GetTickCount64() + 60; gLastStyle = int(InputStyle::Keyboard); }

// Mouse camera: the game only knows a right stick, so mouse speed becomes stick deflection. The
// movement since the last sample (counts per second, scaled by Mouse Sensitivity) is held for a few
// milliseconds, because the game may read the pad several times a frame; a small offset lifts
// slow movement over the game's stick dead zone. Returns true while the mouse is turning the camera.
static bool ApplyMouseCamera(PadState& pad)
{
    static std::mutex mutex;
    static LARGE_INTEGER frequency, last;
    static int16_t stickX = 0, stickY = 0;
    const int sensitivity = Settings().mouseSensitivity;
    if (sensitivity <= 0 || gCapture.load() || InputPointerMode()) return false;
    std::lock_guard lock(mutex);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!frequency.QuadPart) { QueryPerformanceFrequency(&frequency); last = now; }
    const double elapsed = double(now.QuadPart - last.QuadPart) / double(frequency.QuadPart);
    if (elapsed >= 0.008)
    {
        int dx, dy;
        PresenterTakeMouseMotion(dx, dy);
        last = now;
        auto stick = [&](int counts) -> int16_t {
            if (!counts) return 0;
            const double rate = double(counts) / elapsed;                      // counts per second
            const double deflection = rate * sensitivity / (2400.0 * 5.0);     // sensitivity 5: full tilt at 2400 counts/s
            const double magnitude = (std::min)(1.0, std::fabs(deflection));
            const double value = 9000.0 + magnitude * (32767.0 - 9000.0);     // start above the dead zone
            return int16_t(deflection < 0 ? -value : value);
        };
        stickX = stick(dx);
        stickY = stick(-dy);   // moving the mouse away looks up
    }
    bool used = false;
    if (abs(stickX) > abs(pad.rx)) { pad.rx = stickX; used = true; }
    if (abs(stickY) > abs(pad.ry)) { pad.ry = stickY; used = true; }
    return used;
}

const KeyAction& InputKeyAction(int index) { return kActions[index].info; }
int InputGetKey(int index) { EnsureKeys(); return gKeys[index].load(); }

// Binds a key; an action that already had it takes this action's old key, so nothing is left unbound.
void InputSetKey(int index, int key)
{
    EnsureKeys();
    const int previous = gKeys[index].exchange(key);
    for (int i = 0; i < kKeyActions; i++)
        if (i != index && gKeys[i].load() == key) gKeys[i] = previous;
}

bool InputSetKeyById(const char* id, int key)
{
    EnsureKeys();
    for (int i = 0; i < kKeyActions; i++)
        if (strcmp(kActions[i].info.id, id) == 0)
        {
            // Settings saved before Esc became the pause key still have Enter there.
            if (strcmp(id, "start") == 0 && key == VK_RETURN) key = VK_ESCAPE;
            gKeys[i] = key;
            return true;
        }
    return false;
}

void InputResetKeys()
{
    for (int i = 0; i < kKeyActions; i++) gKeys[i] = kActions[i].info.defaultKey;
    gKeysReady = true;
}

std::string InputKeyName(int key)
{
    switch (key)
    {
    case 0: return "(none)";
    case VK_LBUTTON: return "Left Mouse"; case VK_RBUTTON: return "Right Mouse"; case VK_MBUTTON: return "Middle Mouse";
    case VK_XBUTTON1: return "Mouse 4"; case VK_XBUTTON2: return "Mouse 5";
    case VK_UP: return "Up Arrow"; case VK_DOWN: return "Down Arrow"; case VK_LEFT: return "Left Arrow"; case VK_RIGHT: return "Right Arrow";
    case VK_LSHIFT: return "Left Shift"; case VK_RSHIFT: return "Right Shift"; case VK_LCONTROL: return "Left Ctrl"; case VK_RCONTROL: return "Right Ctrl";
    case VK_LMENU: return "Left Alt"; case VK_RMENU: return "Right Alt"; case VK_RETURN: return "Enter"; case VK_SPACE: return "Space";
    case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End";
    case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down";
    }
    char name[64] = {};
    const UINT scan = MapVirtualKeyA(UINT(key), MAPVK_VK_TO_VSC);
    if (scan && GetKeyNameTextA(LONG(scan << 16), name, sizeof(name)) > 0) return name;
    snprintf(name, sizeof(name), "Key %d", key);
    return name;
}

// While capturing, keys don't act as the pad; InputPollNewKey returns the next key pressed once every
// key has been released (0 until then). NIER_FAKE_KEY=<frame>:<vk> presses one for tests.
void InputSetCapture(bool on) { gCapture = on; gCaptureArmed = false; }

int InputPollNewKey()
{
    static const std::pair<uint64_t, int> fake = [] {
        char value[32];
        if (!GetEnvironmentVariableA("NIER_FAKE_KEY", value, sizeof(value))) return std::pair<uint64_t, int>{ ~0ull, 0 };
        const char* colon = strchr(value, ':');
        return std::pair<uint64_t, int>{ strtoull(value, nullptr, 10), colon ? int(strtol(colon + 1, nullptr, 0)) : 0 };
    }();
    if (fake.second && GpuFrameCount() >= fake.first) return fake.second;
    if (!PresenterHasFocus()) return 0;
    int down = 0;
    for (int key = 1; key < 0xFF; key++)   // 1, 2, 4, 5, 6 are the mouse buttons
    {
        if (key == VK_CANCEL || key == 7) continue;
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;   // the left/right variants are reported instead
        if (Key(key)) { down = key; break; }
    }
    if (!gCaptureArmed.load()) { if (!down) gCaptureArmed = true; return 0; }
    return down;
}

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

    // NIER_IGNORE_PADS=1: controllers are not read. Test runs set it (run_boot.ps1), or a controller
    // in the player's hands would drive them too; a recording run leaves it off.
    static const bool ignorePads = GetEnvironmentVariableA("NIER_IGNORE_PADS", nullptr, 0) > 0;
    PadState pad;
    for (uint32_t slot = 0; slot < USERS && !ignorePads; slot++)
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
    if (ApplyMouseCamera(pad)) SetLastStyle(InputStyle::Keyboard);
    if (GetTickCount64() < gInjectUntil.load()) pad.buttons |= gInjected.load();
    {
        // NIER_AUTO_STICK=<frame>,<frames>,<rx>,<ry>: holds the right stick for tests.
        static const struct Auto { uint64_t frame = ~0ull, frames = 0; int rx = 0, ry = 0; } hold = [] {
            Auto a; char value[64];
            if (GetEnvironmentVariableA("NIER_AUTO_STICK", value, sizeof(value))) sscanf(value, "%llu,%llu,%d,%d", &a.frame, &a.frames, &a.rx, &a.ry);
            return a;
        }();
        const uint64_t frame = GpuFrameCount();
        if (frame >= hold.frame && frame < hold.frame + hold.frames) { pad.rx = int16_t(hold.rx); pad.ry = int16_t(hold.ry); }
    }
    {
        // NIER_AUTO_MOVE=<frame>,<frames>,<lx>,<ly>[;...]: holds the left stick over those frames, to
        // walk somewhere in a test run (stick range -32768..32767, y up).
        struct Move { uint64_t frame, frames; int lx, ly; };
        static const std::vector<Move> moves = [] {
            std::vector<Move> list; char value[512];
            if (GetEnvironmentVariableA("NIER_AUTO_MOVE", value, sizeof(value)))
                for (char* item = strtok(value, ";"); item; item = strtok(nullptr, ";"))
                {
                    Move m{};
                    if (sscanf(item, "%llu,%llu,%d,%d", &m.frame, &m.frames, &m.lx, &m.ly) == 4) list.push_back(m);
                }
            return list;
        }();
        const uint64_t frame = GpuFrameCount();
        for (const Move& m : moves)
            if (frame >= m.frame && frame < m.frame + m.frames) { pad.lx = int16_t(m.lx); pad.ly = int16_t(m.ly); }
    }
    pad.buttons |= ScriptedButtons();
    {
        // Recorded input for test runs. NIER_RECORD_INPUT=<file> writes the pad state the game is
        // given each time it changes ("frame buttons lt rt lx ly rx ry"); NIER_REPLAY_INPUT=<file>
        // gives the game that state back by frame and ignores every device (scripted presses still
        // apply). Loading
        // takes wall-clock time, so a replay drifts by a few frames: fine for walking somewhere,
        // not for a fight.
        struct Entry { uint64_t frame; PadState pad; };
        static const std::vector<Entry> replay = [] {
            std::vector<Entry> list; char path[MAX_PATH];
            if (GetEnvironmentVariableA("NIER_REPLAY_INPUT", path, sizeof(path)))
                if (FILE* file = fopen(path, "r"))
                {
                    unsigned long long frame; unsigned buttons, lt, rt; int lx, ly, rx, ry;
                    while (fscanf(file, "%llu %x %u %u %d %d %d %d", &frame, &buttons, &lt, &rt, &lx, &ly, &rx, &ry) == 8)
                        list.push_back({ frame, { uint16_t(buttons), uint8_t(lt), uint8_t(rt), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry) } });
                    fclose(file);
                    Log("[input] replaying %zu recorded pad states from %s\n", list.size(), path);
                }
            return list;
        }();
        const uint64_t frame = GpuFrameCount();
        if (!replay.empty())
        {
            auto next = std::upper_bound(replay.begin(), replay.end(), frame, [](uint64_t f, const Entry& e) { return f < e.frame; });
            pad = next == replay.begin() ? PadState{} : (next - 1)->pad;
            pad.buttons |= ScriptedButtons();   // NIER_AUTO_PRESS still applies, to go on from where a recording ends
        }
        static FILE* record = [] {
            char path[MAX_PATH];
            return GetEnvironmentVariableA("NIER_RECORD_INPUT", path, sizeof(path)) ? fopen(path, "w") : nullptr;
        }();
        if (record)
        {
            static std::mutex mutex;
            static PadState last;
            static bool any = false;
            std::lock_guard lock(mutex);
            if (!any || memcmp(&last, &pad, sizeof(pad)) != 0)
            {
                fprintf(record, "%llu %x %u %u %d %d %d %d\n", (unsigned long long)frame, pad.buttons, pad.leftTrigger, pad.rightTrigger, pad.lx, pad.ly, pad.rx, pad.ry);
                fflush(record);
                last = pad; any = true;
            }
        }
    }

    if (state)
    {
        static std::atomic<int> logged{ 0 };
        if (GetEnvironmentVariableA("NIER_LOG_PAD", nullptr, 0) > 0 && GpuFrameCount() > 3000 && logged++ < 4)
            Log("[input] pad state buffer %08X, caller %s\n", state, GuestCallChain(6).c_str());
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
