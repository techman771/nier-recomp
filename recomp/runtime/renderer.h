// Interface between GPU emulation and a graphics backend. Keep this free of Win32, D3D and Vulkan
// types: each backend (D3D12 now, Vulkan next, possibly Metal) implements exactly these functions.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
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
    float expScale = 1.0f;   // 2^exp_adjust from the fetch constant (dword 3 bits 13-18): scales every sampled value
};

struct RenderTargetKey
{
    uint32_t edramBase = 0, width = 0, height = 0;
};

// A draw's vertex bytes. Unlike a std::vector, sizing it doesn't zero-fill: the bytes are written
// straight after (zero-filling cost as much as the copy), and resizing discards what was there.
struct ByteBuffer
{
    std::unique_ptr<uint8_t[]> bytes;
    size_t count = 0;
    ByteBuffer() = default;
    ByteBuffer(ByteBuffer&&) = default;
    ByteBuffer& operator=(ByteBuffer&&) = default;
    ByteBuffer(const ByteBuffer& other) { assign(other.data(), other.count); }
    ByteBuffer& operator=(const ByteBuffer& other) { if (this != &other) assign(other.data(), other.count); return *this; }
    ByteBuffer& operator=(const std::vector<uint8_t>& other) { assign(other.data(), other.size()); return *this; }
    void resize(size_t size) { bytes.reset(size ? new uint8_t[size] : nullptr); count = size; }
    void assign(const uint8_t* from, size_t size) { resize(size); if (size) memcpy(bytes.get(), from, size); }
    uint8_t* data() { return bytes.get(); }
    const uint8_t* data() const { return bytes.get(); }
    size_t size() const { return count; }
    bool empty() const { return count == 0; }
};

struct RendererDraw
{
    // Written out so that making one doesn't zero it first: `constants` (8 KB) is filled straight
    // after, and a draw is made for every draw call of the game.
    RendererDraw() {}
    std::shared_ptr<GuestShader> vs, ps;
    RenderTargetKey target;
    bool hasColor = true;
    bool depthBound = false;                   // a depth/stencil target is attached
    RenderTargetKey depth;
    uint32_t depthControl = 0, stencilRefMask = 0;
    uint32_t blendControl = 0, writeMask = 0xF;
    float blendConstant[4] = { 0, 0, 0, 0 };   // RB_BLEND_RED..ALPHA: what the constant blend factors (12-15) stand for
    // Alpha test (RB_COLORCONTROL, RB_ALPHA_REF): a pixel is kept only if its alpha passes this
    // comparison with the reference. 0 never, 1 less, 2 equal, 3 less or equal, 4 greater, 5 not
    // equal, 6 greater or equal, 7 always (also when the test is off).
    uint8_t alphaFunc = 7;
    float alphaRef = 0.0f;
    // Generated pixel shader parameter (SQ_PROGRAM_CNTL param_gen, SQ_CONTEXT_MISC param_gen_pos): the
    // interpolator the pixel shader receives the pixel position and facing in, in place of what the
    // vertex shader wrote there; 255 for none.
    uint8_t paramGen = 255;
    bool rectList = false;
    // The indices are a triangle strip (0xFFFFFFFF starts a new one), drawn as one; otherwise a list.
    bool strip = false;
    // Face culling (PA_SU_SC_MODE_CNTL): 0 none, 1 front faces, 2 back faces; which winding is the
    // front. Double-sided cloth, fur and hair are modelled as two coincident layers with opposite
    // normals and rely on this to hide the one facing away.
    uint8_t cull = 0;
    bool frontCounterClockwise = true;
    uint32_t vertexCount = 0;
    std::vector<uint32_t> indices;             // empty means non-indexed
    ByteBuffer vertexData;                     // byte order already corrected
    std::array<uint32_t, 96> vertexBase{};
    std::array<float, 2048> constants;         // not initialized (see the constructor)
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
uint32_t RendererDisplayHz();        // the refresh rate of the display the window is on
// Frame timing, summed over the frames rendered since the last call (which starts a new sum): the
// render thread's time (recording the commands, the present, waiting for the GPU to finish the frame)
// and the GPU's time by stage (RendererPerfStageName), all in milliseconds.
enum { kRendererPerfStages = 8 };
struct RendererPerf
{
    uint32_t frames = 0;
    double recordMs = 0, presentMs = 0, gpuWaitMs = 0, stageMs[kRendererPerfStages] = {};
    double slowestMs = 0;      // the longest the render thread took over one frame
    uint32_t renderWidth = 0, renderHeight = 0, windowWidth = 0, windowHeight = 0;
    char upscaler[32] = "";    // what enlarged the picture in the last frame ("-" for nothing)
    // The system's load while the load readout is on: sums over `loadSamples` samples (two a
    // second) of the processor and GPU use in percent, and the most memory this process held, in GB.
    uint32_t loadSamples = 0;
    double cpuPercent = 0, gpuPercent = 0, ramGb = 0, vramGb = 0;
    char adapter[128] = "";
};
RendererPerf RendererPerfTake();
const char* RendererPerfStageName(int stage);
// Whether this system has an FSR 4 upscaler (it comes with the AMD driver, for the GPUs it supports).
bool RendererFsr4Available();
bool RendererWaitDisplayVblank();   // waits one 60 Hz period of the display's own refreshes; false if it can't
bool RendererReady();
// A picture of ours drawn over the game's, alpha-blended, in the largest 16:9 area of the window
// (the on-screen keyboard). The pixels are copied; null removes it.
void RendererSetOverlay(const uint8_t* rgba, uint32_t width, uint32_t height);
// A short line of text on a dark plate, as an RGBA image (osk.cpp, which has the text drawing): the
// frame rate counter's picture.
bool DrawSmallLabel(const std::string& text, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);   // the device exists and frames are accepted
// Running totals in milliseconds: time the render thread waited for shader compiles, time spent
// creating pipelines, time the render thread waited for pipelines.
void RendererWaitTotals(double totals[3]);

// Called once per guest swap. The planes are copied before returning.
void RendererSubmitMovieFrame(uint64_t frame, const RendererPlane& y, const RendererPlane& u, const RendererPlane& v);
void RendererSubmitFrame(RendererFrame&& frame);
// An emptied command list of an earlier frame, to build the next frame in: its memory has been used
// already, where a newly allocated list costs a page fault for every few commands (a draw is about
// 10 KB). Empty with no capacity if none has come back yet.
std::vector<RendererCommand> RendererTakeCommandList();

void RendererDumpStats();
// Called by the renderer (after the frame's GPU work) with a write-back resolve's pixels, RGBA8 rows.
void GpuWriteResolvedPixels(const RendererResolve& resolve, const uint8_t* rgba, uint32_t rowPitch);
void RendererSaveFrame(uint64_t frame);   // save that frame's image to recomp/frames (any thread)
// Starts compiling a newly translated guest shader in the background (any thread).
void RendererPrepareShader(const std::shared_ptr<GuestShader>& shader);
// Starts creating the pipeline a draw will need, ahead of the render thread (any thread).
void RendererPreparePipeline(const RendererDraw& draw);
// Ahead-of-time work from the pipeline list (prewarm.cpp): compiled and created behind whatever the
// game itself asks for. `flags`: bit 0 rectangle list, 1 colour target, 2 depth bound, 3-4 cull,
// 5 counter-clockwise front.
void RendererPrewarmShader(const std::shared_ptr<GuestShader>& shader);
void RendererPrewarmPipeline(const std::shared_ptr<GuestShader>& vs, const std::shared_ptr<GuestShader>& ps, uint32_t blendControl, uint32_t writeMask,
                             uint32_t depthControl, uint32_t stencilRefMask, uint32_t flags);
void RendererPrewarmProgress(uint32_t& done, uint32_t& total);   // pipelines created, of those asked for
void RendererPrewarmBoost(bool on);                              // extra workers while the game waits (PrewarmWait)
// The "Preparing shaders" picture (osk.cpp): 1920x1080 RGBA for RendererSetOverlay.
bool DrawProgressScreen(uint32_t done, uint32_t total, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
