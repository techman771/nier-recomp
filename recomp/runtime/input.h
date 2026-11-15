// Input (input.cpp): which kind of device the player used last (for button glyphs), and the
// rebindable keyboard controls.
#pragma once
#include <cstdint>
#include <string>

enum class InputStyle { Xbox, PS4, PS5, Nintendo, Keyboard, Steam };
InputStyle InputLastStyle();

// Keyboard bindings: one key (a Windows virtual-key code, 0 for none) per pad control.
constexpr int kKeyActions = 24;   // 0-11 buttons, 12-23 movement, camera and d-pad
struct KeyAction { const char* id; const char* label; int defaultKey; };
const KeyAction& InputKeyAction(int index);
int InputGetKey(int index);
void InputSetKey(int index, int key);              // swaps with an action that already had the key
bool InputSetKeyById(const char* id, int key);     // loading settings
void InputResetKeys();
std::string InputKeyName(int key);
void InputSetCapture(bool on);                     // while on, keys don't act as the pad
int InputPollNewKey();                             // the next key pressed during capture, 0 until then

// Menu pointer: while a menu reports itself (every frame it is open), the mouse is a pointer: the
// cursor shows, mouse buttons aren't game controls, and movement doesn't turn the camera.
void InputKeepPointerMode();                       // call each frame a pointer-driven menu is open
bool InputPointerMode();
void InputInject(uint16_t buttons);                // press pad buttons for a few frames (menu clicks)

// An overlay of ours that takes the input (the on-screen keyboard, osk.cpp): while on, the game is
// given an idle pad, and the overlay reads the controllers (and scripted presses) itself.
void InputSetOverlay(bool on);
void InputOverlayRead(uint16_t& buttons, int16_t& lx, int16_t& ly);

// On-screen keyboard (osk.cpp).
bool OskActive();
void OskChar(wchar_t c);                           // a typed character (WM_CHAR)
void OskKey(int virtualKey);                       // arrow keys
void OskClick(int x, int y, int clientWidth, int clientHeight);
bool OskRun(std::wstring& text, const std::wstring& title, const std::wstring& description, size_t maxChars);
