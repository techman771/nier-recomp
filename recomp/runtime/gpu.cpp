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
        out.width = (c[2] & 0x1FFF) + 1;
        out.height = ((c[2] >> 13) & 0x1FFF) + 1;
        out.pitch = ((c[0] >> 22) & 0x1FF) << 5;
        out.tiled = (c[0] >> 31) & 1;
        out.packedMips = (c[5] >> 11) & 1;
        return true;
    }

    // Movies are drawn as a fullscreen quad over three 8-bit planes: luma plus two half-size chroma.
    struct MovieSource { TextureDescriptor y, u, v; bool valid = false; };
    MovieSource gMovie;
    uint64_t gLogDrawsAt = ~0ull;

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

    std::shared_ptr<DecodedTexture> DecodeTexture(const TextureDescriptor& t, TextureBinding& binding)
    {
        GuestTextureFormat format;
        uint32_t blockSize = 1, blockBytes = 4, logBytes = 2;
        switch (t.format)
        {
        case 2: format = GuestTextureFormat::R8; blockBytes = 1; logBytes = 0; break;
        case 6: format = GuestTextureFormat::RGBA8; break;
        case 18: format = GuestTextureFormat::BC1; blockSize = 4; blockBytes = 8; logBytes = 3; break;
        case 19: format = GuestTextureFormat::BC2; blockSize = 4; blockBytes = 16; logBytes = 4; break;
        case 20: format = GuestTextureFormat::BC3; blockSize = 4; blockBytes = 16; logBytes = 4; break;
        default:
            SkipDraw("texture format " + std::to_string(t.format));
            return nullptr;
        }
        const uint32_t widthBlocks = (t.width + blockSize - 1) / blockSize, heightBlocks = (t.height + blockSize - 1) / blockSize;
        const uint32_t pitchBlocks = std::max(t.pitch / blockSize, widthBlocks);
        const uint32_t guest = PhysicalToGuest(t.base);
        const uint32_t tiledHeight = (heightBlocks + 31) & ~31u;
        const uint64_t bytes = uint64_t(pitchBlocks) * (t.tiled ? tiledHeight : heightBlocks) * blockBytes;
        if (bytes == 0 || uint64_t(guest) + bytes > 0x100000000ull) return nullptr;

        uint64_t key = (uint64_t(t.base) << 20) ^ (uint64_t(t.format) << 8) ^ (uint64_t(t.width) << 44) ^ (uint64_t(t.height) << 30) ^ (t.tiled ? 1 : 0);
        const uint8_t* source = g_base + guest;
        for (uint64_t i = 0; i < bytes; i += 16) key = (key ^ *(const uint32_t*)(source + (i & ~3ull))) * 1099511628211ull;

        binding.scale[0] = float(t.width) / float(widthBlocks * blockSize);
        binding.scale[1] = float(t.height) / float(heightBlocks * blockSize);
        auto found = gDecodedTextures.find(key);
        if (found != gDecodedTextures.end()) return found->second;

        auto decoded = std::make_shared<DecodedTexture>();
        decoded->key = key;
        decoded->format = format;
        decoded->width = widthBlocks * blockSize;
        decoded->height = heightBlocks * blockSize;
        decoded->rowPitch = widthBlocks * blockBytes;
        decoded->data.resize(size_t(decoded->rowPitch) * heightBlocks);

        // A surface with a dimension of 16 or less lives inside a shared 32x32 tile, offset along
        // its short axis: textures wider than tall start 16 down, the rest start 16 across.
        uint32_t packedX = 0, packedY = 0;
        const auto logCeil = [](uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; };
        if (t.tiled && t.packedMips && std::min(logCeil(t.width), logCeil(t.height)) <= 4)
        {
            if (logCeil(t.width) > logCeil(t.height)) packedY = 16 / blockSize;
            else packedX = 16 / blockSize;
        }

        for (uint32_t y = 0; y < heightBlocks; y++)
        {
            uint8_t* row = decoded->data.data() + size_t(y) * decoded->rowPitch;
            for (uint32_t x = 0; x < widthBlocks; x++)
            {
                const uint32_t index = t.tiled ? TiledOffset(x + packedX, y + packedY, pitchBlocks, logBytes) : y * pitchBlocks + x;
                CopySwapped(row + size_t(x) * blockBytes, source + uint64_t(index) * blockBytes, blockBytes, t.endian);
            }
        }
        gDecodedTextures[key] = decoded;
        return decoded;
    }

    RenderTargetKey CurrentTarget(uint32_t& scale)
    {
        const uint32_t surface = gRegisters[0x2000], pitch = surface & 0x3FFF, msaa = (surface >> 16) & 3;
        scale = msaa == 2 ? 2 : 1;
        RenderTargetKey key;
        key.edramBase = gRegisters[0x2001] & 0xFFF;
        key.width = pitch * scale;
        key.height = key.width * 9 / 16;  // EDRAM doesn't record heights; menus are all 16:9
        return key;
    }

    void BuildResolve()
    {
        const uint32_t control = gRegisters[0x2318];
        if ((control & 7) != 0) return;      // depth copies aren't needed until 3D
        uint32_t scale;
        RendererResolve resolve;
        resolve.source = CurrentTarget(scale);
        resolve.destBase = gRegisters[0x2319] & 0x1FFFFFFF;
        resolve.width = gRegisters[0x231A] & 0x3FFF;
        resolve.height = (gRegisters[0x231A] >> 16) & 0x3FFF;
        resolve.swapRedBlue = (gRegisters[0x231B] >> 24) & 1;
        gResolvedBases.insert(resolve.destBase);
        gScanoutBase = resolve.destBase;
        gResolves++;
        gFrameCommands.emplace_back(std::move(resolve));
    }

    void BuildDraw(uint32_t initiator)
    {
        const uint32_t* r = gRegisters;
        const uint32_t mode = r[0x2208] & 7;
        if (mode == 6) { BuildResolve(); return; }
        if (mode != 4) { SkipDraw("depth-only pass"); return; }
        if ((r[0x2001] >> 16) & 0xF) { SkipDraw("render target format"); return; }
        if (((initiator >> 6) & 3) != 2) { SkipDraw("indexed draw"); return; }

        CachedShader* vs = ShaderFor(0), * ps = ShaderFor(1);
        if (!vs || !ps) { SkipDraw("no shader loaded"); return; }
        if (!vs->info.ok || !ps->info.ok) { SkipDraw("untranslated shader"); return; }

        RendererDraw draw;
        uint32_t scale;
        draw.target = CurrentTarget(scale);
        draw.vs = vs->shader;
        draw.ps = ps->shader;
        draw.blendControl = r[0x2201];
        draw.writeMask = r[0x2104] & 0xF;

        const uint32_t primitive = initiator & 0x3F, count = initiator >> 16;
        draw.vertexCount = count;
        switch (primitive)
        {
        case 4: break;
        case 8: draw.rectList = true; break;
        case 5: for (uint32_t i = 2; i < count; i++) draw.indices.insert(draw.indices.end(), { 0, i - 1, i }); break;
        case 6: for (uint32_t i = 2; i < count; i++) draw.indices.insert(draw.indices.end(), { i - 2 + (i & 1), i - 1 - (i & 1), i }); break;
        case 13: for (uint32_t i = 0; i + 4 <= count; i += 4) draw.indices.insert(draw.indices.end(), { i, i + 1, i + 2, i, i + 2, i + 3 }); break;
        default: SkipDraw("primitive type " + std::to_string(primitive)); return;
        }

        for (uint32_t slot : vs->info.vertexSlots)
        {
            const uint32_t d0 = r[REG_FETCH_CONSTANTS + slot * 2], d1 = r[REG_FETCH_CONSTANTS + slot * 2 + 1];
            if ((d0 & 3) != 3) { SkipDraw("vertex fetch constant not set"); return; }
            const uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
            if (bytes > 0x1000000) { SkipDraw("oversized vertex buffer"); return; }
            const size_t offset = draw.vertexData.size();
            draw.vertexData.resize(offset + bytes);
            CopySwapped(draw.vertexData.data() + offset, g_base + PhysicalToGuest(d0 & ~3u), bytes, d1 & 3);
            draw.vertexBase[slot] = uint32_t(offset);
        }

        memcpy(draw.constants.data(), &r[0x4000], draw.constants.size() * 4);
        memcpy(draw.boolConstants.data(), &r[0x4900], draw.boolConstants.size() * 4);

        for (uint32_t slot : ps->info.textureSlots)
        {
            TextureDescriptor t;
            if (!ReadTextureSlot(slot, t)) continue;
            const uint32_t* c = &r[REG_FETCH_CONSTANTS + slot * 6];
            TextureBinding& binding = draw.textures[slot];
            const uint32_t swizzle = (c[3] >> 1) & 0xFFF;
            for (int i = 0; i < 4; i++) binding.swizzle[i] = (swizzle >> (i * 3)) & 7;
            binding.point = ((c[3] >> 19) & 3) == 0;
            if (gResolvedBases.count(t.base)) binding.resolvedBase = t.base;
            else
            {
                binding.decoded = DecodeTexture(t, binding);
                if (!binding.decoded) continue;
            }
            binding.valid = true;
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
        const uint32_t topLeft = r[0x2081], bottomRight = r[0x2082];
        draw.scissor[0] = int((topLeft & 0x3FFF) * scale);
        draw.scissor[1] = int(((topLeft >> 16) & 0x3FFF) * scale);
        draw.scissor[2] = int(std::min((bottomRight & 0x3FFF) * scale, draw.target.width));
        draw.scissor[3] = int(std::min(((bottomRight >> 16) & 0x3FFF) * scale, draw.target.height));

        gSubmittedDraws++;
        gFrameCommands.emplace_back(std::move(draw));
    }

    void OnDraw(uint32_t initiator)
    {
        CaptureMovieSource(initiator);
        BuildDraw(initiator);
        uint64_t frame = gFrames.load();
        if (frame != gLogDrawsAt) return;
        if (gFrameDrawsLogged++ > 60) return;
        const uint32_t* r = gRegisters;
        char line[768];
        int used = snprintf(line, sizeof(line), "[draw f%llu] prim %u src %u count %u mode %u surf %08X col %08X dep %08X blend %08X cctl %08X dctl %08X vte %08X vp %08X/%08X %08X/%08X sciss %08X-%08X prog %08X vs %016llX ps %016llX vf0 %08X %08X copy %08X->%08X p%08X info %08X:",
            frame, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, r[0x2208] & 7, r[0x2000], r[0x2001], r[0x2002], r[0x2201], r[0x2204], r[0x2200], r[0x2206],
            r[0x210F], r[0x2110], r[0x2111], r[0x2112], r[0x2081], r[0x2082], r[0x2180], gShaders[0].hash, gShaders[1].hash, r[0x4800], r[0x4801], r[0x2318], r[0x2319], r[0x231A], r[0x231B]);
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            TextureDescriptor t;
            if (ReadTextureSlot(slot, t) && used < 440)
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
            uint32_t initiator = opcode == PM4_DRAW_INDX ? (s.Next(), s.Next()) : s.Next();
            OnDraw(initiator);
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
