// PC settings: the settings file and the in-game "PC Settings" page.
//
// The page lives in the game's own Options menus. "Select Device" (no storage devices on PC) at the
// bottom of Game Settings, from the title and from the pause menu, is relabelled "PC Settings"; choosing
// it swaps the page's records for ours and lays the page out again, and Return / B swaps the Game
// Settings records back. All of it runs through mid-asm hooks (recomp/config/nier_hooks.toml):
//
//   menu object (sub_8241A188 builds its settings page, sub_8241C020 handles input):
//     +3556 current page kind (4-7 are settings pages)
//     +3572 first record in view, +3576 the cursor's row within the view
//     +3916 24 records of 56 bytes: +0 type, +1 value, +2 min, +3 max, +4 default, +5 flag,
//           +6 32-byte text buffer, +40 item id, +44 label text id, +48 help text id, +52 extra
//           types: 2 on/off, 4 three-way, 5 slider, 6 volume bars, 8 spacer, 9 page, 10 action
//     +5264 records in use
//   item ids 4-21 are copied into the game config by sub_82415C48 and 22-36 are actions (22 Return,
//   26 Restore Defaults, 27 Select Device); ours start at 60 so the game ignores them.
//   text ids go through sub_823CB8C8 (table, id) -> string; a record's value text through
//   sub_82416C20 (record) -> string.
#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "guest.h"
#include "settings.h"
#include "input.h"
#include "glyphs.h"

extern "C" PPC_FUNC(__imp__sub_824197A8);
extern "C" PPC_FUNC(__imp__sub_824178D0);
extern "C" PPC_FUNC(__imp__sub_82416168);
extern "C" PPC_FUNC(__imp__sub_82419120);
extern "C" PPC_FUNC(__imp__sub_82416038);

namespace
{
    PcSettings gSettings;

    std::string SettingsPath()
    {
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("NIER_SETTINGS_FILE", path, sizeof(path))) return path;
        GetModuleFileNameA(nullptr, path, sizeof(path));
        std::string text = path;
        return text.substr(0, text.find_last_of("\\/") + 1) + "settings.ini";
    }

    struct Field { const char* key; std::atomic<int>* value; int min, max; };
    std::vector<Field> Fields()
    {
        return {
            { "borderless", &gSettings.borderless, 0, 1 },
            { "frame_rate_60", &gSettings.frameRate60, 0, 1 },
            { "vsync", &gSettings.vsync, 0, 1 },
            { "resolution_scale", &gSettings.resolutionScale, 0, 9 },
            { "shadow_quality", &gSettings.shadowQuality, 0, 3 },
            { "master_volume", &gSettings.masterVolume, 0, 10 },
            { "mute_in_background", &gSettings.muteInBackground, 0, 1 },
            { "mouse_sensitivity", &gSettings.mouseSensitivity, 0, 10 },
            { "button_icons", &gSettings.buttonIcons, 0, 5 },
        };
    }

    // ---- text ---------------------------------------------------------------------------------
    // Our text ids sit far above the game's (which are below 0x02000000).
    constexpr uint32_t kTextBase = 0x7E000000;
    enum Text : uint32_t
    {
        T_PcSettings = kTextBase, T_PcSettingsHelp,
        T_DisplayMode, T_DisplayModeHelp, T_FrameRate, T_FrameRateHelp, T_VSync, T_VSyncHelp,
        T_MasterVolume, T_MasterVolumeHelp, T_MuteInBackground, T_MuteInBackgroundHelp, T_ReturnHelp,
        T_Windowed, T_Borderless, T_30fps, T_60fps, T_On, T_Off,
        T_QuitToDesktop,
        T_KeysButtons, T_KeysButtonsHelp, T_KeysMove, T_KeysMoveHelp, T_KeysDefaults, T_KeysDefaultsHelp, T_KeyHelp, T_KeysReturnHelp, T_PressKey,
        T_MouseSensitivity, T_MouseSensitivityHelp,
        T_Icons, T_IconsHelp, T_IconAuto, T_IconXbox, T_IconPs4, T_IconPs5, T_IconSteam, T_IconOriginal,
        T_Resolution, T_ResolutionHelp, T_Res360, T_Res480, T_Res540, T_Res720, T_Res900, T_Res1080, T_Res1440, T_Res1800, T_Res2160, T_Res2880,
        T_Shadows, T_ShadowsHelp, T_ShadowOriginal, T_ShadowHigh, T_ShadowVeryHigh, T_ShadowUltra,
        T_Count
    };
    const char* kStrings[T_Count - kTextBase] = {
        "PC Settings", "Adjust display, frame rate and audio settings for PC.",
        "Display Mode", "Play in a window or in borderless fullscreen\n(also Alt+Enter).",
        "Frame Rate", "Choose 60 fps, or the original 30 fps cap.",
        "VSync", "Synchronize frames with the display to prevent tearing.",
        "Master Volume", "Adjust the overall volume.",
        "Mute in Background", "Silence the game while its window isn't focused.",
        "Save PC settings and return to Game Settings.",
        "Windowed", "Borderless", "30 fps", "60 fps", "On", "Off",
        "Quit to Desktop",
        "Key Bindings: Actions", "Choose the keys and mouse buttons for attacks,\nabilities and menus.",
        "Key Bindings: Movement", "Choose the keyboard keys for movement and the camera.",
        "Restore Default Keys", "Put every key binding back to its default.",
        "Press Left or Right, then the new key or mouse button.\nBackspace cancels.",
        "Save the key bindings and return to PC Settings.",
        "Press a key...",
        "Mouse Sensitivity", "Adjust how fast the mouse turns the camera.\nThe lowest setting turns it off.",
        "Button Icons", "Choose the controller button icons.\nKeyboard and mouse always show their own.",
        "Auto", "Xbox Series", "PlayStation 4", "PlayStation 5", "Steam Deck", "Xbox 360",
        "Resolution", "Choose the resolution the game is drawn at.\nHigher is sharper but asks more of the graphics card.",
        "640 x 360", "854 x 480", "960 x 540", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3200 x 1800", "3840 x 2160", "5120 x 2880",
        "Shadow Quality", "Choose how detailed shadows are.\nOriginal is the game's own, with blocky edges.",
        "Original", "High", "Very High", "Ultra",
    };
    uint32_t gStrings[T_Count - kTextBase] = {};   // guest copies
    std::once_flag gStringsOnce;

    uint32_t GuestText(uint32_t id)
    {
        std::call_once(gStringsOnce, [] {
            for (uint32_t i = 0; i < T_Count - kTextBase; i++)
            {
                const size_t n = strlen(kStrings[i]) + 1;
                gStrings[i] = GuestAlloc(uint32_t(n), 4, Region::Heap);
                memcpy(g_base + gStrings[i], kStrings[i], n);
            }
        });
        return gStrings[id - kTextBase];
    }

    // Text that changes (key names): one guest buffer per slot, rewritten in place.
    constexpr uint32_t kKeyLabelText = kTextBase + 0x1000;   // + action index: the action's name
    uint32_t DynamicText(uint32_t slot, const std::string& text)
    {
        static uint32_t buffers[kKeyActions * 2] = {};
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        if (!buffers[slot]) buffers[slot] = GuestAlloc(64, 4, Region::Heap);
        strncpy(reinterpret_cast<char*>(g_base + buffers[slot]), text.c_str(), 63);
        W8(buffers[slot] + 63, 0);
        return buffers[slot];
    }

    // ---- the page -----------------------------------------------------------------------------
    constexpr uint32_t kRecords = 3916, kRecordSize = 56, kRecordCount = 24, kRecordsInUse = 5264, kPageKind = 3556;
    constexpr uint32_t kScroll = 3572, kCursorRow = 3576;
    constexpr uint32_t kSelectDeviceLabel = 11100300, kReturnLabel = 6731000;
    constexpr uint32_t kItemReturn = 22, kItemSpacer = 37, kItemSelectDevice = 27;
    constexpr uint32_t kItemKeysButtons = 70, kItemKeysMove = 71, kItemKeysDefaults = 72, kItemKey = 100;   // kItemKey + action index
    constexpr int kKeysPerPage = 12;

    struct Item { uint32_t id; uint8_t type; std::atomic<int>* value; uint8_t max; Text label, help; Text values[10]; };   // types 2 and 4 name their values
    const Item kItems[] = {
        { 60, 2, &gSettings.borderless, 1, T_DisplayMode, T_DisplayModeHelp, { T_Windowed, T_Borderless } },
        { 61, 2, &gSettings.frameRate60, 1, T_FrameRate, T_FrameRateHelp, { T_30fps, T_60fps } },
        { 62, 2, &gSettings.vsync, 1, T_VSync, T_VSyncHelp, { T_Off, T_On } },
        { 67, 4, &gSettings.resolutionScale, 9, T_Resolution, T_ResolutionHelp, { T_Res360, T_Res480, T_Res540, T_Res720, T_Res900, T_Res1080, T_Res1440, T_Res1800, T_Res2160, T_Res2880 } },
        { 68, 4, &gSettings.shadowQuality, 3, T_Shadows, T_ShadowsHelp, { T_ShadowOriginal, T_ShadowHigh, T_ShadowVeryHigh, T_ShadowUltra } },
        { 63, 6, &gSettings.masterVolume, 10, T_MasterVolume, T_MasterVolumeHelp, {} },
        { 64, 2, &gSettings.muteInBackground, 1, T_MuteInBackground, T_MuteInBackgroundHelp, { T_Off, T_On } },
        { 65, 6, &gSettings.mouseSensitivity, 10, T_MouseSensitivity, T_MouseSensitivityHelp, {} },
        { 66, 4, &gSettings.buttonIcons, 5, T_Icons, T_IconsHelp, { T_IconAuto, T_IconXbox, T_IconPs4, T_IconPs5, T_IconSteam, T_IconOriginal } },
    };
    const Item* FindItem(uint32_t id)
    {
        for (const Item& item : kItems) if (item.id == id) return &item;
        return nullptr;
    }

    // One menu at a time can be showing the PC page; its Game Settings records wait here.
    // Pages: PC Settings sits on Game Settings; the two Key Bindings pages sit on PC Settings.
    enum class Page { Pc, Keys };
    std::mutex gPageMutex;
    uint32_t gPcMenu = 0;
    Page gPage = Page::Pc;
    int gKeysFirst = 0;                 // first action on the open Key Bindings page
    uint32_t gPcCursorRow = 0;          // where the cursor was on PC Settings
    int gCaptureAction = -1;            // action waiting for a key, or -1
    uint32_t gCaptureRow = 0;
    std::vector<uint8_t> gSavedRecords;
    uint32_t gSavedCount = 0, gSavedScroll = 0, gSavedCursorRow = 0;

    void WriteRecord(uint32_t record, uint8_t type, uint8_t value, uint8_t min, uint8_t max, uint8_t flag, uint32_t item, uint32_t label, uint32_t help)
    {
        memset(g_base + record, 0, kRecordSize);
        W8(record + 0, type); W8(record + 1, value); W8(record + 2, min); W8(record + 3, max); W8(record + 4, value); W8(record + 5, flag);
        W32(record + 40, item); W32(record + 44, label); W32(record + 48, help); W32(record + 52, 0xFFFFFFFF);
    }

    void ClearRecord(uint32_t record)
    {
        memset(g_base + record, 0, kRecordSize);
        W32(record + 40, 0xFFFFFFFF);
        W32(record + 52, 0xFFFFFFFF);
    }

    uint32_t FillPcRecords(uint32_t menu)
    {
        const uint32_t first = menu + kRecords;
        uint32_t n = 0;
        for (const Item& item : kItems)
            WriteRecord(first + n++ * kRecordSize, item.type, uint8_t(item.value->load()), 0, item.max, (item.type == 2 || item.type == 4) ? 1 : 0, item.id, item.label, item.help);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemKeysButtons, T_KeysButtons, T_KeysButtonsHelp);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemKeysMove, T_KeysMove, T_KeysMoveHelp);
        WriteRecord(first + n++ * kRecordSize, 8, 0, 0, 0, 0, kItemSpacer, 0xFFFFFFFF, 0xFFFFFFFF);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemReturn, kReturnLabel, T_ReturnHelp);
        for (uint32_t i = n; i < kRecordCount; i++) ClearRecord(first + i * kRecordSize);
        return n;
    }

    // A Key Bindings page: one two-way row per action (changing it starts the key capture; its value
    // text is the key's name), Restore Default Keys and Return.
    uint32_t FillKeyRecords(uint32_t menu, int firstAction)
    {
        const uint32_t first = menu + kRecords;
        uint32_t n = 0;
        // Actions: the buttons (the left stick click, 10, does nothing in this game) and the D-pad's
        // menus; Movement: the two sticks.
        static const int kActionsPage[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 20, 21, 22, 23 };
        static const int kMovementPage[] = { 12, 13, 14, 15, 16, 17, 18, 19 };
        const int* list = firstAction ? kMovementPage : kActionsPage;
        const int count = firstAction ? int(std::size(kMovementPage)) : int(std::size(kActionsPage));
        for (int i = 0; i < count; i++)
            WriteRecord(first + n++ * kRecordSize, 2, 0, 0, 1, 1, kItemKey + list[i], kKeyLabelText + list[i], T_KeyHelp);
        WriteRecord(first + n++ * kRecordSize, 8, 0, 0, 0, 0, kItemSpacer, 0xFFFFFFFF, 0xFFFFFFFF);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemKeysDefaults, T_KeysDefaults, T_KeysDefaultsHelp);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemReturn, kReturnLabel, T_KeysReturnHelp);
        for (uint32_t i = n; i < kRecordCount; i++) ClearRecord(first + i * kRecordSize);
        return n;
    }

    void EndCapture()
    {
        gCaptureAction = -1;
        InputSetCapture(false);
    }

    // Copies the page's values into the settings. Display Mode waits for Return (live, the window
    // would switch while scrolling past it); the rest apply as they change.
    void ReadPcRecords(uint32_t menu, bool includeDisplayMode)
    {
        for (uint32_t i = 0; i < kRecordCount; i++)
        {
            const uint32_t record = menu + kRecords + i * kRecordSize;
            const Item* item = FindItem(R32(record + 40));
            if (!item || (!includeDisplayMode && item->value == &gSettings.borderless)) continue;
            item->value->store(std::clamp(int(int8_t(R8(record + 1))), 0, int(item->max)));
        }
    }

    // Lays the page out again from its records: the calls the builder makes after filling them
    // (without its copy of the game config into the menu, so pending Game Settings changes stay).
    void LayOutPage(uint32_t menu)
    {
        PPCContext& ctx = *CurrentContext();
        const PPCContext saved = ctx;
        const uint32_t kind = R32(menu + kPageKind);
        using Function = void (*)(PPCContext&, uint8_t*);
        for (Function f : { __imp__sub_824197A8, __imp__sub_824178D0, __imp__sub_82416168, __imp__sub_82419120, __imp__sub_82416038 })
        {
            ctx.r3.u64 = menu;
            ctx.r4.u64 = kind;
            f(ctx, g_base);
        }
        ctx = saved;
    }

    void ApplySettings()
    {
        PresenterSetFullscreen(gSettings.borderless != 0);
    }
}

PcSettings& Settings() { return gSettings; }

void SettingsLoad()
{
    const std::string path = SettingsPath();
    if (FILE* file = fopen(path.c_str(), "r"))
    {
        char line[256];
        while (fgets(line, sizeof(line), file))
        {
            char key[64];
            int value;
            if (sscanf(line, " %63[^= ] = %d", key, &value) != 2) continue;
            for (const Field& field : Fields())
                if (strcmp(field.key, key) == 0) field.value->store(std::clamp(value, field.min, field.max));
            if (strncmp(key, "key_", 4) == 0) InputSetKeyById(key + 4, std::clamp(value, 0, 254));
        }
        fclose(file);
        Log("[settings] loaded %s\n", path.c_str());
    }
    // Environment overrides (test runs).
    char value[16];
    if (GetEnvironmentVariableA("NIER_FULLSCREEN", value, sizeof(value))) gSettings.borderless = value[0] == '1';
    if (GetEnvironmentVariableA("NIER_60FPS", value, sizeof(value))) gSettings.frameRate60 = value[0] != '0';
    if (GetEnvironmentVariableA("NIER_30FPS", value, sizeof(value))) gSettings.frameRate60 = 0;
}

void SettingsSave()
{
    const std::string path = SettingsPath();
    FILE* file = fopen(path.c_str(), "w");
    if (!file) { Log("[settings] couldn't write %s\n", path.c_str()); return; }
    fprintf(file, "; NieR (recompiled) PC settings. Edited from Options > Game Settings > PC Settings.\n");
    for (const Field& field : Fields()) fprintf(file, "%s = %d\n", field.key, field.value->load());
    fprintf(file, "; Keyboard bindings (Windows virtual-key codes), from PC Settings > Key Bindings.\n");
    for (int i = 0; i < kKeyActions; i++) fprintf(file, "key_%s = %d\n", InputKeyAction(i).id, InputGetKey(i));
    fclose(file);
    Log("[settings] saved %s\n", path.c_str());
}

// Called often (each audio frame): while the PC page is open its values apply as they change.
void SettingsPollPage()
{
    std::unique_lock lock(gPageMutex, std::try_to_lock);
    if (lock && gPcMenu && gPage == Page::Pc) ReadPcRecords(gPcMenu, false);
}

// ---- mid-asm hooks (nier_hooks.toml) -------------------------------------------------------------

// sub_823CB8C8 (text table, id): our ids return our strings.
// The game's texts that name the START button follow the device in use. Text ids are those of the
// game's text table (the same in every language; ours are English).
//   1112500  "Press the START Button", 1500400 "Press the START button": reworded ("Press the
//            OPTIONS Button", "Press the Esc Key"); where the game shows them isn't known.
//   11202000 "Press START to Skip" (cutscenes): the Start icon inline. <ICON=n> draws cell n of the
//            button atlas (13 is Start), so it follows the icon set like every other icon.
static bool StartPromptText(PPCRegister& r3, PPCRegister& r4)
{
    constexpr uint32_t kTitle = 1112500, kInstall = 1500400, kSkip = 11202000;
    if (r4.u32 != kTitle && r4.u32 != kInstall && r4.u32 != kSkip) return false;
    if (GlyphSetInUse() < 0) return false;   // the game's own icons: its own wording
    const std::string text = r4.u32 == kSkip ? "Press <ICON=13> to Skip" : StartPromptWording(r4.u32 == kTitle);
    static uint32_t buffers[3] = {};
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    uint32_t& buffer = buffers[r4.u32 == kTitle ? 0 : r4.u32 == kInstall ? 1 : 2];
    if (!buffer) buffer = GuestAlloc(64, 4, Region::Heap);
    strncpy(reinterpret_cast<char*>(g_base + buffer), text.c_str(), 63);
    W8(buffer + 63, 0);
    r3.u64 = buffer;
    return true;
}

bool PcSettingsText(PPCRegister& r3, PPCRegister& r4)
{
    if (StartPromptText(r3, r4)) return true;
    if (r4.u32 >= kKeyLabelText && r4.u32 < kKeyLabelText + kKeyActions)
    {
        r3.u64 = DynamicText(r4.u32 - kKeyLabelText, InputKeyAction(int(r4.u32 - kKeyLabelText)).label);
        return true;
    }
    if (r4.u32 < kTextBase || r4.u32 >= T_Count) return false;
    r3.u64 = GuestText(r4.u32);
    return true;
}

// sub_82416C20 (record): the value text of our choices.
bool PcSettingsValueText(PPCRegister& r3)
{
    const uint32_t id = R32(r3.u32 + 40);
    if (id >= kItemKey && id < kItemKey + kKeyActions)
    {
        // A key row: the game asks for its text when it changes the row (Left / Right), which is
        // the request to rebind it; PcSettingsTick takes the key.
        const int action = int(id - kItemKey);
        std::lock_guard lock(gPageMutex);
        if (R8(r3.u32 + 1) != 0 && gCaptureAction < 0 && gPcMenu && gPage == Page::Keys)
        {
            gCaptureAction = action;
            gCaptureRow = R32(gPcMenu + kScroll) + R32(gPcMenu + kCursorRow);
            InputSetCapture(true);
        }
        if (gCaptureAction == action) r3.u64 = GuestText(T_PressKey);
        else r3.u64 = DynamicText(kKeyActions + action, InputKeyName(InputGetKey(action)));
        return true;
    }
    const Item* item = FindItem(id);
    if (!item || (item->type != 2 && item->type != 4)) return false;
    const int value = std::clamp(int(int8_t(R8(r3.u32 + 1))), 0, int(item->max));
    r3.u64 = GuestText(item->values[value]);
    return true;
}

// sub_8241A188 after filling the records (r31 the menu): a fresh page is never the PC page, and
// "Select Device" becomes "PC Settings".
void PcSettingsPageBuilt(PPCRegister& r31)
{
    {
        std::lock_guard lock(gPageMutex);
        if (gPcMenu == r31.u32) gPcMenu = 0;
    }
    if (GetEnvironmentVariableA("NIER_STORAGE_MESSAGES", nullptr, 0) > 0) return;
    for (uint32_t i = 0; i < kRecordCount; i++)
    {
        const uint32_t record = r31.u32 + kRecords + i * kRecordSize;
        if (R32(record + 40) != kItemSelectDevice || R32(record + 44) != kSelectDeviceLabel) continue;
        W32(record + 44, T_PcSettings);
        W32(record + 48, T_PcSettingsHelp);
    }
}

// sub_8241C020, action item 27 (r31 the menu): open the PC page instead of the device selector.
bool PcSettingsOpen(PPCRegister& r31)
{
    if (GetEnvironmentVariableA("NIER_STORAGE_MESSAGES", nullptr, 0) > 0) return false;
    const uint32_t menu = r31.u32;
    {
        std::lock_guard lock(gPageMutex);
        gSavedRecords.assign(g_base + menu + kRecords, g_base + menu + kRecords + kRecordCount * kRecordSize);
        gSavedCount = R32(menu + kRecordsInUse);
        gSavedScroll = R32(menu + kScroll);
        gSavedCursorRow = R32(menu + kCursorRow);
        gPcMenu = menu;
        gPage = Page::Pc;
        EndCapture();
    }
    W32(menu + kRecordsInUse, FillPcRecords(menu));
    W32(menu + kScroll, 0);
    W32(menu + kCursorRow, 0);
    LayOutPage(menu);
    Log("[settings] PC Settings page opened\n");
    return true;
}

// sub_8241BB60 (r3 the menu), Return or B: on the PC page, keep the values and go back to Game Settings.
bool PcSettingsReturn(PPCRegister& r3)
{
    const uint32_t menu = r3.u32;
    bool backToPc = false;
    {
        std::lock_guard lock(gPageMutex);
        if (gPcMenu != menu || gSavedRecords.size() != kRecordCount * kRecordSize) return false;
        if (gPage == Page::Keys)
        {
            // Back from Key Bindings to PC Settings.
            EndCapture();
            gPage = Page::Pc;
            W32(menu + kRecordsInUse, FillPcRecords(menu));
            W32(menu + kScroll, 0);
            W32(menu + kCursorRow, gPcCursorRow);
            backToPc = true;
        }
        else
        {
        ReadPcRecords(menu, true);
        memcpy(g_base + menu + kRecords, gSavedRecords.data(), gSavedRecords.size());
        W32(menu + kRecordsInUse, gSavedCount);
        W32(menu + kScroll, gSavedScroll);
        W32(menu + kCursorRow, gSavedCursorRow);
        gPcMenu = 0;
        }
    }
    SettingsSave();
    if (backToPc) { LayOutPage(menu); return true; }
    ApplySettings();
    LayOutPage(menu);
    Log("[settings] PC Settings page closed\n");
    return true;
}

// sub_8241C020, an action row was chosen (r4 its item id, r31 the menu): our own actions.
bool PcSettingsAction(PPCRegister& r4, PPCRegister& r31)
{
    const uint32_t item = r4.u32, menu = r31.u32;
    if (item != kItemKeysButtons && item != kItemKeysMove && item != kItemKeysDefaults) return false;
    {
        std::lock_guard lock(gPageMutex);
        if (gPcMenu != menu) return false;
        EndCapture();
        if (item == kItemKeysDefaults) InputResetKeys();
        else
        {
            ReadPcRecords(menu, true);
            gPcCursorRow = R32(menu + kScroll) + R32(menu + kCursorRow);
            gPage = Page::Keys;
            gKeysFirst = item == kItemKeysMove ? kKeysPerPage : 0;
            W32(menu + kRecordsInUse, FillKeyRecords(menu, gKeysFirst));
            W32(menu + kScroll, 0);
            W32(menu + kCursorRow, 0);
        }
    }
    if (item == kItemKeysDefaults) SettingsSave();
    LayOutPage(menu);
    return true;
}

// The mouse as a pointer on a settings page (any of them: Game Settings, PC Settings, Key Bindings).
// Rows are 38 pixels apart from y = 172 in the game's 1280x720 space, nine to a screen, between
// x = 240 and 905; values sit around x = 755. Hovering moves the game's cursor (its row within
// the view, +3576); a left click confirms an action row or steps a value down / up depending on
// which side of the value was clicked; a right click goes back; the wheel moves the cursor.
static void MenuPointer(uint32_t menu)
{
    constexpr float kTop = 172, kRowHeight = 38, kLeft = 240, kRight = 905, kValueCentre = 755;
    constexpr uint32_t kVisibleRows = 9;
    const uint32_t kind = R32(menu + kPageKind);
    if (kind < 4 || kind > 7) return;
    InputKeepPointerMode();
    MenuMouse mouse;
    if (!PresenterTakeMenuMouse(mouse)) return;
    if (mouse.rightClicks) { InputInject(0x2000); return; }                     // B: back
    if (mouse.wheel) { InputInject(mouse.wheel > 0 ? 0x0001 : 0x0002); return; }   // d-pad up / down
    if (mouse.x < kLeft || mouse.x > kRight || mouse.y < kTop) return;
    const uint32_t row = uint32_t((mouse.y - kTop) / kRowHeight), scroll = R32(menu + kScroll), count = R32(menu + kRecordsInUse);
    if (row >= kVisibleRows || scroll + row >= count || scroll + row >= kRecordCount) return;
    const uint32_t record = menu + kRecords + (scroll + row) * kRecordSize;
    const uint8_t type = R8(record);
    if (type == 0 || type == 8) return;   // unused, spacer
    if ((mouse.moved || mouse.leftClicks) && R32(menu + kCursorRow) != row)
    {
        W32(menu + kCursorRow, row);
        LayOutPage(menu);
    }
    if (mouse.leftClicks)
    {
        if (type == 9 || type == 10) InputInject(0x1000);                        // A: open / confirm
        else InputInject(mouse.x < kValueCentre ? 0x0004 : 0x0008);              // d-pad left / right: adjust
    }
}

// sub_8241C020 entry, every frame a settings page is open (r3 the menu): the menu pointer, and
// finishing a key capture.
void PcSettingsTick(PPCRegister& r3)
{
    const uint32_t menu = r3.u32;
    {
        bool capturing;
        { std::lock_guard lock(gPageMutex); capturing = gCaptureAction >= 0; }
        if (!capturing) MenuPointer(menu);
        else InputKeepPointerMode();
    }
    bool changed = false;
    {
        std::lock_guard lock(gPageMutex);
        if (gPcMenu != menu || gPage != Page::Keys || gCaptureAction < 0) return;
        uint32_t record = menu + kRecords;
        for (uint32_t i = 0; i < kRecordCount; i++)
            if (R32(menu + kRecords + i * kRecordSize + 40) == kItemKey + uint32_t(gCaptureAction)) record = menu + kRecords + i * kRecordSize;
        const bool moved = R32(menu + kScroll) + R32(menu + kCursorRow) != gCaptureRow;
        const int key = moved ? VK_BACK : InputPollNewKey();   // Backspace cancels (Esc can be bound)
        if (!key) return;
        if (key != VK_BACK)
        {
            InputSetKey(gCaptureAction, key);
            Log("[settings] %s bound to %s\n", InputKeyAction(gCaptureAction).label, InputKeyName(key).c_str());
            changed = true;
        }
        W8(record + 1, 0);
        EndCapture();
    }
    if (changed) SettingsSave();
    LayOutPage(menu);
}
