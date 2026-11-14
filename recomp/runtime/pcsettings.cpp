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
            { "master_volume", &gSettings.masterVolume, 0, 10 },
            { "mute_in_background", &gSettings.muteInBackground, 0, 1 },
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
        T_Count
    };
    const char* kStrings[T_Count - kTextBase] = {
        "PC Settings", "Adjust display, frame rate and audio settings for PC.",
        "Display Mode", "Play in a window or in borderless fullscreen (also Alt+Enter).",
        "Frame Rate", "Choose 60 fps, or the original 30 fps cap.",
        "VSync", "Synchronize frames with the display to prevent tearing.",
        "Master Volume", "Adjust the overall volume.",
        "Mute in Background", "Silence the game while its window isn't focused.",
        "Save PC settings and return to Game Settings.",
        "Windowed", "Borderless", "30 fps", "60 fps", "On", "Off",
        "Quit to Desktop",
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

    // ---- the page -----------------------------------------------------------------------------
    constexpr uint32_t kRecords = 3916, kRecordSize = 56, kRecordCount = 24, kRecordsInUse = 5264, kPageKind = 3556;
    constexpr uint32_t kScroll = 3572, kCursorRow = 3576;
    constexpr uint32_t kSelectDeviceLabel = 11100300, kReturnLabel = 6731000;
    constexpr uint32_t kItemReturn = 22, kItemSpacer = 37, kItemSelectDevice = 27;

    struct Item { uint32_t id; uint8_t type; std::atomic<int>* value; uint8_t max; Text label, help; Text values[2]; };
    const Item kItems[] = {
        { 60, 2, &gSettings.borderless, 1, T_DisplayMode, T_DisplayModeHelp, { T_Windowed, T_Borderless } },
        { 61, 2, &gSettings.frameRate60, 1, T_FrameRate, T_FrameRateHelp, { T_30fps, T_60fps } },
        { 62, 2, &gSettings.vsync, 1, T_VSync, T_VSyncHelp, { T_Off, T_On } },
        { 63, 6, &gSettings.masterVolume, 10, T_MasterVolume, T_MasterVolumeHelp, {} },
        { 64, 2, &gSettings.muteInBackground, 1, T_MuteInBackground, T_MuteInBackgroundHelp, { T_Off, T_On } },
    };
    const Item* FindItem(uint32_t id)
    {
        for (const Item& item : kItems) if (item.id == id) return &item;
        return nullptr;
    }

    // One menu at a time can be showing the PC page; its Game Settings records wait here.
    std::mutex gPageMutex;
    uint32_t gPcMenu = 0;
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
            WriteRecord(first + n++ * kRecordSize, item.type, uint8_t(item.value->load()), 0, item.max, item.type == 2 ? 1 : 0, item.id, item.label, item.help);
        WriteRecord(first + n++ * kRecordSize, 8, 0, 0, 0, 0, kItemSpacer, 0xFFFFFFFF, 0xFFFFFFFF);
        WriteRecord(first + n++ * kRecordSize, 10, 0, 0, 0, 0, kItemReturn, kReturnLabel, T_ReturnHelp);
        for (uint32_t i = n; i < kRecordCount; i++) ClearRecord(first + i * kRecordSize);
        return n;
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
    fclose(file);
    Log("[settings] saved %s\n", path.c_str());
}

// Called often (each audio frame): while the PC page is open its values apply as they change.
void SettingsPollPage()
{
    std::unique_lock lock(gPageMutex, std::try_to_lock);
    if (lock && gPcMenu) ReadPcRecords(gPcMenu, false);
}

// ---- mid-asm hooks (nier_hooks.toml) -------------------------------------------------------------

// sub_823CB8C8 (text table, id): our ids return our strings.
bool PcSettingsText(PPCRegister& r3, PPCRegister& r4)
{
    if (r4.u32 < kTextBase || r4.u32 >= T_Count) return false;
    r3.u64 = GuestText(r4.u32);
    return true;
}

// sub_82416C20 (record): the value text of our choices.
bool PcSettingsValueText(PPCRegister& r3)
{
    const Item* item = FindItem(R32(r3.u32 + 40));
    if (!item || item->type != 2) return false;
    const int value = std::clamp(int(int8_t(R8(r3.u32 + 1))), 0, 1);
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
    {
        std::lock_guard lock(gPageMutex);
        if (gPcMenu != menu || gSavedRecords.size() != kRecordCount * kRecordSize) return false;
        ReadPcRecords(menu, true);
        memcpy(g_base + menu + kRecords, gSavedRecords.data(), gSavedRecords.size());
        W32(menu + kRecordsInUse, gSavedCount);
        W32(menu + kScroll, gSavedScroll);
        W32(menu + kCursorRow, gSavedCursorRow);
        gPcMenu = 0;
    }
    SettingsSave();
    ApplySettings();
    LayOutPage(menu);
    Log("[settings] PC Settings page closed\n");
    return true;
}
