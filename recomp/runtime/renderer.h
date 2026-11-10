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

enum class GuestTextureFormat { RGBA8, BC1, BC2, BC3, R8, RGBA16F, RG16F, R32F, RG32F, RGBA32F };

struct DecodedTexture
{
    uint64_t key = 0;           // identity: address, format and shape
    uint64_t contentHash = 0;   // changes when the guest rewrites the texture
    uint32_t width = 0, height = 0, rowPitch = 0;
    uint32_t faces = 1;                         // 6 for a cube map, faces stored one after another
    uint32_t levels = 1;                        // mip levels, the base included
    GuestTextureFormat format = GuestTextureFormat::RGBA8;
    std::vector<uint8_t> data;
    // Where each subresource (face * levels + level, D3D order) starts in `data`, and its row pitch.
    struct Subresource { size_t offset = 0; uint32_t rowPitch = 0, rows = 0; };
    std::vector<Subresource> subresources;
};

struct TextureBinding
{
    bool valid = false;
    std::shared_ptr<DecodedTexture> decoded;   // null when the texture is a resolved render target
    uint32_t resolvedBase = 0;
    uint32_t resolvedLevels[14] = {};          // guest addresses of levels 1.. that are resolved surfaces too
    uint32_t resolvedLevelCount = 0;
    uint8_t swizzle[4] = { 0, 1, 2, 3 };       // 0-3 channel, 4 zero, 5 one
    bool point = false;
    // Address modes per axis, as host sampler modes: 0 clamp, 1 wrap, 2 mirror, 3 mirror once,
    // 4 border (kSamplerAddressModes in renderer_d3d12.cpp).
    uint8_t addressU = 0, addressV = 0;
    bool cube = false;                         // the shader samples this slot as a cube map
    bool baseOnly = false;                     // the fetch constant's mip filter says base map only
    float scale[2] = { 1.0f, 1.0f };
    uint32_t sign = 0;   // 2-bit sign mode per output component, bit 8: 8-bit host data (two's complement applies)
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
    std::array<TextureBinding, 32> textures;   // one per fetch constant
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
    bool copyDepth = false;                         // copies the depth buffer (`depth`) instead
    uint32_t scale = 1;                             // samples per pixel along each axis (2 for 4x MSAA)
    uint32_t destBase = 0, width = 0, height = 0;   // destination surface size
    int sourceX = 0, sourceY = 0;                   // copied rectangle, in the render target
    int destX = 0, destY = 0;                       // where it lands in the destination
    uint32_t copyWidth = 0, copyHeight = 0;
    bool swapRedBlue = false;
    bool writeBack = false;                         // also copy the result into guest memory
    uint32_t destEndian = 0, destFormat = 0;        // RB_COPY_DEST_INFO fields, for the write-back
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
// Called by the renderer (after the frame's GPU work) with a write-back resolve's pixels, RGBA8 rows.
void GpuWriteResolvedPixels(const RendererResolve& resolve, const uint8_t* rgba, uint32_t rowPitch);
void RendererSaveFrame(uint64_t frame);   // save that frame's image to recomp/frames (any thread)
// Starts compiling a newly translated guest shader in the background (any thread).
void RendererPrepareShader(const std::shared_ptr<GuestShader>& shader);
