// PC settings: saved in settings.ini beside the executable, edited from the in-game "PC Settings"
// page (pcsettings.cpp). Environment variables still override the file for test runs.
#pragma once
#include <atomic>

struct PcSettings
{
    std::atomic<int> borderless{ 1 };          // display mode: 0 windowed, 1 borderless fullscreen
    // Frame Rate: 0 the original 30 fps cap, 1 60 fps, 2 90, 3 120, 4 144, 5 240, 6 uncapped, 7 40 fps
    // (added later, for handhelds on a 40 Hz display: numbered last so saved settings keep their
    // meaning; the menu shows it between 30 and 60).
    std::atomic<int> frameRate60{ 1 };
    std::atomic<int> vsync{ 1 };
    // Output Resolution: the size of the picture the game makes, which is then fitted to the window
    // or screen. 0 the screen's own (the largest 16:9 picture that fits the window; borderless, the
    // monitor), 1 1280x720, 2 1600x900, 3 1920x1080, 4 2560x1440, 5 3200x1800, 6 3840x2160,
    // 7 5120x2880. A window is given that size. What the game is drawn at follows from it: the
    // output itself with Upscaling off, the output divided by the Upscaling Quality's factor with
    // it on. (Before 2026-11-19 a Resolution setting chose the drawn size directly and a quality
    // mode replaced it; nothing in the menu said which of the two was in force.)
    std::atomic<int> outputResolution{ 0 };
    // A drawn resolution stated outright, in place of the rule above (-1: none). Not a setting any
    // more: test and measuring code sets it (0 640x360, 1 854x480, 2 960x540, 3 1280x720, 4 1600x900,
    // 5 1920x1080, 6 2560x1440, 7 3200x1800, 8 3840x2160, 9 5120x2880).
    std::atomic<int> resolutionScale{ -1 };
    std::atomic<int> upscaling{ 0 };           // how a lower render resolution is enlarged: 0 plain filtering, 1 FSR 1, 2 FSR 3.1, 3 FSR 4, 4 DLSS (the last three temporal)
    // Upscaling Quality: with Upscaling on, the drawn resolution as a share of the output's:
    // 1 Native (the output's own; with FSR 3.1 or 4 that is their anti-aliasing alone, "Native
    // AA"), 2 Ultra Quality (output / 1.3), 3 Quality (1.5), 4 Balanced (1.7), 5 Performance
    // (2.0), 6 Ultra Performance (3.0). FSR's own factors. (0 was "Custom", the old Resolution
    // setting: read as Native.)
    std::atomic<int> upscalingQuality{ 1 };
    std::atomic<int> showFps{ 0 };             // frame rate counter in the top left corner
    std::atomic<int> savePage{ 1 };            // which page of three game saves the save list shows (1-10)
    std::atomic<int> shadowQuality{ 0 };       // shadow map size: 0 the original 1024, 1 2048, 2 3072, 3 4096
    std::atomic<int> masterVolume{ 10 };       // 0-10
    std::atomic<int> muteInBackground{ 1 };    // silence while the window isn't focused
    std::atomic<int> mouseSensitivity{ 5 };    // 0-10; 0 turns the mouse camera off
    std::atomic<int> buttonIcons{ 0 };         // controller icons: 0 auto (last controller used), 1 Xbox Series, 2 PS4, 3 PS5, 4 Steam, 5 the game's own (Xbox 360)
};

PcSettings& Settings();
// VSync as it is to be acted on: the setting, except under gamescope (the Steam Deck's compositor,
// Game Mode), where it counts as off whatever the setting says. gamescope shows whole frames at the
// display's pace by itself, and with VSync on its frame limiter, which most people leave on, made
// the game present late and drop frames to catch up: 56-58 fps with frames doubled in the Aerie,
// against 59-60 with VSync off, same spot (2026-11-19). NIER_GAMESCOPE_VSYNC=1 keeps the setting.
bool SettingsVsync();
void SettingsLoad();     // reads settings.ini, then applies environment overrides
void SettingsSave();
// The rate of the game's vertical blank, which is what paces its frames: 60 Hz, or 0 for the
// Uncapped frame rate, where vblanks follow the display or each frame (NIER_GUEST_HZ=<rate>
// overrides, -1 for uncapped; other rates, such as 120, exist only through it, for testing).
int GuestRefreshHz();
// The Frame Rate setting's limit in frames a second for the rates above 60 (90, 120, 144, 240), or 0
// for none (uncapped; 30 and 60 are paced by the fixed vblank instead).
int FrameRateCap();
void SettingsPollPage();   // live values while the PC Settings page is open (called from the audio thread)

// Applying settings that need more than a read at the point of use.
void PresenterSetFullscreen(bool on);   // present.cpp; safe from any thread
void PresenterSetWindowSize(int width, int height);   // the window's inside, when it is a window (0, 0: left as it is); safe from any thread
void PresenterTakeMouseMotion(int& dx, int& dy);   // mouse movement since the last call, in device counts

// Menu pointer (present.cpp): the cursor in the game's 1280x720 screen space and what the mouse did
// since the last call. False when the window isn't focused (or the pointer is outside the picture).
struct MenuMouse { float x = 0, y = 0; bool moved = false; int leftClicks = 0, rightClicks = 0, wheel = 0; };
bool PresenterTakeMenuMouse(MenuMouse& mouse);
