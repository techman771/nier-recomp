// Interface between GPU emulation and a graphics backend. Keep this free of Win32, D3D and Vulkan
// types: each backend (D3D12 now, Vulkan next, possibly Metal) implements exactly these functions.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

struct RendererPlane
{
    const uint8_t* data;
    uint32_t width, height, pitch;
};

struct GuestShader
{
    uint64_t hash = 0;
    std::string hlsl;
    bool pixel = false;
};

enum class GuestTextureFormat { RGBA8, BC1, BC2, BC3, R8 };

struct DecodedTexture
{
    uint64_t key = 0;
    uint32_t width = 0, height = 0, rowPitch = 0;
    uint32_t faces = 1;                         // 6 for a cube map, faces stored one after another
    GuestTextureFormat format = GuestTextureFormat::RGBA8;
    std::vector<uint8_t> data;
};

struct TextureBinding
{
    bool valid = false;
    std::shared_ptr<DecodedTexture> decoded;   // null when the texture is a resolved render target
    uint32_t resolvedBase = 0;
    uint8_t swizzle[4] = { 0, 1, 2, 3 };       // 0-3 channel, 4 zero, 5 one
    bool point = false;
    bool cube = false;                         // the shader samples this slot as a cube map
    float scale[2] = { 1.0f, 1.0f };
};

struct RenderTargetKey
{
    uint32_t edramBase = 0, width = 0, height = 0;
};

struct RendererDraw
{
    std::shared_ptr<GuestShader> vs, ps;
    RenderTargetKey target;
    bool hasColor = true;
    bool depthBound = false;                   // a depth/stencil target is attached
    RenderTargetKey depth;
    uint32_t depthControl = 0, stencilRefMask = 0;
    uint32_t blendControl = 0, writeMask = 0xF;
    bool rectList = false;
    uint32_t vertexCount = 0;
    std::vector<uint32_t> indices;             // empty means non-indexed
    std::vector<uint8_t> vertexData;           // byte order already corrected
    std::array<uint32_t, 96> vertexBase{};
    std::array<float, 2048> constants{};
    std::array<uint32_t, 8> boolConstants{};
    std::array<TextureBinding, 16> textures;
    bool bypassViewport = false;
    float ndcScale[2] = { 1, 1 }, ndcOffset[2] = { 0, 0 };
    float viewport[4] = { 0, 0, 1, 1 };
    int scissor[4] = { 0, 0, 1, 1 };
};

// A copy from EDRAM to memory, optionally followed by clearing the color and depth targets.
struct RendererResolve
{
    RenderTargetKey source;
    bool copyColor = true;
    uint32_t destBase = 0, width = 0, height = 0;   // destination surface size
    int sourceX = 0, sourceY = 0;                   // copied rectangle, in the render target
    int destX = 0, destY = 0;                       // where it lands in the destination
    uint32_t copyWidth = 0, copyHeight = 0;
    bool swapRedBlue = false;
    bool clearColor = false, clearDepth = false;
    uint32_t colorClear = 0, depthClear = 0;
    RenderTargetKey depth;
};

using RendererCommand = std::variant<RendererDraw, RendererResolve>;

struct RendererFrame
{
    uint64_t frame = 0;
    std::vector<RendererCommand> commands;
    uint32_t scanoutBase = 0;                  // resolve destination to display; 0 for none
};

// `nativeWindow` is the platform's window handle.
void RendererInit(void* nativeWindow, uint32_t width, uint32_t height);
void RendererResize(uint32_t width, uint32_t height);

// Called once per guest swap. The planes are copied before returning.
void RendererSubmitMovieFrame(uint64_t frame, const RendererPlane& y, const RendererPlane& u, const RendererPlane& v);
void RendererSubmitFrame(RendererFrame&& frame);

void RendererDumpStats();
