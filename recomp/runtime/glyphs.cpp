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
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
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

int GlyphSetInUse() { return int(CurrentSet()); }

std::string StartPromptWording(bool capital)
{
    const char* button = capital ? " Button" : " button";
    switch (CurrentSet())
    {
    case XboxSeries: case Steam: return std::string("Press the MENU") + button;
    case Ps4: case Ps5: return std::string("Press the OPTIONS") + button;
    case Keyboard:
    {
        const int key = InputGetKey(8);   // the Pause Menu (Start) binding
        if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2) return "Press " + InputKeyName(key);
        return "Press the " + InputKeyName(key) + (capital ? " Key" : " key");
    }
    default: return {};
    }
}

// The title screen's "Press the START Button" is two images of the title layout, both DXT3 with one
// colour and the shape in the alpha: the letters (255x14) and their glow (265x26), centred on the
// same point. Both are replaced by "Press the (Start icon of the icon set in use) Button", drawn at
// twice the size: the letters image gets the letters (in the image's own colour and peak opacity),
// the glow image the letters with their glow and the icon (too tall for the letters image). The
// game's fade and pulse still apply.
constexpr uint32_t kPromptIconHeight = 48, kPromptIconWidth = 110;   // most the icon may take, in the doubled image

// The 256x384 RGBA atlas of an icon set (as Build uses them).
static bool LoadSetAtlas(Set set, std::vector<uint8_t>& rgba)
{
    const uint32_t width = kAtlasWidth * kScale, height = kAtlasHeight * kScale;
    rgba.assign(size_t(width) * height * 4, 0);
    if (set == Keyboard) return DrawKeyboardAtlas(rgba.data(), width, height);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    std::string path = exe;
    path = path.substr(0, path.find_last_of("\\/") + 1) + "glyphs\\" + kSetFiles[set] + ".rgba";
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t header[3] = {};
    const bool ok = fread(header, 4, 3, file) == 3 && memcmp(header, "NGLY", 4) == 0 && header[1] == width && header[2] == height &&
        fread(rgba.data(), 1, rgba.size(), file) == rgba.size();
    fclose(file);
    return ok;
}
constexpr uint32_t kPromptFormat = 19, kPromptWidth = 265, kPromptHeight = 26, kPromptTextWidth = 255, kPromptTextHeight = 14;

static std::shared_ptr<DecodedTexture> TitlePromptFor(const std::shared_ptr<DecodedTexture>& original, bool letters)
{
    if (original->format != GuestTextureFormat::BC2 || original->subresources.empty()) return original;
    const Set set = CurrentSet();
    if (set == Original) return original;   // the game's own icons: its own wording
    const std::string text = set == Keyboard ? "Press the * Key" : "Press the * Button";   // * is the icon
    const uint64_t variant = uint64_t(set + 1) * 0x9E3779B97F4A7C15ull ^ (set == Keyboard ? KeyBindingsStamp() : 0);
    struct Prompt { uint64_t sourceHash = 0, variant = 0; std::shared_ptr<DecodedTexture> texture; };
    static std::mutex mutex;
    static std::map<uint64_t, Prompt> prompts;
    std::lock_guard lock(mutex);
    Prompt& prompt = prompts[original->key];
    if (prompt.texture && prompt.sourceHash == original->contentHash && prompt.variant == variant) return prompt.texture;

    // The image's colour (alpha-weighted mean) and its strongest alpha.
    const DecodedTexture::Subresource& sub = original->subresources[0];
    uint64_t sum[3] = {}, weight = 0;
    uint32_t peak = 0;
    for (uint32_t by = 0; by < sub.rows; by++)
        for (uint32_t bx = 0; bx < sub.rowPitch / 16; bx++)
        {
            const uint8_t* block = original->data.data() + sub.offset + size_t(by) * sub.rowPitch + size_t(bx) * 16;
            const uint16_t c0 = uint16_t(block[8] | block[9] << 8), c1 = uint16_t(block[10] | block[11] << 8);
            uint32_t colors[4][3];
            auto expand = [](uint16_t c, uint32_t* rgb) { rgb[0] = ((c >> 11) & 31) * 255 / 31; rgb[1] = ((c >> 5) & 63) * 255 / 63; rgb[2] = (c & 31) * 255 / 31; };
            expand(c0, colors[0]); expand(c1, colors[1]);
            for (int k = 0; k < 3; k++) { colors[2][k] = (2 * colors[0][k] + colors[1][k]) / 3; colors[3][k] = (colors[0][k] + 2 * colors[1][k]) / 3; }
            const uint32_t bits = uint32_t(block[12]) | block[13] << 8 | block[14] << 16 | uint32_t(block[15]) << 24;
            for (int i = 0; i < 16; i++)
            {
                const uint32_t alpha = ((block[i / 2] >> ((i & 1) * 4)) & 15) * 17;
                const uint32_t* color = colors[(bits >> (2 * i)) & 3];
                for (int k = 0; k < 3; k++) sum[k] += uint64_t(color[k]) * alpha;
                weight += alpha;
                peak = alpha > peak ? alpha : peak;
            }
        }
    if (!weight) return original;

    const uint32_t width = original->width * 2, height = original->height * 2;   // block-padded, like the original
    std::vector<uint8_t> mask(size_t(kPromptWidth * 2) * (kPromptHeight * 2));
    // The Start icon (column 1, row 3 of the atlas): the part of its cell that is drawn on, scaled
    // to fill the prompt's height (a wide, low icon: its width).
    const uint32_t cell = kCell * kScale, atlasWidth = kAtlasWidth * kScale;
    std::vector<uint8_t> atlas;
    if (!LoadSetAtlas(set, atlas)) return original;
    uint32_t x0 = cell, y0 = cell, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < cell; y++)
        for (uint32_t x = 0; x < cell; x++)
            if (atlas[(size_t(3 * cell + y) * atlasWidth + cell + x) * 4 + 3] > 24)
            {
                x0 = x < x0 ? x : x0; y0 = y < y0 ? y : y0;
                x1 = x > x1 ? x : x1; y1 = y > y1 ? y : y1;
            }
    if (x1 < x0) return original;
    const float sourceWidth = float(x1 - x0 + 1), sourceHeight = float(y1 - y0 + 1);
    const float zoom = (std::min)(float(kPromptIconHeight) / sourceHeight, float(kPromptIconWidth) / sourceWidth);
    const uint32_t iconWidth = uint32_t(sourceWidth * zoom + 0.5f), iconHeight = uint32_t(sourceHeight * zoom + 0.5f);
    float iconLeft = 0;
    if (!DrawPromptMask(text, mask.data(), int(kPromptWidth * 2), int(kPromptHeight * 2), 2.0f, !letters, float(iconWidth), &iconLeft)) return original;
    // The letters image is the middle of the glow image's box.
    const uint32_t boxWidth = (letters ? kPromptTextWidth : kPromptWidth) * 2, boxHeight = (letters ? kPromptTextHeight : kPromptHeight) * 2;
    const uint32_t offsetX = (kPromptWidth * 2 - boxWidth) / 2, offsetY = (kPromptHeight * 2 - boxHeight) / 2;
    auto texture = std::make_shared<DecodedTexture>();
    texture->data.assign(size_t(width) * height * 4, 0);
    for (uint32_t y = 0; y < boxHeight && y < height; y++)
        for (uint32_t x = 0; x < boxWidth && x < width; x++)
        {
            uint8_t* texel = &texture->data[(size_t(y) * width + x) * 4];
            for (int k = 0; k < 3; k++) texel[k] = uint8_t(sum[k] / weight);
            texel[3] = uint8_t(mask[size_t(y + offsetY) * kPromptWidth * 2 + x + offsetX] * peak / 255);
        }
    if (!letters)
    {
        // The icon laid over the glow.
        const uint32_t left = uint32_t(iconLeft + 0.5f), top = (kPromptHeight * 2 - iconHeight) / 2;
        for (uint32_t y = 0; y < iconHeight; y++)
            for (uint32_t x = 0; x < iconWidth && left + x < width; x++)
            {
                // Bilinear sample of the cell.
                const float sx = x0 + (x + 0.5f) / zoom - 0.5f, sy = y0 + (y + 0.5f) / zoom - 0.5f;
                const int ix = (std::min)(int(sx < 0 ? 0 : sx), int(cell) - 1), iy = (std::min)(int(sy < 0 ? 0 : sy), int(cell) - 1);
                const int jx = (std::min)(ix + 1, int(cell) - 1), jy = (std::min)(iy + 1, int(cell) - 1);
                const float fx = sx < 0 ? 0 : sx - ix, fy = sy < 0 ? 0 : sy - iy;
                float sample[4] = {};
                const int corners[4][2] = { { ix, iy }, { jx, iy }, { ix, jy }, { jx, jy } };
                const float weights[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
                for (int c = 0; c < 4; c++)
                {
                    const uint8_t* source = &atlas[(size_t(3 * cell + corners[c][1]) * atlasWidth + cell + corners[c][0]) * 4];
                    const float alpha = source[3] / 255.0f * weights[c];
                    for (int k = 0; k < 3; k++) sample[k] += source[k] * alpha;   // premultiplied
                    sample[3] += alpha;
                }
                if (sample[3] <= 0.004f) continue;
                uint8_t* texel = &texture->data[(size_t(top + y) * width + left + x) * 4];
                const float under = texel[3] / 255.0f * (1 - sample[3]), alpha = sample[3] + under;
                for (int k = 0; k < 3; k++) texel[k] = uint8_t((sample[k] + texel[k] * under) / alpha + 0.5f);
                texel[3] = uint8_t((std::min)(alpha, 1.0f) * 255.0f + 0.5f);
            }
    }
    const uint64_t textHash = variant;
    texture->key = original->key ^ 0x7469746C65707274ull ^ uint64_t(letters);
    texture->contentHash = original->contentHash ^ textHash;
    texture->format = GuestTextureFormat::RGBA8;
    texture->width = width;
    texture->height = height;
    texture->rowPitch = width * 4;
    texture->subresources.push_back({ 0, width * 4, height });
    if (!letters) Log("[glyphs] title prompt with the %s Start icon (colour %u %u %u, alpha %u)\n", kSetFiles[set], unsigned(sum[0] / weight), unsigned(sum[1] / weight), unsigned(sum[2] / weight), peak);
    prompt = { original->contentHash, variant, texture };
    return texture;
}

// The title menu's fourth entry, "Downloadable Content", is likewise two DXT3 images (letters
// 213x14, glow 225x26). The entry is Quit to Desktop in this port (pcsettings.cpp), so both are
// redrawn with that wording, in the image's own colour and peak opacity, at twice the size.
constexpr uint32_t kDlcLabelWidth = 225, kDlcLabelHeight = 26, kDlcLabelTextWidth = 213, kDlcLabelTextHeight = 14;

static std::shared_ptr<DecodedTexture> TitleLabelFor(const std::shared_ptr<DecodedTexture>& original, bool letters, const std::string& text,
                                                     uint32_t glowWidth, uint32_t glowHeight, uint32_t textWidth, uint32_t textHeight)
{
    if (original->format != GuestTextureFormat::BC2 || original->subresources.empty()) return original;
    static const bool keep = GetEnvironmentVariableA("NIER_TITLE_DLC", nullptr, 0) > 0;
    if (keep) return original;
    struct Label { uint64_t sourceHash = 0; std::shared_ptr<DecodedTexture> texture; };
    static std::mutex mutex;
    static std::map<uint64_t, Label> labels;
    std::lock_guard lock(mutex);
    Label& label = labels[original->key];
    if (label.texture && label.sourceHash == original->contentHash) return label.texture;

    // The image's colour (alpha-weighted mean) and its strongest alpha.
    const DecodedTexture::Subresource& sub = original->subresources[0];
    uint64_t sum[3] = {}, weight = 0;
    uint32_t peak = 0;
    for (uint32_t by = 0; by < sub.rows; by++)
        for (uint32_t bx = 0; bx < sub.rowPitch / 16; bx++)
        {
            const uint8_t* block = original->data.data() + sub.offset + size_t(by) * sub.rowPitch + size_t(bx) * 16;
            const uint16_t c0 = uint16_t(block[8] | block[9] << 8), c1 = uint16_t(block[10] | block[11] << 8);
            uint32_t colors[4][3];
            auto expand = [](uint16_t c, uint32_t* rgb) { rgb[0] = ((c >> 11) & 31) * 255 / 31; rgb[1] = ((c >> 5) & 63) * 255 / 63; rgb[2] = (c & 31) * 255 / 31; };
            expand(c0, colors[0]); expand(c1, colors[1]);
            for (int k = 0; k < 3; k++) { colors[2][k] = (2 * colors[0][k] + colors[1][k]) / 3; colors[3][k] = (colors[0][k] + 2 * colors[1][k]) / 3; }
            const uint32_t bits = uint32_t(block[12]) | block[13] << 8 | block[14] << 16 | uint32_t(block[15]) << 24;
            for (int i = 0; i < 16; i++)
            {
                const uint32_t alpha = ((block[i / 2] >> ((i & 1) * 4)) & 15) * 17;
                const uint32_t* color = colors[(bits >> (2 * i)) & 3];
                for (int k = 0; k < 3; k++) sum[k] += uint64_t(color[k]) * alpha;
                weight += alpha;
                peak = alpha > peak ? alpha : peak;
            }
        }
    if (!weight) return original;

    const uint32_t width = original->width * 2, height = original->height * 2;   // block-padded, like the original
    std::vector<uint8_t> mask(size_t(glowWidth * 2) * (glowHeight * 2));
    // The letters image is the middle of the glow image's box, and too short for descenders at the
    // usual size: the text is fitted to its rows, so that no part of a letter is left to the glow alone.
    const float band[2] = { float(glowHeight - textHeight), float(glowHeight + textHeight) };
    if (!DrawPromptMask(text, mask.data(), int(glowWidth * 2), int(glowHeight * 2), 2.0f, !letters, 0.0f, nullptr, band)) return original;
    const uint32_t boxWidth = (letters ? textWidth : glowWidth) * 2, boxHeight = (letters ? textHeight : glowHeight) * 2;
    const uint32_t offsetX = (glowWidth * 2 - boxWidth) / 2, offsetY = (glowHeight * 2 - boxHeight) / 2;
    auto texture = std::make_shared<DecodedTexture>();
    texture->data.assign(size_t(width) * height * 4, 0);
    for (uint32_t y = 0; y < boxHeight && y < height; y++)
        for (uint32_t x = 0; x < boxWidth && x < width; x++)
        {
            uint8_t* texel = &texture->data[(size_t(y) * width + x) * 4];
            for (int k = 0; k < 3; k++) texel[k] = uint8_t(sum[k] / weight);
            texel[3] = uint8_t(mask[size_t(y + offsetY) * glowWidth * 2 + x + offsetX] * peak / 255);
        }
    texture->key = original->key ^ 0x717569746C626C00ull ^ uint64_t(letters);
    texture->contentHash = original->contentHash ^ 0x5175697454784474ull;
    texture->format = GuestTextureFormat::RGBA8;
    texture->width = width;
    texture->height = height;
    texture->rowPitch = width * 4;
    texture->subresources.push_back({ 0, width * 4, height });
    if (!letters) Log("[glyphs] title menu: \"%s\" in place of the Downloadable Content label (colour %u %u %u, alpha %u)\n", text.c_str(),
        unsigned(sum[0] / weight), unsigned(sum[1] / weight), unsigned(sum[2] / weight), peak);
    label = { original->contentHash, texture };
    return texture;
}

std::shared_ptr<DecodedTexture> GlyphAtlasFor(const std::shared_ptr<DecodedTexture>& original, uint32_t guestFormat, uint32_t width, uint32_t height)
{
    if (original && guestFormat == kPromptFormat && width == kDlcLabelWidth && height == kDlcLabelHeight)
        return TitleLabelFor(original, false, "Quit to Desktop", kDlcLabelWidth, kDlcLabelHeight, kDlcLabelTextWidth, kDlcLabelTextHeight);
    if (original && guestFormat == kPromptFormat && width == kDlcLabelTextWidth && height == kDlcLabelTextHeight)
        return TitleLabelFor(original, true, "Quit to Desktop", kDlcLabelWidth, kDlcLabelHeight, kDlcLabelTextWidth, kDlcLabelTextHeight);
    if (original && guestFormat == kPromptFormat && width == kPromptWidth && height == kPromptHeight) return TitlePromptFor(original, false);
    if (original && guestFormat == kPromptFormat && width == kPromptTextWidth && height == kPromptTextHeight) return TitlePromptFor(original, true);
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
