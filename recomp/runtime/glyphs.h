// Button glyph replacement (glyphs.cpp).
#pragma once
#include <cstdint>
#include <memory>

struct DecodedTexture;
// The texture to draw in place of a decoded guest texture: one of our glyph atlases when it is the
// game's button atlas and the device in use calls for another set; otherwise the texture itself.
std::shared_ptr<DecodedTexture> GlyphAtlasFor(const std::shared_ptr<DecodedTexture>& original, uint32_t guestFormat, uint32_t width, uint32_t height);
