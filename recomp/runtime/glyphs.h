// Button glyph replacement (glyphs.cpp).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct DecodedTexture;
// The texture to draw in place of a decoded guest texture: one of our glyph atlases when it is the
// game's button atlas and the device in use calls for another set; otherwise the texture itself.
std::shared_ptr<DecodedTexture> GlyphAtlasFor(const std::shared_ptr<DecodedTexture>& original, uint32_t guestFormat, uint32_t width, uint32_t height);

// The keyboard set (keyglyphs.cpp): drawn from the current key bindings.
bool DrawKeyboardAtlas(uint8_t* rgba, uint32_t width, uint32_t height);
// "Press the START Button" for the device in use ("Press the OPTIONS Button", "Press the Esc Key"...);
// empty when the game's own icons are shown. capital: "Button" / "Key" rather than "button" / "key".
std::string StartPromptWording(bool capital);
// keyglyphs.cpp: the title prompt's wording as white text, with a glow if asked, one byte per pixel;
// scale 2 draws it twice as large, smaller if it doesn't fit. A '*' in the text leaves iconWidth
// pixels free for an icon; *iconLeft is where that room starts.
bool DrawPromptMask(const std::string& text, uint8_t* mask, int width, int height, float scale, bool glow, float iconWidth, float* iconLeft);
int GlyphSetInUse();   // the icons shown now: -1 the game's own (Xbox 360), 0 Xbox Series, 1 PS4, 2 PS5, 3 keyboard, 4 Steam
uint64_t KeyBindingsStamp();   // changes when a key is rebound
