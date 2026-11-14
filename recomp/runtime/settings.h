// PC settings: saved in settings.ini beside the executable, edited from the in-game "PC Settings"
// page (pcsettings.cpp). Environment variables still override the file for test runs.
#pragma once
#include <atomic>

struct PcSettings
{
    std::atomic<int> borderless{ 1 };          // display mode: 0 windowed, 1 borderless fullscreen
    std::atomic<int> frameRate60{ 1 };         // 0: the original 30 fps cap, 1: 60 fps
    std::atomic<int> vsync{ 1 };
    std::atomic<int> masterVolume{ 10 };       // 0-10
    std::atomic<int> muteInBackground{ 1 };    // silence while the window isn't focused
};

PcSettings& Settings();
void SettingsLoad();     // reads settings.ini, then applies environment overrides
void SettingsSave();
void SettingsPollPage();   // live values while the PC Settings page is open (called from the audio thread)

// Applying settings that need more than a read at the point of use.
void PresenterSetFullscreen(bool on);   // present.cpp; safe from any thread
