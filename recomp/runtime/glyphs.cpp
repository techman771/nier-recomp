// Button glyphs. The game draws every controller button icon from one atlas (128x192, DXT5, a 4x6
// grid of 32-pixel cells: A B X Y / LT RT LB RB / L R L3 R3 / Back Start D-pad Guide / D-pad right
// left up down / L left-right, L up-down, the game's emblem, empty). When the device used last isn't
// an Xbox 360 pad, the renderer is handed one of our atlases instead (tools/make_glyphs.ps1 draws
// them into glyphs\<set>.rgba beside the executable: 256x384 RGBA, same grid), with the emblem cell
// copied from the game's atlas. NIER_GLYPHS=xbox|ps4|ps5|steam|keyboard|original forces a set.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include "guest.h"
#include "renderer.h"
#include "input.h"
#include "settings.h"
#include "glyphs.h"

namespace
{
    constexpr uint32_t kAtlasWidth = 128, kAtlasHeight = 192, kCell = 32, kScale = 2;
    enum Set { Original = -1, XboxSeries, Ps4, Ps5, Keyboard, Steam, SetCount };
    const char* kSetFiles[SetCount] = { "xbox", "ps4", "ps5", "keyboard", "steam" };

    Set CurrentSet()
    {
        static const int forced = [] {
            char value[16];
            if (!GetEnvironmentVariableA("NIER_GLYPHS", value, sizeof(value))) return -2;
            for (int i = 0; i < SetCount; i++) if (_stricmp(value, kSetFiles[i]) == 0) return i;
            return _stricmp(value, "original") == 0 ? -1 : -2;
        }();
        if (forced != -2) return Set(forced);
        // Keyboard and mouse always get their own icons; PC Settings > Button Icons picks the
        // controller icons (0 is Auto: by the controller used last).
        if (InputLastStyle() == InputStyle::Keyboard) return Keyboard;
        switch (Settings().buttonIcons)
        {
        case 1: return XboxSeries;
        case 2: return Ps4;
        case 3: return Ps5;
        case 4: return Steam;
        case 5: return Original;
        }
        switch (InputLastStyle())
        {
        case InputStyle::PS4: return Ps4;
        case InputStyle::PS5: return Ps5;
        case InputStyle::Keyboard: return Keyboard;
        case InputStyle::Steam: return Steam;
        default: return XboxSeries;
        }
    }

    // One BC3 (DXT5) block to 4x4 RGBA.
    void DecodeBc3(const uint8_t* block, uint8_t out[16][4])
    {
        uint8_t alpha[8] = { block[0], block[1] };
        if (alpha[0] > alpha[1]) for (int i = 1; i < 7; i++) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        else { for (int i = 1; i < 5; i++) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5); alpha[6] = 0; alpha[7] = 255; }
        uint64_t alphaBits = 0;
        for (int i = 0; i < 6; i++) alphaBits |= uint64_t(block[2 + i]) << (8 * i);
        const uint16_t c0 = uint16_t(block[8] | block[9] << 8), c1 = uint16_t(block[10] | block[11] << 8);
        uint8_t colors[4][3];
        auto expand = [](uint16_t c, uint8_t* rgb) { rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31); rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63); rgb[2] = uint8_t((c & 31) * 255 / 31); };
        expand(c0, colors[0]); expand(c1, colors[1]);
        for (int k = 0; k < 3; k++) { colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3); colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3); }
        const uint32_t colorBits = uint32_t(block[12]) | block[13] << 8 | block[14] << 16 | uint32_t(block[15]) << 24;
        for (int i = 0; i < 16; i++)
        {
            memcpy(out[i], colors[(colorBits >> (2 * i)) & 3], 3);
            out[i][3] = alpha[(alphaBits >> (3 * i)) & 7];
        }
    }

    // RGBA pixels of one 32x32 cell of the game's atlas.
    void DecodeCell(const DecodedTexture& atlas, uint32_t column, uint32_t row, uint8_t* rgba)
    {
        const DecodedTexture::Subresource& sub = atlas.subresources[0];
        for (uint32_t by = 0; by < kCell / 4; by++)
            for (uint32_t bx = 0; bx < kCell / 4; bx++)
            {
                uint8_t texels[16][4];
                DecodeBc3(atlas.data.data() + sub.offset + size_t(row * kCell / 4 + by) * sub.rowPitch + size_t(column * kCell / 4 + bx) * 16, texels);
                for (int i = 0; i < 16; i++) memcpy(rgba + ((by * 4 + i / 4) * kCell + bx * 4 + i % 4) * 4, texels[i], 4);
            }
    }

    bool LooksLikeGlyphAtlas(const DecodedTexture& t)
    {
        if (t.format != GuestTextureFormat::BC3 || t.width != kAtlasWidth || t.height != kAtlasHeight || t.subresources.empty())
        {
            Log("[glyphs] candidate rejected: host format %d, %ux%u\n", int(t.format), t.width, t.height);
            return false;
        }
        uint8_t cell[kCell * kCell * 4];
        DecodeCell(t, 3, 5, cell);   // the empty cell
        uint32_t filled = 0;
        for (uint32_t i = 0; i < kCell * kCell; i++) filled += cell[i * 4 + 3] > 32;
        if (filled > 16) { Log("[glyphs] 128x192 texture isn't the atlas: %u texels in the empty cell\n", filled); return false; }
        DecodeCell(t, 0, 0, cell);   // the A button: mostly opaque
        uint32_t opaque = 0;
        for (uint32_t i = 0; i < kCell * kCell; i++) opaque += cell[i * 4 + 3] > 200;
        if (opaque <= kCell * kCell / 3) Log("[glyphs] 128x192 texture isn't the atlas: A cell has %u opaque texels\n", opaque);
        return opaque > kCell * kCell / 3;
    }

    std::mutex gMutex;
    struct Built { uint64_t sourceHash = 0; std::shared_ptr<DecodedTexture> texture; };
    std::map<std::pair<uint64_t, int>, Built> gBuilt;   // (game atlas key, set)

    std::shared_ptr<DecodedTexture> Build(const DecodedTexture& original, Set set)
    {
        const uint32_t width = kAtlasWidth * kScale, height = kAtlasHeight * kScale;
        auto texture = std::make_shared<DecodedTexture>();
        texture->data.resize(size_t(width) * height * 4);
        if (set == Keyboard)
        {
            // Drawn from the current key bindings (keyglyphs.cpp).
            if (!DrawKeyboardAtlas(texture->data.data(), width, height)) return nullptr;
            if (GetEnvironmentVariableA("NIER_DUMP_GLYPHS", nullptr, 0) > 0)   // debugging: the drawn atlas, raw RGBA
                if (FILE* dump = fopen("../captures/keyboard_atlas.rgba", "wb")) { fwrite(texture->data.data(), 1, texture->data.size(), dump); fclose(dump); }
        }
        else
        {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, sizeof(exe));
        std::string path = exe;
        path = path.substr(0, path.find_last_of("\\/") + 1) + "glyphs\\" + kSetFiles[set] + ".rgba";
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) { Log("[glyphs] %s is missing (tools/make_glyphs.ps1 creates it); keeping the game's icons\n", path.c_str()); return nullptr; }
        uint32_t header[3] = {};
        const bool ok = fread(header, 4, 3, file) == 3 && memcmp(header, "NGLY", 4) == 0 && header[1] == width && header[2] == height &&
            fread(texture->data.data(), 1, texture->data.size(), file) == texture->data.size();
        fclose(file);
        if (!ok) { Log("[glyphs] %s isn't a %ux%u glyph atlas\n", path.c_str(), width, height); return nullptr; }

        }

        // The game's emblem (column 2, row 5) stays, scaled up.
        uint8_t cell[kCell * kCell * 4];
        DecodeCell(original, 2, 5, cell);
        for (uint32_t y = 0; y < kCell * kScale; y++)
            for (uint32_t x = 0; x < kCell * kScale; x++)
                memcpy(&texture->data[(size_t(5 * kCell * kScale + y) * width + 2 * kCell * kScale + x) * 4], cell + ((y / kScale) * kCell + x / kScale) * 4, 4);

        texture->key = original.key ^ (0x9E3779B97F4A7C15ull * uint64_t(set + 1));
        texture->contentHash = original.contentHash ^ uint64_t(set + 1) ^ (set == Keyboard ? KeyBindingsStamp() : 0);
        texture->format = GuestTextureFormat::RGBA8;
        texture->width = width;
        texture->height = height;
        texture->rowPitch = width * 4;
        texture->subresources.push_back({ 0, width * 4, height });
        Log("[glyphs] using the %s icons\n", kSetFiles[set]);
        return texture;
    }
}

std::shared_ptr<DecodedTexture> GlyphAtlasFor(const std::shared_ptr<DecodedTexture>& original, uint32_t guestFormat, uint32_t width, uint32_t height)
{
    if (guestFormat != 20 || width != kAtlasWidth || height != kAtlasHeight || !original) return original;
    const Set set = CurrentSet();
    if (set == Original) return original;
    std::lock_guard lock(gMutex);
    static std::map<uint64_t, std::pair<uint64_t, bool>> checked;   // atlas key -> (content hash, is the glyph atlas)
    auto& check = checked[original->key];
    if (check.first != original->contentHash) check = { original->contentHash, LooksLikeGlyphAtlas(*original) };
    if (!check.second) return original;
    Built& built = gBuilt[{ original->key, int(set) }];
    const uint64_t source = original->contentHash ^ (set == Keyboard ? KeyBindingsStamp() : 0);   // rebuilt when a key is rebound
    if (!built.texture || built.sourceHash != source)
    {
        built.sourceHash = source;
        built.texture = Build(*original, set);
    }
    return built.texture ? built.texture : original;
}
