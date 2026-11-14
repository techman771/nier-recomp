// Input (input.cpp): which kind of device the player used last, for button glyphs.
#pragma once

enum class InputStyle { Xbox, PS4, PS5, Nintendo, Keyboard };
InputStyle InputLastStyle();
