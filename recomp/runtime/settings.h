// PC settings: saved in settings.ini beside the executable, edited from the in-game "PC Settings"
// page (pcsettings.cpp). Environment variables still override the file for test runs.
#pragma once
#include <atomic>

struct PcSettings
{
    std::atomic<int> borderless{ 1 };          // display mode: 0 windowed, 1 borderless fullscreen
    std::atomic<int> frameRate60{ 1 };         // Frame Rate: 0 the original 30 fps cap, 1 60 fps, 2 uncapped (experimental)
    std::atomic<int> vsync{ 1 };
    // Internal resolution (applied between frames): 0 640x360, 1 854x480, 2 960x540, 3 1280x720 (the original),
    // 4 1600x900, 5 1920x1080, 6 2560x1440, 7 3200x1800, 8 3840x2160, 9 5120x2880.
    std::atomic<int> resolutionScale{ 3 };
    std::atomic<int> upscaling{ 0 };           // how a lower render resolution is enlarged: 0 plain filtering, 1 FSR 1, 2 FSR 3.1 (temporal)
    std::atomic<int> showFps{ 0 };             // frame rate counter in the top left corner
    std::atomic<int> savePage{ 1 };            // which page of three game saves the save list shows (1-10)
    std::atomic<int> shadowQuality{ 0 };       // shadow map size: 0 the original 1024, 1 2048, 2 3072, 3 4096
    std::atomic<int> masterVolume{ 10 };       // 0-10
    std::atomic<int> muteInBackground{ 1 };    // silence while the window isn't focused
    std::atomic<int> mouseSensitivity{ 5 };    // 0-10; 0 turns the mouse camera off
    std::atomic<int> buttonIcons{ 0 };         // controller icons: 0 auto (last controller used), 1 Xbox Series, 2 PS4, 3 PS5, 4 Steam, 5 the game's own (Xbox 360)
};

PcSettings& Settings();
void SettingsLoad();     // reads settings.ini, then applies environment overrides
void SettingsSave();
// The rate of the game's vertical blank, which is what paces its frames: 60 Hz, or 0 for the
// Uncapped frame rate, where vblanks follow the display or each frame (NIER_GUEST_HZ=<rate>
// overrides, -1 for uncapped; other rates, such as 120, exist only through it, for testing).
int GuestRefreshHz();
void SettingsPollPage();   // live values while the PC Settings page is open (called from the audio thread)

// Applying settings that need more than a read at the point of use.
void PresenterSetFullscreen(bool on);   // present.cpp; safe from any thread
void PresenterTakeMouseMotion(int& dx, int& dy);   // mouse movement since the last call, in device counts

// Menu pointer (present.cpp): the cursor in the game's 1280x720 screen space and what the mouse did
// since the last call. False when the window isn't focused (or the pointer is outside the picture).
struct MenuMouse { float x = 0, y = 0; bool moved = false; int leftClicks = 0, rightClicks = 0, wheel = 0; };
bool PresenterTakeMenuMouse(MenuMouse& mouse);
