// PC settings: saved in settings.ini beside the executable, edited from the in-game "PC Settings"
// page (pcsettings.cpp). Environment variables still override the file for test runs.
#pragma once
#include <atomic>

struct PcSettings
{
    std::atomic<int> borderless{ 1 };          // display mode: 0 windowed, 1 borderless fullscreen
    std::atomic<int> frameRate60{ 1 };         // 0: the original 30 fps cap, 1: 60 fps
    std::atomic<int> vsync{ 1 };
    // Internal resolution (applied between frames): 0 640x360, 1 854x480, 2 960x540, 3 1280x720 (the original),
    // 4 1600x900, 5 1920x1080, 6 2560x1440, 7 3200x1800, 8 3840x2160, 9 5120x2880.
    std::atomic<int> resolutionScale{ 3 };
    std::atomic<int> masterVolume{ 10 };       // 0-10
    std::atomic<int> muteInBackground{ 1 };    // silence while the window isn't focused
    std::atomic<int> mouseSensitivity{ 5 };    // 0-10; 0 turns the mouse camera off
    std::atomic<int> buttonIcons{ 0 };         // controller icons: 0 auto (last controller used), 1 Xbox Series, 2 PS4, 3 PS5, 4 Steam, 5 the game's own (Xbox 360)
};

PcSettings& Settings();
void SettingsLoad();     // reads settings.ini, then applies environment overrides
void SettingsSave();
void SettingsPollPage();   // live values while the PC Settings page is open (called from the audio thread)

// Applying settings that need more than a read at the point of use.
void PresenterSetFullscreen(bool on);   // present.cpp; safe from any thread
void PresenterTakeMouseMotion(int& dx, int& dy);   // mouse movement since the last call, in device counts

// Menu pointer (present.cpp): the cursor in the game's 1280x720 screen space and what the mouse did
// since the last call. False when the window isn't focused (or the pointer is outside the picture).
struct MenuMouse { float x = 0, y = 0; bool moved = false; int leftClicks = 0, rightClicks = 0, wheel = 0; };
bool PresenterTakeMenuMouse(MenuMouse& mouse);
