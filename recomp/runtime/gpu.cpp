// Stand-in for the Xenos GPU. It consumes the real PM4 command stream and performs everything the
// CPU side can observe (read pointer, fences, scratch writeback, interrupts, vblank), but draws
// nothing. Replacing the draw handling with a real backend is the later renderer work.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "guest.h"
#include "renderer.h"
#include "xenos_shader.h"

namespace
{
    constexpr uint32_t MMIO_BASE = 0x7FC80000;
    constexpr uint32_t REG_CP_RB_RPTR = 0x1C4, REG_CP_RB_WPTR = 0x1C5;
    constexpr uint32_t REG_SCRATCH_UMSK = 0x1DC, REG_SCRATCH_ADDR = 0x1DD, REG_SCRATCH_REG0 = 0x578;
    constexpr uint32_t REG_VBLANK_STATUS = 0x1951;  // read by the game's vblank interrupt handler
    constexpr uint32_t REG_COUNT = 0x8000;

    enum Pm4 : uint32_t
    {
        PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_WAIT_FOR_IDLE = 0x26, PM4_DRAW_INDX_2 = 0x36,
        PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_WAIT_REG_MEM = 0x3C, PM4_MEM_WRITE = 0x3D, PM4_REG_TO_MEM = 0x3E,
        PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46, PM4_INTERRUPT = 0x54,
        PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B, PM4_XE_SWAP = 0x64,
    };

    uint32_t gRingBase = 0, gRingDwords = 0, gWritebackAddress = 0;
    std::atomic<uint32_t> gInterruptCallback{ 0 }, gInterruptUser{ 0 };
    uint32_t gRegisters[REG_COUNT];
    uint32_t gReadIndex = 0;
    PPCContext* gGpuContext = nullptr;
    uint32_t gGpuPcr = 0;

    std::atomic<uint64_t> gFrames{ 0 }, gDraws{ 0 }, gIndirect{ 0 }, gFences{ 0 }, gCpInterrupts{ 0 }, gVblanks{ 0 };
    std::mutex gStatsMutex;
    std::map<uint32_t, uint64_t> gUnknownOpcodes;
    int gWaitTimeoutsLogged = 0, gCorruptLogged = 0;
    std::atomic<bool> gRunning{ false };

    struct Stream
    {
        uint32_t base, dwords, read, end;
        uint32_t Available() const { return (end + dwords - read) % dwords; }
        uint32_t Next()
        {
            uint32_t value = R32(base + read * 4);
            read = (read + 1) % dwords;
            return value;
        }
        void Skip(uint32_t count) { read = (read + count) % dwords; }
    };

    uint32_t Swap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value & 0x00FF00FF) << 8) | ((value & 0xFF00FF00) >> 8);
        case 2: return __builtin_bswap32(value);
        case 3: return (value >> 16) | (value << 16);
        default: return value;
        }
    }

    // GPU addresses carry their byte order in the low two bits. Values are stored in host order
    // after the swap, so endian 2 ends up as big-endian guest memory.
    void GpuStore(uint32_t address, uint32_t value)
    {
        uint32_t guest = PhysicalToGuest(address & ~3u);
        *(volatile uint32_t*)(g_base + guest) = Swap(value, address & 3);
    }

    uint32_t GpuLoad(uint32_t address)
    {
        uint32_t guest = PhysicalToGuest(address & ~3u);
        return Swap(*(volatile uint32_t*)(g_base + guest), address & 3);
    }

    void DispatchInterrupt(PPCContext& ctx, uint32_t pcr, uint32_t source, uint32_t cpu)
    {
        uint32_t callback = gInterruptCallback.load();
        if (!callback) return;
        W8(pcr + 0x10C, uint8_t(cpu));
        uint64_t stack = ctx.r1.u64;
        ctx.r3.u64 = source;
        ctx.r4.u64 = gInterruptUser.load();
        ctx.lr = 0;
        CallGuestFunction(ctx, callback);
        ctx.r1.u64 = stack;
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= REG_COUNT) return;
        if ((index == 0x2307 || index == 0x2308) && gRegisters[index] != value)
        {
            static int changes = 0;   // SQ_VS_CONST / SQ_PS_CONST: where each stage's constants start
            if (changes++ < 16) Log("[gpu] %s = %08X\n", index == 0x2307 ? "SQ_VS_CONST" : "SQ_PS_CONST", value);
        }
        gRegisters[index] = value;
        if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
        {
            uint32_t slot = index - REG_SCRATCH_REG0;
            if ((1u << slot) & gRegisters[REG_SCRATCH_UMSK])
                W32(PhysicalToGuest(gRegisters[REG_SCRATCH_ADDR] + slot * 4), value);
        }
    }

    bool Compare(uint32_t function, uint32_t value, uint32_t reference)
    {
        switch (function & 7)
        {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value <= reference;
        case 3: return value == reference;
        case 4: return value != reference;
        case 5: return value >= reference;
        case 6: return value > reference;
        default: return true;
        }
    }

    uint32_t PollValue(uint32_t waitInfo, uint32_t poll)
    {
        return (waitInfo & 0x10) ? GpuLoad(poll) : (poll < REG_COUNT ? gRegisters[poll] : 0);
    }

    constexpr uint32_t REG_FETCH_CONSTANTS = 0x4800;
    int gFrameDrawsLogged = 0;
    std::vector<RendererCommand> gFrameCommands;
    uint32_t gScanoutBase = 0;

    struct TextureDescriptor
    {
        uint32_t base, format, width, height, pitch, endian;
        bool tiled, packedMips;
        uint32_t dimension;   // 0 1D, 1 2D or stacked, 2 3D, 3 cube
        uint32_t mipAddress;  // physical address of levels 1 and up (0 when there are none)
        uint32_t maxLevel;
    };

    // Texture fetch constants are 6 dwords per slot; type 2 marks a texture.
    bool ReadTextureSlot(uint32_t slot, TextureDescriptor& out)
    {
        const uint32_t* c = &gRegisters[REG_FETCH_CONSTANTS + slot * 6];
        if ((c[0] & 3) != 2) return false;
        out.base = c[1] & 0xFFFFF000;
        if (!out.base) return false;
        out.format = c[1] & 0x3F;
        out.endian = (c[1] >> 6) & 3;
        out.dimension = (c[5] >> 9) & 3;
        if (out.dimension == 0)
        {
            out.width = (c[2] & 0xFFFFFF) + 1;   // 1D: a 24-bit width and no height
            out.height = 1;
        }
        else
        {
            out.width = (c[2] & 0x1FFF) + 1;
            out.height = ((c[2] >> 13) & 0x1FFF) + 1;
        }
        out.pitch = ((c[0] >> 22) & 0x1FF) << 5;
        out.tiled = (c[0] >> 31) & 1;
        out.packedMips = (c[5] >> 11) & 1;
        out.mipAddress = c[5] & 0xFFFFF000;
        out.maxLevel = (c[4] >> 6) & 0xF;
        return true;
    }

    // Movies are drawn as a fullscreen quad over three 8-bit planes: luma plus two half-size chroma.
    struct MovieSource { TextureDescriptor y, u, v; bool valid = false; };
    MovieSource gMovie;
    uint64_t gLogDrawsAt = ~0ull;
    std::atomic<bool> gCaptureRequested{ false };
    uint64_t gCaptureFrame = ~0ull;

    constexpr uint32_t PRIM_QUAD_LIST = 13, FORMAT_8 = 2;

    void CaptureMovieSource(uint32_t initiator)
    {
        if ((initiator & 0x3F) != PRIM_QUAD_LIST) return;
        MovieSource source;
        if (ReadTextureSlot(0, source.y) && ReadTextureSlot(1, source.u) && ReadTextureSlot(2, source.v) &&
            source.y.format == FORMAT_8 && source.u.format == FORMAT_8 && source.v.format == FORMAT_8 && !source.y.tiled && !source.u.tiled && !source.v.tiled)
        {
            source.valid = true;
            gMovie = source;
        }
    }

    RendererPlane PlaneFrom(const TextureDescriptor& t)
    {
        return { g_base + PhysicalToGuest(t.base), t.width, t.height, t.pitch };
    }

    // Measures how evenly new movie images arrive, independent of the 60Hz swap rate.
    uint64_t gMovieImages = 0, gMovieLateImages = 0, gMovieSwaps = 0;
    double gMovieWorstGapMs = 0, gMovieTotalGapMs = 0;
    uint64_t gMovieLastHash = 0, gMovieLastSwap = 0;
    uint64_t gMovieGapBins[8];  // new-image gaps, in units of one 60Hz refresh
    LARGE_INTEGER gMovieLastChange{};

    void MeasureMovieFrame(const MovieSource& m)
    {
        uint64_t hash = 1469598103934665603ull;
        const uint8_t* plane = g_base + PhysicalToGuest(m.y.base);
        for (uint32_t row = 0; row < m.y.height; row += 16)
            for (uint32_t x = 0; x < m.y.width; x += 16) hash = (hash ^ plane[row * m.y.pitch + x]) * 1099511628211ull;

        gMovieSwaps++;
        if (hash == gMovieLastHash) return;
        gMovieLastHash = hash;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        uint64_t frame = gFrames.load();
        if (gMovieLastChange.QuadPart && frame - gMovieLastSwap < 30)
        {
            double gap = double(now.QuadPart - gMovieLastChange.QuadPart) * 1000.0 / frequency.QuadPart;
            gMovieTotalGapMs += gap;
            gMovieImages++;
            gMovieGapBins[std::min<int>(int(gap / 16.683 + 0.5), 7)]++;
            if (gap > gMovieWorstGapMs) gMovieWorstGapMs = gap;
            if (gap > 60.0) gMovieLateImages++;
        }
        gMovieLastChange = now;
        gMovieLastSwap = frame;
    }

    // Until real rendering exists, only movie frames have a picture to show.
    void PresentCurrentFrame(uint64_t frame)
    {
        if (frame % 300 == 0)
        {
            Log("[movie] frame %llu: source %s", frame, gMovie.valid ? "captured" : "none");
            if (gMovie.valid)
            {
                const TextureDescriptor* planes[3] = { &gMovie.y, &gMovie.u, &gMovie.v };
                for (auto* p : planes)
                {
                    const uint8_t* data = g_base + PhysicalToGuest(p->base);
                    uint64_t sum = 0;
                    uint32_t low = 255, high = 0, total = 0;
                    for (uint32_t row = 0; row < p->height; row += 8)
                        for (uint32_t x = 0; x < p->width; x += 8, total++)
                        {
                            uint32_t v = data[row * p->pitch + x];
                            sum += v; low = v < low ? v : low; high = v > high ? v : high;
                        }
                    Log(" | %08X %ux%u min %u max %u mean %llu", p->base, p->width, p->height, low, high, sum / total);
                }
            }
            Log("\n");
        }
        if (gMovie.valid)
        {
            MeasureMovieFrame(gMovie);
            RendererSubmitMovieFrame(frame, PlaneFrom(gMovie.y), PlaneFrom(gMovie.u), PlaneFrom(gMovie.v));
            gMovie.valid = false;
            gFrameCommands.clear();
            return;
        }
        RendererFrame output;
        output.frame = frame;
        output.commands = std::move(gFrameCommands);
        output.scanoutBase = gScanoutBase;
        gFrameCommands.clear();
        RendererSubmitFrame(std::move(output));
    }

    // Shader microcode as last uploaded by the game, kept by stage (0 vertex, 1 pixel).
    struct ShaderCapture { std::vector<uint32_t> words; uint64_t hash = 0; };
    ShaderCapture gShaders[2];
    std::map<uint64_t, std::vector<uint32_t>> gShaderLibrary[2];

    void StoreShader(uint32_t stage, std::vector<uint32_t> words)
    {
        if (stage > 1) return;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t w : words) hash = (hash ^ w) * 1099511628211ull;
        gShaders[stage].hash = hash;
        gShaders[stage].words = words;
        if (!gShaderLibrary[stage].count(hash) && words.size() > 24 && !words[0] && !words[1] && !words[2])
        {
            // Real programs never start with zeros. This happened when D3D read microcode through a
            // physical-memory window that didn't share storage with the one it was written through.
            static int warned = 0;
            if (warned++ < 4) Log("[gpu] %s shader %016llX starts with zeros; its microcode was probably read from the wrong memory\n", stage ? "pixel" : "vertex", hash);
        }
        if (!gShaderLibrary[stage].count(hash))
        {
            CreateDirectoryA("..\\shaders", nullptr);
            char path[96];
            snprintf(path, sizeof(path), "..\\shaders\\%s_%016llX.bin", stage ? "ps" : "vs", hash);
            if (FILE* file = fopen(path, "wb")) { fwrite(words.data(), 4, words.size(), file); fclose(file); }
            gShaderLibrary[stage][hash] = std::move(words);
        }
    }

    struct CachedShader { std::shared_ptr<GuestShader> shader; TranslatedShader info; };
    std::unordered_map<uint64_t, CachedShader> gTranslated[2];
    std::set<uint32_t> gResolvedBases;
    std::unordered_map<uint64_t, std::shared_ptr<DecodedTexture>> gDecodedTextures;
    std::atomic<uint64_t> gSkippedDraws{ 0 }, gSubmittedDraws{ 0 }, gResolves{ 0 };
    std::map<std::string, uint64_t> gSkipReasons;

    void SkipDraw(const std::string& reason)
    {
        gSkippedDraws++;
        std::lock_guard lock(gStatsMutex);
        gSkipReasons[reason]++;
    }

    CachedShader* ShaderFor(uint32_t stage)
    {
        ShaderCapture& capture = gShaders[stage];
        if (capture.words.empty()) return nullptr;
        auto found = gTranslated[stage].find(capture.hash);
        if (found != gTranslated[stage].end()) return &found->second;

        CachedShader entry;
        entry.info = TranslateShader(capture.words, stage == 1, capture.hash);
        if (!entry.info.ok) Log("[shader] %s %016llX not translated: %s\n", stage ? "pixel" : "vertex", capture.hash, entry.info.error.c_str());
        auto shader = std::make_shared<GuestShader>();
        shader->hash = capture.hash;
        shader->hlsl = entry.info.hlsl;
        shader->pixel = stage == 1;
        entry.shader = shader;
        char path[96];
        snprintf(path, sizeof(path), "..\\shaders\\%s_%016llX.hlsl", stage ? "ps" : "vs", capture.hash);
        if (FILE* file = fopen(path, "wb")) { fwrite(entry.info.hlsl.data(), 1, entry.info.hlsl.size(), file); fclose(file); }
        return &(gTranslated[stage][capture.hash] = std::move(entry));
    }

    // Xenos tiling: maps a block position to its index in the tiled surface.
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t logBytes)
    {
        const uint32_t aligned = (width + 31) & ~31u;
        const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (logBytes + 7);
        const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBytes;
        const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBytes)) + ((y & 1) << 4);
        return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) + ((((y & 8) >> 2) + (x >> 3)) & 3) * 64) >> logBytes;
    }

    void CopySwapped(uint8_t* out, const uint8_t* in, uint32_t bytes, uint32_t endian)
    {
        memcpy(out, in, bytes);
        if (endian == 1) for (uint32_t i = 0; i + 2 <= bytes; i += 2) std::swap(out[i], out[i + 1]);
        else if (endian == 2) for (uint32_t i = 0; i + 4 <= bytes; i += 4) { std::swap(out[i], out[i + 3]); std::swap(out[i + 1], out[i + 2]); }
        else if (endian == 3) for (uint32_t i = 0; i + 4 <= bytes; i += 4) { std::swap(out[i], out[i + 2]); std::swap(out[i + 1], out[i + 3]); }
    }

    std::map<uint32_t, uint64_t> gSubstitutedFormats;

    // Stands in for formats not decoded yet (mostly depth textures used for shadows): opaque white,
    // which reads as "lit" to a shadow lookup, rather than skipping the whole draw.
    std::shared_ptr<DecodedTexture> SubstituteTexture(uint32_t format)
    {
        static const std::shared_ptr<DecodedTexture> white = [] {
            auto texture = std::make_shared<DecodedTexture>();
            texture->key = 0xFFFFFFFFFFFF0001ull;
            texture->width = texture->height = 4;
            texture->rowPitch = 16;
            texture->data.assign(64, 255);
            texture->subresources.push_back({ 0, 16, 4 });
            return texture;
        }();
        std::lock_guard lock(gStatsMutex);
        if (gSubstitutedFormats[format]++ == 0) Log("[gpu] texture format %u not decoded; substituting white (frame %llu)\n", format, GpuFrameCount());
        return white;
    }

    // CTX1 (format 60): a two-channel DXT-style block, as decoded by Xenia's texture_load_ctx1.
    // Dword 0 holds the endpoints (r0 in bits 8-15, g0 in 0-7, r1 in 24-31, g1 in 16-23); dword 1 holds
    // a 2-bit index per texel, row by row from the low bits: 0 = endpoint 0, 1 = endpoint 1, 2 and 3
    // = the points a third and two thirds of the way. Output is RGBA8 as (x, y, y, y), matching
    // Xenia's RGGG swizzle for the R8G8 host format.
    void DecodeCtx1Block(const uint8_t* block, uint8_t* out, uint32_t rowPitch)
    {
        uint32_t ends, indices;
        memcpy(&ends, block, 4);
        memcpy(&indices, block + 4, 4);
        const uint32_t r0 = (ends >> 8) & 0xFF, g0 = ends & 0xFF, r1 = ends >> 24, g1 = (ends >> 16) & 0xFF;
        static const uint32_t kWeight1[4] = { 0, 3, 1, 2 };   // thirds of endpoint 1 for each index
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
            {
                const uint32_t w1 = kWeight1[(indices >> ((y * 4 + x) * 2)) & 3], w0 = 3 - w1;
                const uint8_t r = uint8_t((r0 * w0 + r1 * w1) / 3), g = uint8_t((g0 * w0 + g1 * w1) / 3);
                uint8_t* texel = out + size_t(y) * rowPitch + x * 4;
                texel[0] = r; texel[1] = g; texel[2] = g; texel[3] = g;
            }
    }

    std::shared_ptr<DecodedTexture> DecodeTexture(const TextureDescriptor& t, TextureBinding& binding)
    {
        GuestTextureFormat format;
        uint32_t blockSize = 1, blockBytes = 4, logBytes = 2;
        bool ctx1 = false;   // decoded on the CPU into RGBA8 rather than uploaded as blocks
        switch (t.format)
        {
        case 2: format = GuestTextureFormat::R8; blockBytes = 1; logBytes = 0; break;
        case 6: format = GuestTextureFormat::RGBA8; break;
        case 18: format = GuestTextureFormat::BC1; blockSize = 4; blockBytes = 8; logBytes = 3; break;
        case 19: format = GuestTextureFormat::BC2; blockSize = 4; blockBytes = 16; logBytes = 4; break;
        case 20: format = GuestTextureFormat::BC3; blockSize = 4; blockBytes = 16; logBytes = 4; break;
        case 29: format = GuestTextureFormat::RGBA16F; blockBytes = 8; logBytes = 3; break;   // k_16_16_16_16_EXPAND
        case 60: format = GuestTextureFormat::RGBA8; blockSize = 4; blockBytes = 8; logBytes = 3; ctx1 = true; break;   // k_CTX1
        case 31: format = GuestTextureFormat::RG16F; break;                                    // k_16_16_FLOAT
        case 32: format = GuestTextureFormat::RGBA16F; blockBytes = 8; logBytes = 3; break;   // k_16_16_16_16_FLOAT
        case 36: format = GuestTextureFormat::R32F; break;                                     // k_32_FLOAT
        case 37: format = GuestTextureFormat::RG32F; blockBytes = 8; logBytes = 3; break;     // k_32_32_FLOAT
        case 38: format = GuestTextureFormat::RGBA32F; blockBytes = 16; logBytes = 4; break;  // k_32_32_32_32_FLOAT
        default:
            return SubstituteTexture(t.format);
        }
        const uint32_t widthBlocks = (t.width + blockSize - 1) / blockSize, heightBlocks = (t.height + blockSize - 1) / blockSize;
        const uint32_t pitchBlocks = std::max(t.pitch / blockSize, widthBlocks);
        const uint32_t guest = PhysicalToGuest(t.base);
        const uint32_t tiledHeight = (heightBlocks + 31) & ~31u;
        const uint64_t faceBytes = uint64_t(pitchBlocks) * (t.tiled ? tiledHeight : heightBlocks) * blockBytes;
        // Cube faces follow each other, each padded to 32-block-aligned rows and a 4 KB boundary.
        const uint32_t faces = t.dimension == 3 ? 6 : 1;
        const uint64_t faceStride = faces > 1 ? ((uint64_t(((pitchBlocks + 31) & ~31u)) * tiledHeight * blockBytes + 4095) & ~4095ull) : faceBytes;
        const uint64_t bytes = faceStride * (faces - 1) + faceBytes;
        if (faceBytes == 0 || uint64_t(guest) + bytes > 0x100000000ull) return nullptr;

        // Mip levels (Xenia texture_util.cc GetGuestTextureLayout). Level L >= 1 lives in the mip
        // area; its pitch and height come from the power-of-two base size shifted down, padded to
        // 32x32 blocks, and each level (all faces) is padded to 4 KB. From the "packed" level on
        // (shorter side 16 texels or less), the remaining levels share that level's tile at fixed
        // offsets.
        const auto logCeil = [](uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; };
        const auto logFloor = [](uint32_t v) { uint32_t n = 0; while ((2u << n) <= v) n++; return n; };
        const auto alignUp = [](uint64_t v, uint64_t a) { return (v + a - 1) / a * a; };
        const auto packedOffset = [&](uint32_t level, uint32_t& x, uint32_t& y) {
            // Xenia GetPackedMipOffset for 2D textures, in blocks; false when the level isn't packed.
            const uint32_t lw = logCeil(t.width), lh = logCeil(t.height), ls = std::min(lw, lh);
            x = y = 0;
            if (!t.packedMips || ls > 4 + level) return false;
            const uint32_t packedBase = ls > 4 ? ls - 4 : 0, packed = level - packedBase;
            if (packed < 3)
            {
                if (lw > lh) y = 16 >> packed; else x = 16 >> packed;
            }
            else
            {
                if (lw > lh) x = (1u << (lw - packedBase)) >> (packed - 2);
                else y = (1u << (lh - packedBase)) >> (packed - 2);
            }
            x /= blockSize;
            y /= blockSize;
            return true;
        };
        const uint32_t mipPage = t.mipAddress >> 12;
        uint32_t levels = 1;
        if (mipPage && t.tiled && t.dimension != 0)
            levels = std::min(t.maxLevel, logFloor(std::max(t.width, t.height))) + 1;
        const uint32_t packedLevel = t.packedMips ? (std::min(logCeil(t.width), logCeil(t.height)) > 4 ? std::min(logCeil(t.width), logCeil(t.height)) - 4 : 0) : 32;
        const uint32_t pow2Width = 1u << logCeil(t.width), pow2Height = 1u << logCeil(t.height);
        struct GuestLevel { uint64_t offset = 0; uint32_t pitchBlocks = 0, rows = 0; uint64_t faceStride = 0; uint32_t packX = 0, packY = 0; };
        std::vector<GuestLevel> guestLevels(levels);
        uint64_t mipBytes = 0;
        const auto storageLayout = [&](uint32_t shift, GuestLevel& g) {
            const uint32_t levelWidth = std::max(pow2Width >> shift, 1u), levelHeight = std::max(pow2Height >> shift, 1u);
            g.pitchBlocks = uint32_t(alignUp((levelWidth + blockSize - 1) / blockSize, 32));
            g.rows = uint32_t(alignUp((levelHeight + blockSize - 1) / blockSize, 32));
            g.faceStride = alignUp(uint64_t(g.pitchBlocks) * g.rows * blockBytes, 4096);
        };
        if (levels > 1 && packedLevel == 0)
        {
            // The base is already in a packed tile, so every mip shares one tile at the mip address,
            // laid out with the base size's pitch.
            GuestLevel tail;
            storageLayout(0, tail);
            for (uint32_t level = 1; level < levels; level++)
            {
                guestLevels[level] = tail;
                packedOffset(level, guestLevels[level].packX, guestLevels[level].packY);
            }
            mipBytes = tail.faceStride * faces;
        }
        else if (levels > 1)
        {
            // Levels 1 up to the packed level each have their own storage, one after another; the
            // packed level's tile also holds every smaller level.
            uint64_t offset = 0;
            const uint32_t lastStored = std::min(levels - 1, packedLevel);
            for (uint32_t level = 1; level <= lastStored; level++)
            {
                storageLayout(level, guestLevels[level]);
                guestLevels[level].offset = offset;
                if (level == packedLevel) packedOffset(level, guestLevels[level].packX, guestLevels[level].packY);
                offset += guestLevels[level].faceStride * faces;
            }
            for (uint32_t level = lastStored + 1; level < levels; level++)
            {
                guestLevels[level] = guestLevels[lastStored];
                packedOffset(level, guestLevels[level].packX, guestLevels[level].packY);
            }
            mipBytes = offset;
        }
        const uint32_t mipGuest = mipPage ? PhysicalToGuest(t.mipAddress) : 0;
        if (levels > 1 && uint64_t(mipGuest) + mipBytes > 0x100000000ull) levels = 1;

        // A texture is identified by where it lives and its shape; its contents can change (bone
        // matrices are rewritten every frame), so they're tracked by a separate hash and re-uploaded
        // into the same host texture rather than creating a new one each time.
        const uint64_t key = (uint64_t(t.base) << 20) ^ (uint64_t(t.format) << 8) ^ (uint64_t(t.width) << 44) ^ (uint64_t(t.height) << 30) ^ (t.tiled ? 1 : 0) ^ (uint64_t(faces) << 1) ^ (uint64_t(levels) << 58) ^ (uint64_t(t.mipAddress) << 4);
        const uint8_t* source = g_base + guest;
        // Hashing the contents on every use dominated the command thread. Small textures (bone
        // matrices, rewritten between draws) are still hashed every time; larger ones once per frame,
        // since the game streams them in between frames.
        struct HashMemo { uint64_t frame = ~0ull, hash = 0; };
        static std::unordered_map<uint64_t, HashMemo> hashMemo;
        static const bool hashEveryUse = GetEnvironmentVariableA("NIER_HASH_EVERY_USE", nullptr, 0) > 0;   // debugging
        const bool large = !hashEveryUse && bytes + (levels > 1 ? mipBytes : 0) > 64 * 1024;
        HashMemo* memo = large ? &hashMemo[key] : nullptr;
        uint64_t content = 1469598103934665603ull;
        if (memo && memo->frame == gFrames.load()) content = memo->hash;
        else if (!large)
        {
            // Every word: a bone matrix rewritten in place may change only some of a texel's
            // components, which sampling one word in four would miss (stale bones for one mesh).
            for (uint64_t i = 0; i + 8 <= bytes; i += 8) content = (content ^ *(const uint64_t*)(source + i)) * 1099511628211ull;
            if (levels > 1)
                for (uint64_t i = 0; i + 8 <= mipBytes; i += 8) content = (content ^ *(const uint64_t*)(g_base + mipGuest + i)) * 1099511628211ull;
        }
        else
        {
            for (uint64_t i = 0; i < bytes; i += 16) content = (content ^ *(const uint32_t*)(source + (i & ~3ull))) * 1099511628211ull;
            if (levels > 1)
                for (uint64_t i = 0; i < mipBytes; i += 64) content = (content ^ *(const uint32_t*)(g_base + mipGuest + (i & ~3ull))) * 1099511628211ull;
            if (memo) *memo = { gFrames.load(), content };
        }

        binding.scale[0] = float(t.width) / float(widthBlocks * blockSize);
        binding.scale[1] = float(t.height) / float(heightBlocks * blockSize);
        auto found = gDecodedTextures.find(key);
        if (found != gDecodedTextures.end() && found->second->contentHash == content) return found->second;

        auto decoded = std::make_shared<DecodedTexture>();
        decoded->key = key;
        decoded->contentHash = content;
        decoded->format = format;
        decoded->width = widthBlocks * blockSize;
        decoded->height = heightBlocks * blockSize;
        decoded->rowPitch = ctx1 ? widthBlocks * 4 * 4 : widthBlocks * blockBytes;
        decoded->faces = faces;
        decoded->levels = levels;

        // Host layout: for each face, each level's rows (CTX1 expands to RGBA8 texel rows).
        size_t total = 0;
        decoded->subresources.resize(size_t(faces) * levels);
        for (uint32_t face = 0; face < faces; face++)
            for (uint32_t level = 0; level < levels; level++)
            {
                const uint32_t w = std::max(decoded->width >> level, 1u), h = std::max(decoded->height >> level, 1u);
                const uint32_t wb = (w + blockSize - 1) / blockSize, hb = (h + blockSize - 1) / blockSize;
                DecodedTexture::Subresource& sub = decoded->subresources[size_t(face) * levels + level];
                sub.offset = total;
                sub.rowPitch = ctx1 ? wb * 16 : wb * blockBytes;
                sub.rows = ctx1 ? hb * 4 : hb;
                total += size_t(sub.rowPitch) * sub.rows;
            }
        decoded->data.resize(total);

        for (uint32_t face = 0; face < faces; face++)
            for (uint32_t level = 0; level < levels; level++)
            {
                const DecodedTexture::Subresource& sub = decoded->subresources[size_t(face) * levels + level];
                const uint32_t w = std::max(decoded->width >> level, 1u), h = std::max(decoded->height >> level, 1u);
                const uint32_t wb = (w + blockSize - 1) / blockSize, hb = (h + blockSize - 1) / blockSize;
                const uint8_t* levelSource;
                uint32_t levelPitch, packX = 0, packY = 0;
                if (level == 0)
                {
                    levelSource = source + faceStride * face;
                    levelPitch = pitchBlocks;
                    if (t.tiled) packedOffset(0, packX, packY);
                }
                else
                {
                    const GuestLevel& g = guestLevels[level];
                    levelSource = g_base + mipGuest + g.offset + g.faceStride * face;
                    levelPitch = g.pitchBlocks;
                    packX = g.packX;
                    packY = g.packY;
                }
                for (uint32_t y = 0; y < hb; y++)
                {
                    uint8_t* row = decoded->data.data() + sub.offset + size_t(y) * sub.rowPitch * (ctx1 ? 4 : 1);
                    for (uint32_t x = 0; x < wb; x++)
                    {
                        const uint32_t index = t.tiled ? TiledOffset(x + packX, y + packY, levelPitch, logBytes) : y * levelPitch + x;
                        if (ctx1)
                        {
                            uint8_t block[8];
                            CopySwapped(block, levelSource + uint64_t(index) * blockBytes, 8, t.endian);
                            DecodeCtx1Block(block, row + size_t(x) * 16, sub.rowPitch);
                        }
                        else CopySwapped(row + size_t(x) * blockBytes, levelSource + uint64_t(index) * blockBytes, blockBytes, t.endian);
                    }
                }
            }
        gDecodedTextures[key] = decoded;
        return decoded;
    }

    // EDRAM records no surface height, so a surface extends over all the tiles (80x16 samples each,
    // 2048 in total) after its base. Every draw to the same base and width then shares one surface.
    RenderTargetKey TargetFor(uint32_t base, uint32_t widthSamples)
    {
        RenderTargetKey key;
        key.edramBase = base;
        key.width = widthSamples;
        const uint32_t tilesPerRow = std::max(1u, widthSamples / 80);
        const uint32_t freeTiles = base < 2048 ? 2048 - base : 1;
        key.height = std::clamp(freeTiles / tilesPerRow * 16, 16u, 4096u);
        return key;
    }

    RenderTargetKey CurrentTarget(uint32_t& scale)
    {
        const uint32_t surface = gRegisters[0x2000], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
        scale = msaa == 2 ? 2 : 1;
        return TargetFor(gRegisters[0x2001] & 0xFFF, pitch * scale);
    }

    // PA_SC_WINDOW_OFFSET: two signed 15-bit values. Predicated tiling renders the screen in strips,
    // shifting each strip into EDRAM with this offset.
    void WindowOffset(int& x, int& y)
    {
        const uint32_t value = gRegisters[0x2080];
        x = int32_t(value << 17) >> 17;
        y = int32_t((value >> 16) << 17) >> 17;
    }

    void BuildResolve()
    {
        const uint32_t control = gRegisters[0x2318];
        uint32_t scale;
        RendererResolve resolve;
        resolve.source = CurrentTarget(scale);
        resolve.copyColor = (control & 7) == 0;     // color target 0; 1-3 (other targets) aren't used
        resolve.copyDepth = (control & 7) == 4;
        resolve.scale = scale;
        resolve.clearColor = (control >> 8) & 1;
        resolve.clearDepth = (control >> 9) & 1;
        resolve.colorClear = gRegisters[0x231E];
        resolve.depthClear = gRegisters[0x231D];
        resolve.depth = TargetFor(gRegisters[0x2002] & 0xFFF, resolve.source.width);
        static int clearsLogged = 0;
        if ((resolve.clearColor || resolve.clearDepth) && clearsLogged++ < 6)
            Log("[gpu] copy with clear: control %08X regs 231C..231F = %08X %08X %08X %08X\n", control, gRegisters[0x231C], gRegisters[0x231D], gRegisters[0x231E], gRegisters[0x231F]);
        if (!resolve.copyColor && !resolve.copyDepth)
        {
            if (resolve.clearColor || resolve.clearDepth) gFrameCommands.emplace_back(std::move(resolve));
            return;
        }
        resolve.destBase = gRegisters[0x2319] & 0x1FFFFFFF;
        resolve.width = gRegisters[0x231A] & 0x3FFF;
        resolve.height = (gRegisters[0x231A] >> 16) & 0x3FFF;
        resolve.swapRedBlue = resolve.copyColor && ((gRegisters[0x231B] >> 24) & 1);
        // Small colour resolves are also written back to guest memory: the game reads its luminance
        // reduction on the CPU to set exposure, and full-screen write-backs would cost too much.
        static const int writeBackMode = [] {
            char value[16];
            if (!GetEnvironmentVariableA("NIER_RESOLVE_WRITEBACK", value, sizeof(value))) return 0;
            return _stricmp(value, "all") == 0 ? 1 : _stricmp(value, "none") == 0 ? 2 : 0;
        }();
        resolve.destEndian = gRegisters[0x231B] & 7;
        resolve.destFormat = (gRegisters[0x231B] >> 7) & 0x3F;
        resolve.writeBack = resolve.copyColor && writeBackMode != 2 && (writeBackMode == 1 || resolve.width * resolve.height <= 64 * 64);

        // The copy is issued as a rect list whose corners give the copied rectangle in screen space.
        // Reading the EDRAM side subtracts the window offset; the destination uses screen space.
        float minX = 0, minY = 0, maxX = float(resolve.width), maxY = float(resolve.height);
        const uint32_t fetch0 = gRegisters[REG_FETCH_CONSTANTS], fetch1 = gRegisters[REG_FETCH_CONSTANTS + 1];
        if ((fetch0 & 3) == 3 && ((fetch1 >> 2) & 0xFFFFFF) >= 6)
        {
            const uint32_t vertices = PhysicalToGuest(fetch0 & ~3u);
            minX = minY = 1e30f;
            maxX = maxY = -1e30f;
            for (int i = 0; i < 3; i++)
            {
                const uint32_t xBits = R32(vertices + i * 8), yBits = R32(vertices + i * 8 + 4);
                const float x = reinterpret_cast<const float&>(xBits), y = reinterpret_cast<const float&>(yBits);
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
        int offsetX, offsetY;
        WindowOffset(offsetX, offsetY);
        resolve.destX = std::max(0, int(minX));
        resolve.destY = std::max(0, int(minY));
        resolve.copyWidth = uint32_t(std::max(0, int(maxX) - resolve.destX));
        resolve.copyHeight = uint32_t(std::max(0, int(maxY) - resolve.destY));
        resolve.sourceX = (resolve.destX + offsetX) * int(scale);
        resolve.sourceY = (resolve.destY + offsetY) * int(scale);
        static int copiesLogged = 0;
        if ((offsetX || offsetY) && copiesLogged++ < 4)
            Log("[gpu] tiled copy: window offset %d,%d rect %d,%d %ux%u -> 0x%08X\n", offsetX, offsetY, resolve.destX, resolve.destY, resolve.copyWidth, resolve.copyHeight, resolve.destBase);
        gResolvedBases.insert(resolve.destBase);
        if (resolve.copyColor) gScanoutBase = resolve.destBase;
        gResolves++;
        gFrameCommands.emplace_back(std::move(resolve));
    }

    struct IndexSource { uint32_t base = 0, size = 0; std::vector<uint32_t> immediate; };
    constexpr uint32_t kRestart = 0xFFFFFFFF;

    // The vertex indices a draw consumes, before primitive assembly: generated for auto-indexed
    // draws, otherwise read from the guest and byte-swapped. Restart markers become kRestart.
    bool ReadIndices(uint32_t initiator, const IndexSource& source, std::vector<uint32_t>& out)
    {
        const uint32_t* r = gRegisters;
        const uint32_t select = (initiator >> 6) & 3, count = initiator >> 16;
        const bool is32 = (initiator >> 11) & 1;
        const bool restartEnabled = (r[0x2205] >> 21) & 1;
        const uint32_t restartValue = r[0x2103], offset = r[0x2102];
        out.resize(count);
        if (select == 2)
        {
            for (uint32_t i = 0; i < count; i++) out[i] = i + offset;
            return true;
        }
        std::vector<uint8_t> raw;
        if (select == 0)
        {
            const uint32_t bytes = count * (is32 ? 4 : 2);
            if (!source.base || bytes > 0x4000000) return false;
            raw.resize(bytes + 4);
            CopySwapped(raw.data(), g_base + PhysicalToGuest(source.base), bytes, source.size >> 30);
        }
        else if (select == 1)
        {
            raw.resize(source.immediate.size() * 4 + 4);
            memcpy(raw.data(), source.immediate.data(), source.immediate.size() * 4);
            if (raw.size() < size_t(count) * (is32 ? 4 : 2)) return false;
        }
        else return false;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t value = is32 ? *(const uint32_t*)(raw.data() + i * 4) : *(const uint16_t*)(raw.data() + i * 2);
            const uint32_t restart = restartValue & (is32 ? 0xFFFFFFFFu : 0xFFFFu);
            out[i] = (restartEnabled && value == restart) ? kRestart : value + offset;
        }
        return true;
    }

    // Converts the guest primitive type to a triangle list (rect lists stay as triples for the GS).
    bool AssemblePrimitives(uint32_t primitive, const std::vector<uint32_t>& in, RendererDraw& draw)
    {
        auto& out = draw.indices;
        auto forEachSegment = [&](auto&& emit) {
            size_t segmentStart = 0;
            for (size_t i = 0; i <= in.size(); i++)
            {
                if (i == in.size() || in[i] == kRestart)
                {
                    if (i > segmentStart) emit(segmentStart, i);
                    segmentStart = i + 1;
                }
            }
        };
        switch (primitive)
        {
        case 4: case 8:
            for (size_t i = 0; i + 3 <= in.size(); i += 3)
                if (in[i] != kRestart && in[i + 1] != kRestart && in[i + 2] != kRestart) out.insert(out.end(), { in[i], in[i + 1], in[i + 2] });
            draw.rectList = primitive == 8;
            return true;
        case 5:
            forEachSegment([&](size_t begin, size_t end) {
                for (size_t i = begin + 2; i < end; i++) out.insert(out.end(), { in[begin], in[i - 1], in[i] });
            });
            return true;
        case 6:
            forEachSegment([&](size_t begin, size_t end) {
                for (size_t i = begin + 2; i < end; i++)
                {
                    const bool odd = (i - begin) & 1;
                    out.insert(out.end(), { in[odd ? i - 1 : i - 2], in[odd ? i - 2 : i - 1], in[i] });
                }
            });
            return true;
        case 13:
            for (size_t i = 0; i + 4 <= in.size(); i += 4) out.insert(out.end(), { in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3] });
            return true;
        default:
            return false;
        }
    }

    void BuildDraw(uint32_t initiator, const IndexSource& source)
    {
        const uint32_t* r = gRegisters;
        const uint32_t mode = r[0x2208] & 7;
        if (mode == 6) { BuildResolve(); return; }
        if (mode != 4 && mode != 5) { SkipDraw("EDRAM mode " + std::to_string(mode)); return; }
        const bool hasColor = mode == 4;
        if (hasColor && ((r[0x2001] >> 16) & 0xF)) { SkipDraw("render target format " + std::to_string((r[0x2001] >> 16) & 0xF)); return; }

        CachedShader* vs = ShaderFor(0), * ps = ShaderFor(1);
        if (!vs || !ps) { SkipDraw("no shader loaded"); return; }
        if (!vs->info.ok || !ps->info.ok) { SkipDraw("untranslated shader"); return; }
        // NIER_SKIP_SHADER=<hash>[,<hash>...] drops draws using those shaders (debugging).
        static const std::set<uint64_t> skipShaders = [] {
            std::set<uint64_t> hashes;
            char value[512];
            if (GetEnvironmentVariableA("NIER_SKIP_SHADER", value, sizeof(value)))
                for (char* item = strtok(value, ","); item; item = strtok(nullptr, ",")) hashes.insert(strtoull(item, nullptr, 16));
            return hashes;
        }();
        if (!skipShaders.empty() && (skipShaders.count(vs->shader->hash) || skipShaders.count(ps->shader->hash))) return;

        RendererDraw draw;
        uint32_t scale;
        draw.target = CurrentTarget(scale);
        draw.vs = vs->shader;
        draw.ps = ps->shader;
        draw.blendControl = r[0x2201];
        draw.writeMask = r[0x2104] & 0xF;

        draw.hasColor = hasColor;
        draw.depthControl = r[0x2200];
        draw.stencilRefMask = r[0x210D];
        static const bool depthDisabled = GetEnvironmentVariableA("NIER_NO_DEPTH", nullptr, 0) > 0;  // debugging
        if ((draw.depthControl & 3) && !depthDisabled)   // stencil or depth test enabled
        {
            draw.depthBound = true;
            draw.depth = TargetFor(r[0x2002] & 0xFFF, draw.target.width);
        }
        if (!hasColor && !draw.depthBound) { SkipDraw("depth pass with depth disabled"); return; }

        const uint32_t primitive = initiator & 0x3F;
        std::vector<uint32_t> sequence;
        if (!ReadIndices(initiator, source, sequence)) { SkipDraw("unreadable index buffer"); return; }
        if (primitive == 1)
        {
            static int logged = 0;   // point lists: what they are and how big (debugging)
            if (logged++ < 12)
                Log("[gpu] point list f%llu: %zu points vs %016llX ps %016llX point size %08X minmax %08X program %08X misc %08X vte %08X target %X\n",
                    GpuFrameCount(), sequence.size(), vs->shader->hash, ps->shader->hash, r[0x2280], r[0x2281], r[0x2180], r[0x2181], r[0x2206], draw.target.edramBase);
        }
        if (!AssemblePrimitives(primitive, sequence, draw)) { SkipDraw("primitive type " + std::to_string(primitive)); return; }
        if (draw.indices.empty()) return;
        draw.vertexCount = uint32_t(draw.indices.size());

        // Vertex buffers: when the shader addresses vertices with the plain vertex index, only the
        // vertices between the draw's smallest and largest index are copied (a fetch constant often
        // covers a whole level's geometry). The base is then shifted so index * stride still lands
        // on the copied bytes (unsigned wrap-around is fine).
        uint32_t minIndex = 0xFFFFFFFF, maxIndex = 0;
        static const bool noTrim = GetEnvironmentVariableA("NIER_NO_VERTEX_TRIM", nullptr, 0) > 0;   // debugging
        if (vs->info.directVertexIndex && !noTrim)
            for (uint32_t index : draw.indices) { minIndex = std::min(minIndex, index); maxIndex = std::max(maxIndex, index); }
        struct VertexCopy { uint32_t slot, first, bytes, endian; const uint8_t* source; };
        VertexCopy copies[96];
        uint32_t copyCount = 0, totalBytes = 0;
        for (uint32_t slot : vs->info.vertexSlots)
        {
            const uint32_t d0 = r[REG_FETCH_CONSTANTS + slot * 2], d1 = r[REG_FETCH_CONSTANTS + slot * 2 + 1];
            if ((d0 & 3) != 3) { SkipDraw("vertex fetch constant not set"); return; }
            const uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
            if (bytes > 0x1000000) { SkipDraw("oversized vertex buffer"); return; }
            uint32_t first = 0, last = bytes;
            auto use = vs->info.vertexUse.find(slot);
            if (vs->info.directVertexIndex && minIndex <= maxIndex && use != vs->info.vertexUse.end() && use->second.stride)
            {
                const uint64_t from = uint64_t(minIndex) * use->second.stride, to = uint64_t(maxIndex) * use->second.stride + use->second.end;
                if (from < bytes)
                {
                    first = uint32_t(from) & ~3u;
                    last = uint32_t(std::min<uint64_t>(to, bytes));
                }
            }
            copies[copyCount++] = { slot, first, last - first, d1 & 3, g_base + PhysicalToGuest(d0 & ~3u) };
            totalBytes += last - first;
        }
        draw.vertexData.resize(totalBytes);
        uint32_t written = 0;
        for (uint32_t i = 0; i < copyCount; i++)
        {
            const VertexCopy& copy = copies[i];
            CopySwapped(draw.vertexData.data() + written, copy.source + copy.first, copy.bytes, copy.endian);
            draw.vertexBase[copy.slot] = written - copy.first;
            written += copy.bytes;
        }

        memcpy(draw.constants.data(), &r[0x4000], draw.constants.size() * 4);
        memcpy(draw.boolConstants.data(), &r[0x4900], draw.boolConstants.size() * 4);

        std::set<uint32_t> textureSlots = ps->info.textureSlots, cubeSlots = ps->info.cubeSlots;
        textureSlots.insert(vs->info.textureSlots.begin(), vs->info.textureSlots.end());
        cubeSlots.insert(vs->info.cubeSlots.begin(), vs->info.cubeSlots.end());
        for (uint32_t slot : textureSlots)
        {
            TextureBinding& binding = draw.textures[slot];
            binding.cube = cubeSlots.count(slot) != 0;   // set even when unbound, so the empty view matches
            TextureDescriptor t;
            if (!ReadTextureSlot(slot, t)) continue;
            const uint32_t* c = &r[REG_FETCH_CONSTANTS + slot * 6];
            const uint32_t swizzle = (c[3] >> 1) & 0xFFF;
            for (int i = 0; i < 4; i++) binding.swizzle[i] = (swizzle >> (i * 3)) & 7;
            binding.point = ((c[3] >> 19) & 3) == 0;
            if (t.height == 1)
            {
                static int logged = 0;   // one-row textures (bone matrices): how they are filtered
                if (logged++ < 2) Log("[gpu] one-row texture slot %u dimension %u format %u width %u: mag filter %u min filter %u mip filter %u, dword3 %08X\n", slot, t.dimension, t.format, t.width, (c[3] >> 19) & 3, (c[3] >> 21) & 3, (c[3] >> 23) & 3, c[3]);
            }
            binding.baseOnly = ((c[3] >> 23) & 3) == 2;   // mip filter: base map only
            // Sign modes belong to the texture's components; the view's swizzle has already moved them.
            const uint32_t signs = (c[0] >> 2) & 0xFF;
            binding.sign = 0;
            for (int i = 0; i < 4; i++)
                if (binding.swizzle[i] < 4) binding.sign |= ((signs >> (binding.swizzle[i] * 2)) & 3) << (i * 2);
            const bool eightBit = t.format == 2 || t.format == 6 || (t.format >= 18 && t.format <= 20) || t.format == 60;
            if (binding.sign && eightBit) binding.sign |= 256;
            if (signs)
            {
                static std::set<uint32_t> seen;
                if (seen.size() < 16 && seen.insert(t.format << 8 | signs).second)
                    Log("[gpu] texture format %u uses sign modes x%u y%u z%u w%u\n", t.format, signs & 3, (signs >> 2) & 3, (signs >> 4) & 3, (signs >> 6) & 3);
            }
            if (binding.cube != (t.dimension == 3))
            {
                static int mismatches = 0;
                if (mismatches++ < 8) Log("[gpu] texture slot %u: shader samples %s, fetch constant dimension %u\n", slot, binding.cube ? "a cube" : "2D", t.dimension);
            }
            if (gResolvedBases.count(t.base))
            {
                binding.resolvedBase = t.base;
                // Levels written by resolves (the luminance reduction renders each step into the next
                // mip level). Layout as in DecodeTexture for 32-bit texels without packed levels:
                // each level padded to 32x32-texel tiles and 4 KB, one after another.
                if (t.mipAddress && t.tiled && t.dimension == 1 && !t.packedMips && t.maxLevel)
                {
                    uint32_t logW = 0, logH = 0;
                    while ((1u << logW) < t.width) logW++;
                    while ((1u << logH) < t.height) logH++;
                    uint32_t largest = std::max(t.width, t.height), logLargest = 0;
                    while ((2u << logLargest) <= largest) logLargest++;
                    const uint32_t levels = std::min({ t.maxLevel, logLargest, 14u });
                    uint64_t offset = 0;
                    for (uint32_t level = 1; level <= levels; level++)
                    {
                        const uint32_t address = t.mipAddress + uint32_t(offset);
                        if (!gResolvedBases.count(address)) break;
                        binding.resolvedLevels[binding.resolvedLevelCount++] = address;
                        const uint64_t pitch = (std::max(1u << logW >> level, 1u) + 31) & ~31u, rows = (std::max(1u << logH >> level, 1u) + 31) & ~31u;
                        offset += (pitch * rows * 4 + 4095) & ~4095ull;
                    }
                    static std::set<uint32_t> logged;
                    if (binding.resolvedLevelCount && logged.size() < 8 && logged.insert(t.base).second)
                        Log("[gpu] resolved mip chain at %08X: %u resolved levels after the base\n", t.base, binding.resolvedLevelCount);
                }
            }
            else
            {
                binding.decoded = DecodeTexture(t, binding);
                if (!binding.decoded) continue;
            }
            binding.valid = true;
        }

        // NIER_DUMP_DRAW=<vertex shader hash>: writes the next draws with that shader (from the
        // -LogDrawsAt frame on) to captures/: byte-swapped vertex data, indices, vertex constants and
        // the texture in slot 16 (bone matrices), for offline analysis (debugging).
        {
            static std::map<uint64_t, int> dumpShaders = [] {   // hash -> draws dumped so far
                std::map<uint64_t, int> hashes;
                char value[512];
                if (GetEnvironmentVariableA("NIER_DUMP_DRAW", value, sizeof(value)))
                    for (char* item = strtok(value, ","); item; item = strtok(nullptr, ",")) hashes[strtoull(item, nullptr, 16)] = 0;
                return hashes;
            }();
            auto dumpEntry = dumpShaders.find(vs->shader->hash);
            if (dumpEntry != dumpShaders.end() && gLogDrawsAt && gFrames.load() >= gLogDrawsAt && dumpEntry->second < 3)
            {
                CreateDirectoryA("..\\captures", nullptr);
                char path[128];
                snprintf(path, sizeof(path), "..\\captures\\draw_%016llX_%d.bin", (unsigned long long)vs->shader->hash, dumpEntry->second++);
                if (FILE* file = fopen(path, "wb"))
                {
                    // Header: counts, then each block; slot 95's base within the vertex data.
                    const uint32_t header[6] = { uint32_t(draw.vertexData.size()), uint32_t(draw.indices.size()), uint32_t(draw.constants.size()),
                                                 draw.textures[16].decoded ? uint32_t(draw.textures[16].decoded->data.size()) : 0u, draw.vertexBase[95],
                                                 draw.textures[16].decoded ? draw.textures[16].decoded->width : 0u };
                    fwrite(header, 4, 6, file);
                    fwrite(draw.vertexData.data(), 1, draw.vertexData.size(), file);
                    fwrite(draw.indices.data(), 4, draw.indices.size(), file);
                    fwrite(draw.constants.data(), 4, draw.constants.size(), file);
                    if (draw.textures[16].decoded) fwrite(draw.textures[16].decoded->data.data(), 1, draw.textures[16].decoded->data.size(), file);
                    fclose(file);
                    Log("[gpu] dumped draw to %s (frame %llu, t16 fetch constant dword 0 %08X)\n", path, gFrames.load(), gRegisters[REG_FETCH_CONSTANTS + 16 * 6]);
                }
            }
        }

        // A disabled viewport means the shader already outputs window coordinates.
        const uint32_t vte = r[0x2206];
        draw.bypassViewport = (vte & 0x3F) == 0;
        const float xScale = reinterpret_cast<const float&>(r[0x210F]), xOffset = reinterpret_cast<const float&>(r[0x2110]);
        const float yScale = reinterpret_cast<const float&>(r[0x2111]), yOffset = reinterpret_cast<const float&>(r[0x2112]);
        if (draw.bypassViewport)
        {
            draw.ndcScale[0] = 2.0f * scale / draw.target.width;
            draw.ndcScale[1] = -2.0f * scale / draw.target.height;
            draw.ndcOffset[0] = -1.0f;
            draw.ndcOffset[1] = 1.0f;
            draw.viewport[0] = 0;
            draw.viewport[1] = 0;
            draw.viewport[2] = float(draw.target.width);
            draw.viewport[3] = float(draw.target.height);
        }
        else
        {
            draw.viewport[0] = (xOffset - std::fabs(xScale)) * scale;
            draw.viewport[1] = (yOffset - std::fabs(yScale)) * scale;
            draw.viewport[2] = std::fabs(xScale) * 2.0f * scale;
            draw.viewport[3] = std::fabs(yScale) * 2.0f * scale;
        }
        int offsetX, offsetY;
        WindowOffset(offsetX, offsetY);
        if ((r[0x2205] >> 16) & 1)   // PA_SU_SC_MODE_CNTL: window offset applies to vertices
        {
            draw.viewport[0] += float(offsetX * int(scale));
            draw.viewport[1] += float(offsetY * int(scale));
        }
        const uint32_t topLeft = r[0x2081], bottomRight = r[0x2082];
        const int scissorShiftX = (topLeft >> 31) ? 0 : offsetX, scissorShiftY = (topLeft >> 31) ? 0 : offsetY;
        auto clampTo = [](int value, uint32_t limit) { return std::clamp(value, 0, int(limit)); };
        draw.scissor[0] = clampTo((int(topLeft & 0x3FFF) + scissorShiftX) * int(scale), draw.target.width);
        draw.scissor[1] = clampTo((int((topLeft >> 16) & 0x3FFF) + scissorShiftY) * int(scale), draw.target.height);
        draw.scissor[2] = clampTo((int(bottomRight & 0x3FFF) + scissorShiftX) * int(scale), draw.target.width);
        draw.scissor[3] = clampTo((int((bottomRight >> 16) & 0x3FFF) + scissorShiftY) * int(scale), draw.target.height);

        gSubmittedDraws++;
        // Debugging: vertex data and state of depth-only and rect draws in the logged frame.
        if (gFrames.load() == gLogDrawsAt)
        {
            static std::set<std::string> seen;
            char state[320];
            snprintf(state, sizeof(state), "[draw f%llu state] color %d prim %u idx %zu target %X %ux%u vp %.0f,%.0f %.0fx%.0f sc %d,%d-%d,%d win %08X mode %08X tl %08X br %08X clip %08X",
                gFrames.load(), hasColor, primitive, draw.indices.size(), draw.target.edramBase, draw.target.width, draw.target.height,
                draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], draw.scissor[0], draw.scissor[1], draw.scissor[2], draw.scissor[3],
                r[0x2080], r[0x2205], r[0x2081], r[0x2082], r[0x2204]);
            std::string key(state);
            key = key.substr(key.find(" target"));
            if (seen.insert(key).second && seen.size() < 60) Log("%s\n", state);
        }
        if (gFrames.load() == gLogDrawsAt && (!hasColor || draw.rectList))
        {
            const float* v = reinterpret_cast<const float*>(draw.vertexData.data());
            const size_t floats = std::min<size_t>(draw.vertexData.size() / 4, 24);
            std::string values;
            char item[32];
            for (size_t i = 0; i < floats; i++) { snprintf(item, sizeof(item), " %.3f", v[i]); values += item; }
            Log("[draw f%llu detail] color %d rect %d depthctl %08X zscale %08X zoff %08X target %ux%u depth base %X vp %.0f,%.0f %.0fx%.0f verts:%s\n",
                gFrames.load(), hasColor, draw.rectList, draw.depthControl, r[0x2113], r[0x2114], draw.target.width, draw.target.height, draw.depth.edramBase,
                draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], values.c_str());
        }
        gFrameCommands.emplace_back(std::move(draw));
    }

    void OnDraw(uint32_t initiator, const IndexSource& source)
    {
        CaptureMovieSource(initiator);
        BuildDraw(initiator, source);
        uint64_t frame = gFrames.load();
        if (frame != gLogDrawsAt) return;
        if (gFrameDrawsLogged++ > 5000) return;
        const uint32_t* r = gRegisters;
        char line[2048];
        int used = snprintf(line, sizeof(line), "[draw f%llu] prim %u src %u count %u mode %u surf %08X col %08X dep %08X blend %08X cctl %08X dctl %08X vte %08X vp %08X/%08X %08X/%08X sciss %08X-%08X prog %08X vs %016llX ps %016llX vf0 %08X %08X copy %08X->%08X p%08X info %08X:",
            frame, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, r[0x2208] & 7, r[0x2000], r[0x2001], r[0x2002], r[0x2201], r[0x2204], r[0x2200], r[0x2206],
            r[0x210F], r[0x2110], r[0x2111], r[0x2112], r[0x2081], r[0x2082], r[0x2180], gShaders[0].hash, gShaders[1].hash, r[0x4800], r[0x4801], r[0x2318], r[0x2319], r[0x231A], r[0x231B]);
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            TextureDescriptor t;
            if (ReadTextureSlot(slot, t) && used < 1900)
                used += snprintf(line + used, sizeof(line) - used, " [%u fmt %u %ux%u %s base %08X p%u e%u]", slot, t.format, t.width, t.height, t.tiled ? "tiled" : "linear", t.base, t.pitch, t.endian);
        }
        Log("%s\n", line);
    }

    void ExecuteStream(Stream stream, int depth);

    void ExecuteType3(Stream& s, uint32_t opcode, uint32_t count, int depth)
    {
        Stream after = s;
        after.Skip(count);

        switch (opcode)
        {
        case PM4_INDIRECT_BUFFER:
        case PM4_INDIRECT_BUFFER_PFD:
        {
            uint32_t address = s.Next() & ~3u;
            uint32_t length = s.Next() & 0xFFFFF;
            gIndirect++;
            if (depth < 8 && length != 0)
                ExecuteStream({ PhysicalToGuest(address), length + 1, 0, length }, depth + 1);
            break;
        }
        case PM4_MEM_WRITE:
        {
            uint32_t address = s.Next();
            for (uint32_t i = 1; i < count; i++)
            {
                GpuStore((address & ~3u) + (i - 1) * 4 | (address & 3), s.Next());
            }
            break;
        }
        case PM4_EVENT_WRITE_SHD:
        {
            uint32_t initiator = s.Next(), address = s.Next(), value = s.Next();
            GpuStore(address, (initiator >> 31) ? uint32_t(gFrames.load()) : value);
            gFences++;
            break;
        }
        case PM4_COND_WRITE:
        {
            uint32_t waitInfo = s.Next(), poll = s.Next(), reference = s.Next(), mask = s.Next(), target = s.Next(), data = s.Next();
            if (Compare(waitInfo, PollValue(waitInfo, poll) & mask, reference))
            {
                if (waitInfo & 0x100) GpuStore(target, data);
                else WriteRegister(target, data);
                gFences++;
            }
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t waitInfo = s.Next(), poll = s.Next(), reference = s.Next(), mask = s.Next();
            for (DWORD start = GetTickCount(); !Compare(waitInfo, PollValue(waitInfo, poll) & mask, reference); SwitchToThread())
            {
                if (GetTickCount() - start > 200)
                {
                    if (gWaitTimeoutsLogged++ < 8) Log("[gpu] WAIT_REG_MEM gave up: info 0x%X poll 0x%X ref 0x%X mask 0x%X\n", waitInfo, poll, reference, mask);
                    break;
                }
            }
            break;
        }
        case PM4_REG_RMW:
        {
            uint32_t info = s.Next(), andValue = s.Next(), orValue = s.Next();
            uint32_t index = info & 0x1FFF;
            if (index < REG_COUNT) WriteRegister(index, (gRegisters[index] & andValue) | orValue);
            break;
        }
        case PM4_REG_TO_MEM:
        {
            uint32_t index = s.Next(), address = s.Next();
            GpuStore(address, index < REG_COUNT ? gRegisters[index] : 0);
            break;
        }
        case PM4_INTERRUPT:
        {
            uint32_t mask = s.Next();
            gCpInterrupts++;
            for (uint32_t cpu = 0; cpu < 6; cpu++)
                if (mask & (1u << cpu)) DispatchInterrupt(*gGpuContext, gGpuPcr, 1, cpu);
            break;
        }
        case PM4_XE_SWAP:
        {
            uint64_t frame = gFrames.fetch_add(1) + 1;
            gFrameDrawsLogged = 0;
            if (frame == gCaptureFrame + 1)
            {
                LogSetCaptureFile(nullptr);
                Log("[capture] frame %llu captured\n", gCaptureFrame);
                gCaptureFrame = ~0ull;
            }
            if (gCaptureRequested.exchange(false))
            {
                // The frame starting now is logged draw by draw and its image saved.
                gCaptureFrame = frame;
                gLogDrawsAt = frame;
                CreateDirectoryA("..\\captures", nullptr);
                char path[96];
                snprintf(path, sizeof(path), "..\\captures\\capture_f%llu.log", frame);
                LogSetCaptureFile(fopen(path, "w"));
                RendererSaveFrame(frame);
                Log("[capture] capturing frame %llu\n", frame);
            }
            PresentCurrentFrame(frame);
            if (frame <= 3 || frame % 600 == 0)
                Log("[gpu] frame %llu: %llu draws, %llu indirect buffers, %llu fences, %llu CP interrupts, %llu vblanks\n", frame, gDraws.load(), gIndirect.load(), gFences.load(), gCpInterrupts.load(), gVblanks.load());
            break;
        }
        case 0x27:  // IM_LOAD: shader microcode loaded from memory
        {
            uint32_t addressType = s.Next();
            uint32_t startSize = s.Next();
            uint32_t guest = PhysicalToGuest(addressType & ~3u);
            std::vector<uint32_t> words(startSize & 0xFFFF);
            for (size_t i = 0; i < words.size(); i++) words[i] = R32(guest + uint32_t(i) * 4);
            StoreShader(addressType & 3, std::move(words));
            break;
        }
        case 0x2B:  // IM_LOAD_IMMEDIATE: shader microcode inside the packet
        {
            uint32_t stage = s.Next();
            uint32_t startSize = s.Next();
            std::vector<uint32_t> words(startSize & 0xFFFF);
            for (auto& word : words) word = s.Next();
            StoreShader(stage & 3, std::move(words));
            break;
        }
        case 0x2D:  // SET_CONSTANT: the packet's type selects which register bank it targets
        {
            uint32_t offsetType = s.Next();
            static const uint32_t kBanks[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t bank = (offsetType >> 16) & 0xFF;
            if (bank < 5)
            {
                uint32_t index = (offsetType & 0x7FF) + kBanks[bank];
                for (uint32_t i = 1; i < count; i++) WriteRegister(index + i - 1, s.Next());
            }
            break;
        }
        case 0x55:  // SET_CONSTANT2
        case 0x56:  // SET_SHADER_CONSTANTS
        {
            uint32_t index = s.Next() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++) WriteRegister(index + i - 1, s.Next());
            break;
        }
        case 0x2F:  // LOAD_ALU_CONSTANT: constants read from memory rather than the packet
        {
            uint32_t address = s.Next() & 0x3FFFFFFF;
            uint32_t offsetType = s.Next();
            uint32_t size = s.Next() & 0xFFF;
            static const uint32_t kBanks[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t bank = (offsetType >> 16) & 0xFF;
            if (bank < 5)
            {
                uint32_t index = (offsetType & 0x7FF) + kBanks[bank];
                uint32_t guest = PhysicalToGuest(address & ~3u);
                for (uint32_t i = 0; i < size; i++) WriteRegister(index + i, R32(guest + i * 4));
            }
            break;
        }
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
        {
            uint32_t initiator;
            IndexSource source;
            if (opcode == PM4_DRAW_INDX)
            {
                s.Next();   // visibility query control
                initiator = s.Next();
                if (((initiator >> 6) & 3) == 0 && count >= 4) { source.base = s.Next(); source.size = s.Next(); }
            }
            else
            {
                initiator = s.Next();
                for (uint32_t i = 1; i < count; i++) source.immediate.push_back(s.Next());
            }
            OnDraw(initiator, source);
            gDraws++;
            break;
        }
        case PM4_NOP:
        case PM4_WAIT_FOR_IDLE:
        case PM4_EVENT_WRITE:
        case PM4_EVENT_WRITE_EXT:
        case PM4_EVENT_WRITE_ZPD:
        case 0x2E: case 0x3B: case 0x48: case 0x5E:
        case 0x60: case 0x61: case 0x62: case 0x63:
            break;  // state and drawing work with nothing the CPU can observe
        default:
        {
            std::lock_guard lock(gStatsMutex);
            gUnknownOpcodes[opcode]++;
            break;
        }
        }
        s = after;
    }

    void ExecuteStream(Stream s, int depth)
    {
        while (s.read != s.end)
        {
            uint32_t header = s.Next();
            if (header == 0 || header == 0xCDCDCDCD || header == 0x0BADF00D) continue;
            uint32_t type = header >> 30;
            if (type == 0)
            {
                uint32_t index = header & 0x7FFF, count = ((header >> 16) & 0x3FFF) + 1;
                bool oneRegister = (header >> 15) & 1;
                if (count > s.Available()) goto corrupt;
                for (uint32_t i = 0; i < count; i++) WriteRegister(oneRegister ? index : index + i, s.Next());
            }
            else if (type == 1)
            {
                if (s.Available() < 2) goto corrupt;
                WriteRegister(header & 0x7FF, s.Next());
                WriteRegister((header >> 11) & 0x7FF, s.Next());
            }
            else if (type == 3)
            {
                uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                if (count > s.Available()) goto corrupt;
                ExecuteType3(s, (header >> 8) & 0x7F, count, depth);
            }
            continue;
        corrupt:
            if (gCorruptLogged++ < 8) Log("[gpu] packet overruns its buffer: header 0x%08X, depth %d, stream base 0x%08X\n", header, depth, s.base);
            return;
        }
    }

    void CommandThread()
    {
        char drawLogSetting[32];
        if (GetEnvironmentVariableA("NIER_LOG_DRAWS_AT", drawLogSetting, sizeof(drawLogSetting))) gLogDrawsAt = strtoull(drawLogSetting, nullptr, 10);
        PPCContext ctx;
        gGpuContext = &ctx;
        gGpuPcr = AttachGuestThread(ctx);
        uint32_t idle = 0;
        while (true)
        {
            uint32_t write = R32(MMIO_BASE + REG_CP_RB_WPTR * 4) & (gRingDwords - 1);
            if (write == gReadIndex)
            {
                if (++idle > 256) Sleep(1);
                else SwitchToThread();
                continue;
            }
            idle = 0;
            ExecuteStream({ gRingBase, gRingDwords, gReadIndex, write }, 0);
            gReadIndex = write;
            W32(MMIO_BASE + REG_CP_RB_RPTR * 4, gReadIndex);
            if (gWritebackAddress) W32(gWritebackAddress, gReadIndex);
        }
    }

    void VblankThread()
    {
        PPCContext ctx;
        uint32_t pcr = AttachGuestThread(ctx);
        LARGE_INTEGER frequency, next;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next);
        const int64_t period = frequency.QuadPart * 1001 / 60000;
        while (true)
        {
            next.QuadPart += period;
            for (LARGE_INTEGER now;; Sleep(1))
            {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= next.QuadPart) break;
            }
            W32(MMIO_BASE + REG_VBLANK_STATUS * 4, 1);
            gVblanks++;
            DispatchInterrupt(ctx, pcr, 0, 2);
        }
    }

    void WriteVideoMode(uint32_t mode)
    {
        float refresh = 60.0f;
        uint32_t refreshBits;
        memcpy(&refreshBits, &refresh, 4);
        W32(mode + 0x00, 1280);
        W32(mode + 0x04, 720);
        W32(mode + 0x08, 0);        // progressive
        W32(mode + 0x0C, 1);        // widescreen
        W32(mode + 0x10, 1);        // high definition
        W32(mode + 0x14, refreshBits);
        W32(mode + 0x18, 1);        // NTSC
        W32(mode + 0x1C, 0x8A);
        W32(mode + 0x20, 1);
    }
}

uint64_t GpuFrameCount() { return gFrames.load(); }

void GpuDumpStats()
{
    Log("[gpu] %llu frames, %llu draws, %llu indirect buffers, %llu fences, %llu CP interrupts, %llu vblanks, ring read %u\n", gFrames.load(), gDraws.load(), gIndirect.load(), gFences.load(), gCpInterrupts.load(), gVblanks.load(), gReadIndex);
    if (gMovieImages)
    {
        Log("[movie] %llu new images over %llu swaps: average gap %.1fms, worst %.1fms, %llu gaps over 60ms\n", gMovieImages, gMovieSwaps, gMovieTotalGapMs / gMovieImages, gMovieWorstGapMs, gMovieLateImages);
        Log("[movie] gaps in refreshes: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7+:%llu\n", gMovieGapBins[0], gMovieGapBins[1], gMovieGapBins[2], gMovieGapBins[3], gMovieGapBins[4], gMovieGapBins[5], gMovieGapBins[6], gMovieGapBins[7]);
    }
    Log("[gpu] shaders seen: %zu vertex, %zu pixel\n", gShaderLibrary[0].size(), gShaderLibrary[1].size());
    Log("[gpu] %llu draws submitted, %llu resolves, %llu draws skipped\n", gSubmittedDraws.load(), gResolves.load(), gSkippedDraws.load());
    std::lock_guard lock(gStatsMutex);
    for (const auto& [reason, count] : gSkipReasons) Log("[gpu] skipped: %s x%llu\n", reason.c_str(), count);
    for (const auto& [format, count] : gSubstitutedFormats) Log("[gpu] texture format %u substituted x%llu\n", format, count);
    for (const auto& [opcode, count] : gUnknownOpcodes) Log("[gpu] unhandled opcode 0x%02X seen %llu times\n", opcode, count);
}

GUEST_IMPORT(VdInitializeEngines) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdShutdownEngines) { ctx.r3.u64 = 0; }

GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    gInterruptUser = ctx.r4.u32;
    gInterruptCallback = ctx.r3.u32;
    Log("[gpu] interrupt callback 0x%08X user 0x%08X\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdInitializeRingBuffer)
{
    gRingBase = PhysicalToGuest(ctx.r3.u32);
    gRingDwords = (1u << (ctx.r4.u32 + 3)) / 4;
    gReadIndex = 0;
    Log("[gpu] ring buffer at guest 0x%08X, %u dwords\n", gRingBase, gRingDwords);
    if (!gRunning.exchange(true))
    {
        std::thread(CommandThread).detach();
        std::thread(VblankThread).detach();
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    gWritebackAddress = PhysicalToGuest(ctx.r3.u32);
    Log("[gpu] read pointer writeback at guest 0x%08X\n", gWritebackAddress);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { ctx.r3.u64 = 0; }

GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    W32(ctx.r3.u32, 0xBEEF0000);
    W32(ctx.r4.u32, 0xBEEF0001);
    ctx.r3.u64 = 0;
}

// The caller reserves 64 dwords in the primary ring and expects the kernel to write the swap packet.
GUEST_IMPORT(VdSwap)
{
    uint32_t buffer = ctx.r3.u32;
    NoteGuestSwap();
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1) < 2)
        Log("[gpu] VdSwap args: r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
    W32(buffer, (3u << 30) | (62u << 16) | (PM4_XE_SWAP << 8));
    for (uint32_t i = 1; i < 64; i++) W32(buffer + i * 4, 0);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdQueryVideoMode) { WriteVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }
GUEST_IMPORT(XGetVideoMode) { WriteVideoMode(ctx.r3.u32); ctx.r3.u64 = 0; }
GUEST_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 3; }  // widescreen, 720p

GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    float power = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &power, 4);
    W32(ctx.r3.u32, 2);
    W32(ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdRetrainEDRAM) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdSetDisplayMode) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdEnableDisableClockGating) { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdPersistDisplay) { if (ctx.r4.u32) W32(ctx.r4.u32, 0); ctx.r3.u64 = 1; }
GUEST_IMPORT(KiApcNormalRoutineNop) { ctx.r3.u64 = 0; }

GUEST_IMPORT(KeInitializeDpc)
{
    uint32_t dpc = ctx.r3.u32;
    W16(dpc, 19);
    W32(dpc + 0x0C, ctx.r4.u32);
    W32(dpc + 0x10, ctx.r5.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification) { ctx.r3.u64 = 0; }
GUEST_IMPORT(XGetAVPack) { ctx.r3.u64 = 6; }          // HDMI
GUEST_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0xFFFF; }
GUEST_IMPORT(XGetLanguage) { ctx.r3.u64 = 1; }        // English

GUEST_IMPORT(ExGetXConfigSetting)
{
    uint32_t category = ctx.r3.u32, setting = ctx.r4.u32, buffer = ctx.r5.u32, size = ctx.r6.u32, required = ctx.r7.u32;
    uint32_t value = 0;
    bool known = true;
    if (category == 3 && setting == 0x0A) value = 0x00040000;       // video flags: widescreen
    else if (category == 3 && setting == 0x09) value = 1;           // language: English
    else if (category == 3 && setting == 0x0B) value = 0x40;        // retail flags
    else if (category == 2 && setting == 0x02) value = 0x00001000;  // AV region: NTSC-M
    else if (category == 3 && setting == 0x0E)                      // country: one byte, United States
    {
        if (buffer && size >= 1) W8(buffer, 103);
        if (required) W16(required, 1);
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    else known = false;

    if (!known)
    {
        Log("[xconfig] unhandled category %u setting 0x%X size %u\n", category, setting, size);
        ctx.r3.u64 = X_STATUS_INVALID_PARAMETER;
        return;
    }
    if (buffer && size >= 4) W32(buffer, value);
    if (required) W16(required, 4);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

void GpuRequestCapture() { gCaptureRequested = true; }

// Writes a resolve's pixels into guest memory the way the copy hardware lays out a 32-bit colour
// destination: tiled with the destination pitch, red/blue swapped if requested, then endian-swapped.
void GpuWriteResolvedPixels(const RendererResolve& resolve, const uint8_t* rgba, uint32_t rowPitch)
{
    static std::set<uint32_t> loggedFormats;
    if (loggedFormats.size() < 8 && loggedFormats.insert(resolve.destFormat).second)
        Log("[gpu] writing resolves back to guest memory (destination format %u, endian %u, e.g. %08X %ux%u)\n", resolve.destFormat, resolve.destEndian, resolve.destBase, resolve.width, resolve.height);
    const uint32_t pitch = (resolve.width + 31) & ~31u;
    uint8_t* destination = g_base + PhysicalToGuest(resolve.destBase);
    for (uint32_t y = 0; y < resolve.height; y++)
        for (uint32_t x = 0; x < resolve.width; x++)
        {
            uint8_t texel[4];
            memcpy(texel, rgba + size_t(y) * rowPitch + x * 4, 4);
            if (resolve.swapRedBlue) std::swap(texel[0], texel[2]);
            CopySwapped(destination + uint64_t(TiledOffset(x, y, pitch, 2)) * 4, texel, 4, resolve.destEndian);
        }
}
