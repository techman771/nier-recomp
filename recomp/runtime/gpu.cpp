// Stand-in for the Xenos GPU. It consumes the real PM4 command stream and performs everything the
// CPU side can observe (read pointer, fences, scratch writeback, interrupts, vblank), but draws
// nothing. Replacing the draw handling with a real backend is the later renderer work.
#define NOMINMAX
#pragma comment(lib, "synchronization.lib")
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "guest.h"
#include "renderer.h"
#include "settings.h"
#include "glyphs.h"
#include "framecapture.h"
#include "xenos_shader.h"
// The hash is compiled here, inline, with this file's own flags (AVX2): the library that is linked
// (xxhash.lib, used elsewhere) was built without them and hashes at about half the speed, and this
// thread hashes tens of megabytes a frame (texture contents, vertex data). The values are the same.
#define XXH_INLINE_ALL
#include <xxHash/xxhash.h>

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

    // The game's D3D library waits for the GPU to pass a fence by spinning (sub_82573758 calling
    // sub_82579D50 in a loop), which kept its main thread at 100% of a core for most of every frame.
    // Harmless on a desktop; on the Steam Deck the CPU and GPU share 15 W. The command thread counts
    // its progress here (a fence written, the ring's read pointer moved) and the wait loop sleeps
    // until the count changes (GpuSpinWait, mid-asm hook in nier_hooks.toml).
    std::atomic<uint32_t> gProgress{ 0 };
    std::atomic<int> gProgressWaiters{ 0 };
    inline void Progress()
    {
        gProgress.fetch_add(1);
        if (gProgressWaiters.load()) WakeByAddressAll(&gProgress);
    }
    std::mutex gStatsMutex;
    std::map<uint32_t, uint64_t> gUnknownOpcodes;
    int gWaitTimeoutsLogged = 0, gCorruptLogged = 0;
    struct WaitStats { uint64_t count = 0, ticks = 0; uint32_t lastReference = 0, mask = 0; };
    std::map<uint64_t, WaitStats> gWaitStats;   // WAIT_REG_MEM by (info, polled location)

    // Command thread time per frame (swap to swap): waiting in WAIT_REG_MEM, idle with the ring empty
    // (waiting for the game's commands), and the rest (work). Summed separately for frames that took
    // more than 1.5 vblanks, to see what makes 60 fps frames late.
    struct FrameTiming { uint64_t frames = 0, total = 0, wait = 0, idle = 0; };
    FrameTiming gOnTime, gLate;
    std::atomic<float> gCommandWorkMs{ 0 };   // the command thread's working time a frame, smoothed (GpuCommandWorkMs)
    uint64_t gFrameWaitTicks = 0, gFrameIdleTicks = 0;
    // For the same trace: textures converted this frame (count, bytes, time) and time spent hashing textures in full.
    uint64_t gFrameDecodeTicks = 0, gFrameDecodeBytes = 0, gFrameDecodeCount = 0, gFrameHashTicks = 0, gFrameHashBytes = 0;
    // And the time in each kind of command packet (by opcode; packets inside an indirect buffer count for themselves).
    uint64_t gFrameOpTicks[256] = {};
    // Vertex bytes copied for this frame's draws (BuildDraw), and the longest any one packet of each kind took.
    uint64_t gFrameVertexCopied = 0, gFrameVertexKept = 0, gFrameVertexHashed = 0;
    // Vertex copies kept from earlier draws (BuildDraw): by where the bytes came from and their hash.
    struct VertexKept { ByteBuffer data; uint64_t lastFrame = 0, bornFrame = 0; };
    std::unordered_map<uint64_t, VertexKept> gVertexCache;
    uint64_t gVertexCacheBytes = 0;
    // And per place (address, extent, byte order): how often in a row its bytes were new, and until which frame it is just copied.
    // (hits, hashedFrame, hashedKey: a range whose bytes have hashed the same 60 times running is
    // hashed once a frame, and its other draws in that frame take that hash. Level geometry is drawn
    // two or three times a frame (shadows, the scene), and hashing it for each was 8 MB a frame.)
    // Such a range of 16 KB or more is then checked the way unchanging plain textures are
    // (DecodeTexture): 64 bytes of every 512 at an offset that moves on each frame, the sample for
    // each of the 8 offsets taken while the full hash still was, a full hash every 600 frames.
    struct VertexRange
    {
        uint32_t misses = 0; uint64_t copyUntil = 0; uint32_t hits = 0; uint64_t hashedFrame = ~0ull, hashedKey = 0;
        uint64_t fullFrame = 0, phases[8] = {}; uint32_t phaseMask = 0;
    };
    std::unordered_map<uint64_t, VertexRange> gVertexRanges;

    // Index lists (BuildDraw). Every indexed draw read its indices from the game's memory, turned
    // them the right way round, widened them to 32 bits, assembled its primitive and found its
    // smallest and largest index, and the renderer then copied the list to the GPU: on the Steam
    // Deck a tenth of this thread and 6 MB a frame for the renderer in a wide scene, for meshes
    // whose indices never change. The list assembled for a draw is kept, by where its indices came
    // from and how they were to be read, with a hash of the game's bytes there; a later draw of the
    // same bytes takes the list, the flags and the index range as they are. The bytes are checked as
    // vertex ranges are: in full until they have come out the same 60 times, then once a frame, and
    // from 4 KB up by a moving sample (64 bytes of every 512, 8 offsets) with a full hash every 600
    // frames. Places whose indices keep changing are told by three misses in a row and left alone
    // for two seconds. NIER_NO_INDEX_CACHE=1: assemble for every draw, as before.
    // NIER_VERIFY_INDEX_CACHE=1 (testing): assemble anyway and compare with the kept list.
    struct IndexKept
    {
        IndexList list;
        bool rectList = false, strip = false;
        uint32_t minIndex = 0xFFFFFFFF, maxIndex = 0;
        uint64_t contentKey = 0, lastFrame = 0, bornFrame = 0, hashedFrame = ~0ull, fullFrame = 0, copyUntil = 0;
        uint64_t phases[8] = {};
        uint32_t hits = 0, misses = 0, phaseMask = 0;
    };
    std::unordered_map<uint64_t, IndexKept> gIndexCache;

    // Once a frame: copies nobody drew from for two seconds go (sooner when a lot is held).
    void VertexCacheTrim(uint64_t frame)
    {
        if (frame % 30) return;
        for (auto it = gIndexCache.begin(); it != gIndexCache.end();)
        {
            if (it->second.lastFrame + 120 < frame && it->second.copyUntil < frame) it = gIndexCache.erase(it);
            else ++it;
        }
        const uint64_t age = gVertexCacheBytes > (768ull << 20) ? 15 : 120;
        for (auto it = gVertexCache.begin(); it != gVertexCache.end();)
        {
            if (it->second.lastFrame + age < frame) { gVertexCacheBytes -= it->second.data.size(); it = gVertexCache.erase(it); }
            else ++it;
        }
        if (frame % 1800 == 0)
            for (auto it = gVertexRanges.begin(); it != gVertexRanges.end();)
            {
                if (it->second.copyUntil + 1800 < frame && !it->second.misses) it = gVertexRanges.erase(it);
                else ++it;
            }
    }
    uint64_t gFrameOpLongest[256] = {};
    uint32_t gFrameOpCount[256] = {};
    const bool gLogPacing = GetEnvironmentVariableA("NIER_LOG_PACING", nullptr, 0) > 0;
    LARGE_INTEGER gLastSwapTime{};
    uint64_t gFrameTimeBins[6] = {};   // frame time in vblanks: <1.25, <1.75, <2.25, <2.75, <3.5, more

    void NoteFrameTiming()
    {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (gLastSwapTime.QuadPart)
        {
            const uint64_t total = uint64_t(now.QuadPart - gLastSwapTime.QuadPart);
            const double vblanks = double(total) * 60.0 / double(frequency.QuadPart);
            gFrameTimeBins[vblanks < 1.25 ? 0 : vblanks < 1.75 ? 1 : vblanks < 2.25 ? 2 : vblanks < 2.75 ? 3 : vblanks < 3.5 ? 4 : 5]++;
            FrameTiming& bucket = vblanks > 1.5 ? gLate : gOnTime;
            bucket.frames++;
            bucket.total += total;
            bucket.wait += gFrameWaitTicks;
            bucket.idle += gFrameIdleTicks;
            const float workMs = float(double(total - gFrameWaitTicks - gFrameIdleTicks) * 1000.0 / double(frequency.QuadPart));
            gCommandWorkMs = gCommandWorkMs.load() * 0.95f + (std::min)(workMs, 16.7f) * 0.05f;
            // NIER_LOG_PACING=1: where the command thread's time went in a frame that took over 1.5
            // vblanks, and (for comparison) in every 100th frame that did not.
            static const bool logPacing = GetEnvironmentVariableA("NIER_LOG_PACING", nullptr, 0) > 0;
            static uint32_t onTime = 0;
            if (logPacing && (vblanks > 1.5 || ++onTime % 100 == 0))
            {
                const double k = 1000.0 / double(frequency.QuadPart);
                Log("[pace] command thread, frame %llu (%s): %.1f ms = %.1f waiting (WAIT_REG_MEM) + %.1f idle (no commands from the game) + %.1f working (of that %.1f converting %llu textures, %llu KB, and %.1f hashing %llu KB in full; vertex data %llu KB copied, %llu KB kept from before, %llu KB hashed; %llu MB kept in all)\n",
                    (unsigned long long)gFrames.load(), vblanks > 1.5 ? "late" : "on time", double(total) * k, double(gFrameWaitTicks) * k, double(gFrameIdleTicks) * k,
                    double(total - gFrameWaitTicks - gFrameIdleTicks) * k, double(gFrameDecodeTicks) * k, (unsigned long long)gFrameDecodeCount, (unsigned long long)(gFrameDecodeBytes >> 10),
                    double(gFrameHashTicks) * k, (unsigned long long)(gFrameHashBytes >> 10), (unsigned long long)(gFrameVertexCopied >> 10), (unsigned long long)(gFrameVertexKept >> 10), (unsigned long long)(gFrameVertexHashed >> 10),
                    (unsigned long long)(gVertexCacheBytes >> 20));
                {
                    // The packet kinds that took the most time.
                    int order[256];
                    for (int i = 0; i < 256; i++) order[i] = i;
                    std::partial_sort(order, order + 5, order + 256, [](int a, int b) { return gFrameOpTicks[a] > gFrameOpTicks[b]; });
                    char text[256];
                    int at = 0;
                    uint64_t sum = 0;
                    for (int i = 0; i < 256; i++) sum += gFrameOpTicks[i];
                    for (int i = 0; i < 5 && gFrameOpTicks[order[i]]; i++)
                        at += snprintf(text + at, sizeof(text) - at, "%s0x%02X x%u %.1f ms (longest %.2f)", i ? ", " : "", order[i], gFrameOpCount[order[i]], double(gFrameOpTicks[order[i]]) * k, double(gFrameOpLongest[order[i]]) * k);
                    Log("[pace]   packets: %s; all packets %.1f ms\n", at ? text : "-", double(sum) * k);
                }
            }
        }
        gLastSwapTime = now;
        gFrameWaitTicks = gFrameIdleTicks = 0;
        gFrameDecodeTicks = gFrameDecodeBytes = gFrameDecodeCount = gFrameHashTicks = gFrameHashBytes = 0;
        memset(gFrameOpTicks, 0, sizeof(gFrameOpTicks));
        gFrameVertexCopied = gFrameVertexKept = gFrameVertexHashed = 0;
        memset(gFrameOpLongest, 0, sizeof(gFrameOpLongest));
        memset(gFrameOpCount, 0, sizeof(gFrameOpCount));
    }
    std::atomic<bool> gRunning{ false };

    struct Stream
    {
        uint32_t base, dwords, read, end;
        // (read and end stay below dwords. No division here: a remainder by a length only known at
        // run time for every word read was a tenth of the command thread's time on the Steam Deck.)
        uint32_t Available() const { return end >= read ? end - read : end + dwords - read; }
        uint32_t Next()
        {
            uint32_t value = R32(base + read * 4);
            if (++read == dwords) read = 0;
            return value;
        }
        void Skip(uint32_t count) { read += count; if (read >= dwords) read %= dwords; }
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
        if (out.width <= 16 && out.height <= 16 && out.format >= 18 && out.format <= 20)
        {
            static std::set<uint32_t> seen;   // debugging: the raw state of tiny compressed textures
            if (seen.size() < 12 && seen.insert(out.base ^ slot).second)
                Log("[gpu] tiny texture slot %u %ux%u format %u base %08X: %08X %08X %08X %08X %08X %08X\n", slot, out.width, out.height, out.format, out.base, c[0], c[1], c[2], c[3], c[4], c[5]);
        }
        return true;
    }

    // Movies are drawn as a fullscreen quad over three 8-bit planes: luma plus two half-size chroma.
    struct MovieSource { TextureDescriptor y, u, v; bool valid = false; };
    MovieSource gMovie;
    uint64_t gLogDrawsAt = ~0ull;
    std::atomic<bool> gCaptureRequested{ false };
    std::atomic<bool> gRecordingRequested{ false };   // F9: record every frame for 10 seconds (framecapture.cpp)
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

    uint64_t gSaveCommandsAt = ~0ull;   // F12: the frame whose renderer commands are saved (framecapture.cpp)
    const std::vector<uint32_t>* ShaderWords(bool pixel, uint64_t hash);

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
        const size_t commandCount = gFrameCommands.size();
        output.commands = std::move(gFrameCommands);
        output.scanoutBase = gScanoutBase;
        if (FrameSequenceActive()) FrameSequenceAdd(output);
        if (frame == gSaveCommandsAt)
        {
            char path[96];
            CreateDirectoryA("../captures", nullptr);
            snprintf(path, sizeof(path), "../captures/frame_f%llu.nfrm", frame - 1);
            FrameCaptureSave(output, path, ShaderWords);
            gSaveCommandsAt = ~0ull;
        }
        gFrameCommands = RendererTakeCommandList();   // an earlier frame's list, emptied: memory already in use
        gFrameCommands.reserve(commandCount + commandCount / 4 + 16);   // draws are large; avoid regrowing
        RendererSubmitFrame(std::move(output));
    }

    // Shader microcode as last uploaded by the game, kept by stage (0 vertex, 1 pixel).
    struct ShaderCapture { std::vector<uint32_t> words; uint64_t hash = 0; };
    ShaderCapture gShaders[2];
    std::map<uint64_t, std::vector<uint32_t>> gShaderLibrary[2];

    // The shader dumps in recomp\shaders (microcode for tools/pipeline_list, HLSL to read) are written
    // by a thread of their own. Written where the shader was first met, on the command thread, two
    // files a shader, a scene that brought thirteen new shaders held its first frame for 60 ms.
    struct DumpQueue
    {
        std::mutex mutex;
        std::condition_variable work;
        std::deque<std::pair<std::string, std::string>> files;   // path, contents
        bool started = false;
    } gDumps;

    void WriteDumpLater(const char* path, const void* data, size_t bytes)
    {
        std::lock_guard lock(gDumps.mutex);
        if (!gDumps.started)
        {
            gDumps.started = true;
            std::thread([] {
                CreateDirectoryA("..\\shaders", nullptr);
                for (;;)
                {
                    std::pair<std::string, std::string> file;
                    {
                        std::unique_lock lock(gDumps.mutex);
                        gDumps.work.wait(lock, [] { return !gDumps.files.empty(); });
                        file = std::move(gDumps.files.front());
                        gDumps.files.pop_front();
                    }
                    if (FILE* out = fopen(file.first.c_str(), "wb")) { fwrite(file.second.data(), 1, file.second.size(), out); fclose(out); }
                }
            }).detach();
        }
        gDumps.files.emplace_back(path, std::string(static_cast<const char*>(data), bytes));
        gDumps.work.notify_one();
    }

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
            char path[96];
            snprintf(path, sizeof(path), "..\\shaders\\%s_%016llX.bin", stage ? "ps" : "vs", hash);
            WriteDumpLater(path, words.data(), words.size() * 4);
            gShaderLibrary[stage][hash] = std::move(words);
        }
    }

    const std::vector<uint32_t>* ShaderWords(bool pixel, uint64_t hash)
    {
        auto found = gShaderLibrary[pixel ? 1 : 0].find(hash);
        return found == gShaderLibrary[pixel ? 1 : 0].end() ? nullptr : &found->second;
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
        if (entry.info.ok) RendererPrepareShader(shader);   // compile in the background, ahead of use
        char path[96];
        snprintf(path, sizeof(path), "..\\shaders\\%s_%016llX.hlsl", stage ? "ps" : "vs", capture.hash);
        WriteDumpLater(path, entry.info.hlsl.data(), entry.info.hlsl.size());
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

    // Xenos endian modes: 1 swaps bytes in 16-bit units, 2 in 32-bit units, 3 swaps 16-bit halves.
    void SwapInPlace(uint8_t* data, uint32_t bytes, uint32_t endian)
    {
        if (endian == 1)
        {
            uint32_t i = 0;
            for (; i + 2 <= bytes && (i & 7); i += 2) std::swap(data[i], data[i + 1]);
            for (; i + 8 <= bytes; i += 8)
            {
                uint64_t v;
                memcpy(&v, data + i, 8);
                v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
                memcpy(data + i, &v, 8);
            }
            for (; i + 2 <= bytes; i += 2) std::swap(data[i], data[i + 1]);
        }
        else if (endian == 2)
            for (uint32_t i = 0; i + 4 <= bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, data + i, 4);
                v = __builtin_bswap32(v);
                memcpy(data + i, &v, 4);
            }
        else if (endian == 3)
            for (uint32_t i = 0; i + 4 <= bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, data + i, 4);
                v = (v << 16) | (v >> 16);
                memcpy(data + i, &v, 4);
            }
    }

    // Copies and corrects the byte order in one pass over the data (vertex and index buffers go
    // through here on every draw; copying and then swapping in place read it all twice).
    void CopySwapped(uint8_t* out, const uint8_t* in, uint32_t bytes, uint32_t endian)
    {
        uint32_t i = 0;
        if (endian == 1)
            for (; i + 4 <= bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, in + i, 4);
                v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
                memcpy(out + i, &v, 4);
            }
        else if (endian == 2)
            for (; i + 4 <= bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, in + i, 4);
                v = __builtin_bswap32(v);
                memcpy(out + i, &v, 4);
            }
        else if (endian == 3)
            for (; i + 4 <= bytes; i += 4)
            {
                uint32_t v;
                memcpy(&v, in + i, 4);
                v = (v << 16) | (v >> 16);
                memcpy(out + i, &v, 4);
            }
        else { memcpy(out, in, bytes); return; }
        if (i < bytes)   // an odd tail: as SwapInPlace leaves it
        {
            memcpy(out + i, in + i, bytes - i);
            SwapInPlace(out + i, bytes - i, endian);
        }
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
        // Hashing the contents on every use dominated the command thread. Small data textures (bone
        // matrices, rewritten between draws) are still hashed every time; the rest once per frame,
        // since the game streams them in between frames.
        // (stable, phases, phaseMask: plain textures that have stopped changing, below.)
        struct HashMemo { uint64_t frame = ~0ull, hash = 0, signature = 0, fullFrame = 0; uint32_t stable = 0, phaseMask = 0; uint64_t phases[16] = {}; };
        static std::unordered_map<uint64_t, HashMemo> hashMemo;
        static const bool hashEveryUse = GetEnvironmentVariableA("NIER_HASH_EVERY_USE", nullptr, 0) > 0;   // debugging
        // Of the small ones only data textures are (one row, or a float format); hashing every small
        // image on each use was still a seventh of the thread's time in a busy scene.
        const bool dataTexture = t.height == 1 || t.format == 29 || (t.format >= 31 && t.format <= 38);
        const bool large = !hashEveryUse && (bytes + (levels > 1 ? mipBytes : 0) > 64 * 1024 || !dataTexture);
        HashMemo* memo = large ? &hashMemo[key] : nullptr;
        // Every byte is hashed (XXH3, many GB/s): a bone matrix rewritten in place may change only some
        // of a texel's components, which sampling would miss.
        uint64_t content = 0;
        const uint64_t frameNow = gFrames.load();
        if (memo && memo->frame == frameNow) content = memo->hash;
        else
        {
            // Block-compressed textures are the game's art: never edited in place, only replaced whole
            // when the game streams another one into the same memory. For those a sample is hashed
            // each frame (64 bytes at 48 places across the base level and the mips) and the full hash
            // taken only when the sample changes, or every ten seconds or so regardless. Hashing all
            // of them in full once a frame was a fifth of this thread's time at The Aerie. Plain
            // formats (which include the text glyph sheets, edited a glyph at a time) are always
            // hashed in full. NIER_HASH_FULL=1 hashes everything in full (debugging).
            static const bool hashFull = GetEnvironmentVariableA("NIER_HASH_FULL", nullptr, 0) > 0;
            uint64_t signature = 0;
            bool unchanged = false;
            // Plain (uncompressed) textures of 256 KB and more were hashed in full every frame: 25 MB a
            // frame in the lighthouse, 16 MB of it one 2048x2048 texture, which on the Steam Deck was a
            // quarter of this thread's time (the thread that decides whether a frame is late there).
            // They can be edited in place (the glyph sheets are), so no fixed sample will do. Instead:
            // while a texture's full hash keeps coming out the same, 64 bytes of every 1024 are hashed,
            // at an offset that moves on each frame, so every byte is looked at once in 16 frames, and
            // the sample for each of the 16 offsets was taken while the full hash was being checked
            // too. A sample that differs brings the full hash back for the next 300 uses. As with the
            // compressed ones, a full hash is taken every ten seconds or so regardless.
            // NIER_VERIFY_TEXTURE_HASH=1 (testing): the full hash is taken as well and a difference logged.
            static const bool verifyHash = GetEnvironmentVariableA("NIER_VERIFY_TEXTURE_HASH", nullptr, 0) > 0;
            const bool plain = memo && blockSize != 4 && !hashFull && levels <= 1 && bytes >= (256u << 10);
            const uint32_t phase = uint32_t(frameNow & 15);
            auto plainSample = [&] {
                uint64_t h = 0x9E3779B97F4A7C15ull + phase;
                for (uint64_t at = uint64_t(phase) * 64; at + 64 <= bytes; at += 1024) h = XXH3_64bits_withSeed(source + at, 64, h);
                return h;
            };
            if (plain && memo->stable >= 300 && memo->phaseMask == 0xFFFF)
            {
                unchanged = plainSample() == memo->phases[phase];
                if (!unchanged) { memo->stable = 0; memo->phaseMask = 0; }
                else if (frameNow - memo->fullFrame >= 600 + ((key * 0x9E3779B97F4A7C15ull) >> 55)) unchanged = false;   // its turn for a full hash
                else if (verifyHash && XXH3_64bits(source, size_t(bytes)) != memo->hash)
                    Log("[gpu] texture %08X (%ux%u, format %u) changed where the sample did not look (frame %llu)\n", t.base, t.width, t.height, t.format, (unsigned long long)frameNow);
            }
            if (memo && blockSize == 4 && !hashFull && bytes >= 4096)
            {
                uint8_t sample[48 * 64];
                const uint64_t spread = (bytes - 64) / 39;
                for (int i = 0; i < 40; i++) memcpy(sample + i * 64, source + uint64_t(i) * spread, 64);
                const bool mips = levels > 1 && mipBytes >= 4096;
                for (int i = 0; i < 8; i++)
                {
                    if (mips) memcpy(sample + (40 + i) * 64, g_base + mipGuest + uint64_t(i) * ((mipBytes - 64) / 7), 64);
                    else memset(sample + (40 + i) * 64, 0, 64);
                }
                signature = XXH3_64bits(sample, sizeof(sample)) | 1;   // never zero
                unchanged = memo->signature == signature;
                // The full hash "regardless" is a safeguard, so it can wait its turn: each texture's
                // comes due 600-1111 frames after its last one (by a hash of the key: the key's own
                // low bits are the same for nearly every texture, which made everything first seen in
                // one frame, a whole scene's textures, come due together), and no more than 2 MB of
                // them are done in a frame, the rest staying due. All of a scene's at once held the
                // command thread for 8 ms or more: one frame late about every ten seconds in a busy
                // cutscene, in groups that repeated at the same spacing.
                if (unchanged && frameNow - memo->fullFrame >= 600 + ((key * 0x9E3779B97F4A7C15ull) >> 55))
                {
                    static uint64_t budgetFrame = ~0ull, budgetLeft = 0;
                    const uint64_t budget = 2u << 20;
                    if (budgetFrame != frameNow) { budgetFrame = frameNow; budgetLeft = budget; }
                    const uint64_t cost = bytes + (levels > 1 ? mipBytes : 0);
                    if (budgetLeft == budget || cost <= budgetLeft) { unchanged = false; budgetLeft -= (std::min)(budgetLeft, cost); }   // (one larger than the budget still gets its turn)
                }
            }
            if (unchanged)
            {
                content = memo->hash;
                memo->frame = frameNow;
            }
            else
            {
                LARGE_INTEGER hashBegin, hashEnd;
                QueryPerformanceCounter(&hashBegin);
                content = XXH3_64bits(source, size_t(bytes));
                if (levels > 1) content = XXH3_64bits_withSeed(g_base + mipGuest, size_t(mipBytes), content);
                QueryPerformanceCounter(&hashEnd);
                if (gLogPacing && bytes + (levels > 1 ? mipBytes : 0) >= (512u << 10))
                {
                    // Which large textures get the full hash, and how often (the first 40 kinds, then every 600th time).
                    static std::unordered_map<uint64_t, uint32_t> times;
                    uint32_t& n = times[key];
                    if ((n++ == 0 && times.size() <= 40) || n % 600 == 0)
                        Log("[pace] full hash of texture %08X: format %u, %ux%u, %u levels, %u KB, block size %u, tiled %d (time %u, frame %llu)\n", t.base, t.format, t.width, t.height,
                            levels, uint32_t((bytes + (levels > 1 ? mipBytes : 0)) >> 10), blockSize, t.tiled ? 1 : 0, n, (unsigned long long)frameNow);
                }
                gFrameHashTicks += uint64_t(hashEnd.QuadPart - hashBegin.QuadPart);
                gFrameHashBytes += bytes + (levels > 1 ? mipBytes : 0);
                if (memo)
                {
                    const bool same = memo->hash == content && memo->frame != ~0ull;
                    memo->stable = same ? memo->stable + 1 : 0;
                    if (!same) memo->phaseMask = 0;
                    memo->frame = frameNow;
                    memo->hash = content;
                    memo->signature = signature;
                    memo->fullFrame = frameNow;
                    if (plain && same) { memo->phases[phase] = plainSample(); memo->phaseMask |= 1u << phase; }
                }
            }
        }

        binding.scale[0] = float(t.width) / float(widthBlocks * blockSize);
        binding.scale[1] = float(t.height) / float(heightBlocks * blockSize);
        auto found = gDecodedTextures.find(key);
        if (found != gDecodedTextures.end() && found->second->contentHash == content) return GlyphAtlasFor(found->second, t.format, t.width, t.height);

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
        LARGE_INTEGER decodeBegin;
        QueryPerformanceCounter(&decodeBegin);
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
        {
            LARGE_INTEGER decodeEnd;
            QueryPerformanceCounter(&decodeEnd);
            gFrameDecodeTicks += uint64_t(decodeEnd.QuadPart - decodeBegin.QuadPart);
            gFrameDecodeBytes += total;
            gFrameDecodeCount++;
        }
        // NIER_DUMP_TEX=<physical base>[,...] writes the decoded base level (host layout) to captures/,
        // with a header: format, width, height, row pitch, rows (debugging).
        static const std::set<uint32_t> dumpTextures = [] {
            std::set<uint32_t> bases;
            char value[512];
            if (GetEnvironmentVariableA("NIER_DUMP_TEX", value, sizeof(value)))
                for (char* item = strtok(value, ","); item; item = strtok(nullptr, ",")) bases.insert(strtoul(item, nullptr, 16));
            return bases;
        }();
        // NIER_DUMP_TEX_FORMAT=<guest format>: the same for the first 48 mipmapped 2D textures of that
        // format. After the base level come the level count and, per further level, row pitch, rows
        // and data (tools can then compare each level with the base scaled down).
        static const uint32_t dumpFormat = [] {
            char value[16];
            return GetEnvironmentVariableA("NIER_DUMP_TEX_FORMAT", value, sizeof(value)) ? uint32_t(strtoul(value, nullptr, 10)) : ~0u;
        }();
        static int formatDumps = 0;
        const bool dumpByFormat = t.format == dumpFormat && levels > 1 && faces == 1 && formatDumps < 48;
        if (dumpByFormat) formatDumps++;
        if (dumpTextures.count(t.base) || dumpByFormat)
        {
            CreateDirectoryA("..\\captures", nullptr);
            char path[96];
            snprintf(path, sizeof(path), "..\\captures\\tex_%08X.bin", t.base);
            if (FILE* file = fopen(path, "wb"))
            {
                const DecodedTexture::Subresource& sub = decoded->subresources[0];
                const uint32_t header[6] = { uint32_t(format), decoded->width, decoded->height, sub.rowPitch, sub.rows, t.format };
                fwrite(header, 4, 6, file);
                fwrite(decoded->data.data() + sub.offset, 1, size_t(sub.rowPitch) * sub.rows, file);
                const uint32_t levelCount = faces == 1 ? uint32_t(decoded->subresources.size()) : 1;
                fwrite(&levelCount, 4, 1, file);
                for (uint32_t level = 1; level < levelCount; level++)
                {
                    const DecodedTexture::Subresource& mip = decoded->subresources[level];
                    const uint32_t shape[2] = { mip.rowPitch, mip.rows };
                    fwrite(shape, 4, 2, file);
                    fwrite(decoded->data.data() + mip.offset, 1, size_t(mip.rowPitch) * mip.rows, file);
                }
                fclose(file);
                Log("[gpu] dumped texture %08X (%ux%u format %u, pitch %u, tiled %d, endian %u, levels %u, packed %d)\n", t.base, t.width, t.height, t.format, t.pitch, t.tiled ? 1 : 0, t.endian, levels, t.packedMips ? 1 : 0);
            }
        }
        gDecodedTextures[key] = decoded;
        return GlyphAtlasFor(decoded, t.format, t.width, t.height);   // button icons for the device in use
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
        static thread_local std::vector<uint8_t> raw;   // reused: no allocation or zero-fill per draw
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
    // `in` is consumed: a triangle list without restart markers, the usual case, is taken as it is.
    bool AssemblePrimitives(uint32_t primitive, std::vector<uint32_t>& in, RendererDraw& draw)
    {
        std::vector<uint32_t> out;
        struct Hand { RendererDraw& draw; std::vector<uint32_t>& out; ~Hand() { if (!out.empty()) draw.indices = std::move(out); } } hand{ draw, out };
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
        {
            draw.rectList = primitive == 8;
            uint32_t restarts = 0;
            for (uint32_t index : in) restarts += index == kRestart;
            if (!restarts)
            {
                in.resize(in.size() - in.size() % 3);
                out = std::move(in);
                return true;
            }
            out.reserve(in.size());
            for (size_t i = 0; i + 3 <= in.size(); i += 3)
                if (in[i] != kRestart && in[i + 1] != kRestart && in[i + 2] != kRestart) { out.push_back(in[i]); out.push_back(in[i + 1]); out.push_back(in[i + 2]); }
            return true;
        }
        case 5:
            out.reserve(in.size() * 3);
            forEachSegment([&](size_t begin, size_t end) {
                for (size_t i = begin + 2; i < end; i++) { out.push_back(in[begin]); out.push_back(in[i - 1]); out.push_back(in[i]); }
            });
            return true;
        case 6:
            // Strips are most of the game's geometry: handed on as they are (the restart marker is the
            // host's strip cut value) and drawn as strips, instead of being expanded to a list.
            draw.strip = true;
            out = std::move(in);
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

        // Built in place at the end of the frame's commands, and taken off again if the draw is given
        // up: a draw is about 10 KB, and building it apart and moving it in copied all of that again.
        gFrameCommands.emplace_back(std::in_place_type<RendererDraw>);
        struct Pending
        {
            bool keep = false;
            ~Pending() { if (!keep) gFrameCommands.pop_back(); }
        } pending;
        RendererDraw* building = &std::get<RendererDraw>(gFrameCommands.back());
#define draw (*building)
        uint32_t scale;
        draw.target = CurrentTarget(scale);
        draw.vs = vs->shader;
        draw.ps = ps->shader;
        draw.blendControl = r[0x2201];
        memcpy(draw.blendConstant, &r[0x2105], sizeof(draw.blendConstant));   // RB_BLEND_RED, GREEN, BLUE, ALPHA (floats)
        draw.alphaFunc = (r[0x2202] & 8) ? uint8_t(r[0x2202] & 7) : 7;        // RB_COLORCONTROL: function in bits 0-2, enable in bit 3
        memcpy(&draw.alphaRef, &r[0x210E], 4);                                // RB_ALPHA_REF (float)
        draw.paramGen = (r[0x2180] >> 18) & 1 ? uint8_t((r[0x2181] >> 8) & 0xFF) : 255;   // SQ_PROGRAM_CNTL bit 18, SQ_CONTEXT_MISC bits 8-15
        {
            static std::set<uint32_t> seenGen;   // which interpolators the game uses for it (debugging)
            if (draw.paramGen != 255 && seenGen.size() < 8 && seenGen.insert(draw.paramGen).second)
                Log("[gpu] generated pixel parameter in interpolator %u (first with pixel shader %016llX)\n", draw.paramGen, ps->shader->hash);
        }
        {
            static std::set<uint32_t> seen;   // which alpha tests the game uses (debugging)
            if (draw.alphaFunc != 7 && seen.size() < 8 && seen.insert(uint32_t(draw.alphaFunc) << 24 ^ r[0x210E]).second)
                Log("[gpu] alpha test: function %u, reference %.4f\n", draw.alphaFunc, draw.alphaRef);
        }
        draw.writeMask = r[0x2104] & 0xF;

        draw.hasColor = hasColor;
        draw.depthControl = r[0x2200];
        draw.stencilRefMask = r[0x210D];
        {
            // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces, bit 2 set makes clockwise
            // the front. Rectangles are never culled. NIER_NO_CULL=1 turns culling off (debugging).
            static const bool noCull = GetEnvironmentVariableA("NIER_NO_CULL", nullptr, 0) > 0;
            const uint32_t mode = r[0x2205];
            if (!draw.rectList && !noCull)
            {
                if ((mode & 3) == 3) { SkipDraw("both faces culled"); return; }
                draw.cull = (mode & 1) ? 1 : (mode & 2) ? 2 : 0;
            }
            draw.frontCounterClockwise = !((mode >> 2) & 1);
        }
        static const bool depthDisabled = GetEnvironmentVariableA("NIER_NO_DEPTH", nullptr, 0) > 0;  // debugging
        if ((draw.depthControl & 3) && !depthDisabled)   // stencil or depth test enabled
        {
            draw.depthBound = true;
            draw.depth = TargetFor(r[0x2002] & 0xFFF, draw.target.width);
        }
        if (!hasColor && !draw.depthBound) { SkipDraw("depth pass with depth disabled"); return; }

        const uint32_t primitive = initiator & 0x3F;
        uint32_t minIndex = 0xFFFFFFFF, maxIndex = 0;
        bool rangeKnown = false;
        // The index cache (see gIndexCache): only for indices read from the game's memory.
        static const bool noIndexCache = GetEnvironmentVariableA("NIER_NO_INDEX_CACHE", nullptr, 0) > 0;
        static const bool verifyIndexCache = GetEnvironmentVariableA("NIER_VERIFY_INDEX_CACHE", nullptr, 0) > 0;
        const uint64_t indexFrame = gFrames.load();
        IndexKept* indexEntry = nullptr;
        uint64_t indexKey = 0;
        bool indexHit = false, indexStore = false;
        {
            const uint32_t count = initiator >> 16, bytes = count * (((initiator >> 11) & 1) ? 4 : 2);
            if (!noIndexCache && ((initiator >> 6) & 3) == 0 && source.base && count >= 24 && bytes <= 0x4000000)
            {
                const uint8_t* guest = g_base + PhysicalToGuest(source.base);
                const uint64_t where[4] = { uint64_t(source.base) | uint64_t(initiator) << 32, uint64_t(source.size >> 30) | uint64_t(r[0x2102]) << 32,
                                            uint64_t(r[0x2103]) | uint64_t((r[0x2205] >> 21) & 1) << 32, bytes };
                const uint64_t identity = XXH3_64bits(where, sizeof(where));
                indexEntry = &gIndexCache[identity];
                IndexKept& e = *indexEntry;
                if (indexFrame >= e.copyUntil)
                {
                    const uint32_t phase = uint32_t(indexFrame & 7);
                    auto sample = [&] {
                        uint64_t h = identity + phase;
                        for (uint32_t at = phase * 64; at + 64 <= bytes; at += 512) h = XXH3_64bits_withSeed(guest + at, 64, h);
                        return h;
                    };
                    const bool sampling = bytes >= 4096;
                    bool known = e.hashedFrame == indexFrame && e.hits >= 60;
                    if (!known && sampling && e.hits >= 60 && e.phaseMask == 0xFF && indexFrame - e.fullFrame < 600)
                    {
                        if (sample() == e.phases[phase]) { known = true; e.hashedFrame = indexFrame; }
                        else { e.hits = 0; e.phaseMask = 0; }
                    }
                    if (known) indexKey = e.contentKey;
                    else
                    {
                        indexKey = XXH3_64bits_withSeed(guest, bytes, identity) | 1;   // never zero
                        const bool same = indexKey == e.contentKey;
                        e.hits = same ? e.hits + 1 : 0;
                        if (!same) e.phaseMask = 0;
                        else if (sampling) { e.phases[phase] = sample(); e.phaseMask |= 1u << phase; }
                        e.hashedFrame = indexFrame;
                        e.fullFrame = indexFrame;
                    }
                    if (indexKey == e.contentKey && !e.list.empty())
                    {
                        indexHit = true;
                        e.misses = 0;
                        e.lastFrame = indexFrame;
                    }
                    else if (++e.misses >= 3 && e.contentKey)
                    {
                        // Indices that keep changing: not hashed or kept for two seconds.
                        e = IndexKept{};
                        e.copyUntil = indexFrame + 120;
                        e.lastFrame = indexFrame;
                    }
                    else indexStore = true;
                }
            }
        }
        if (indexHit && !verifyIndexCache)
        {
            const IndexKept& e = *indexEntry;
            draw.indices = e.list;
            draw.indicesKept = e.bornFrame + 2 <= indexFrame;
            draw.rectList = e.rectList;
            draw.strip = e.strip;
            minIndex = e.minIndex;
            maxIndex = e.maxIndex;
            rangeKnown = true;
        }
        else
        {
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
            if (indexHit)   // NIER_VERIFY_INDEX_CACHE: what was kept against what the game has now
            {
                const IndexKept& e = *indexEntry;
                static int differences = 0;
                if ((e.list.size() != draw.indices.size() || memcmp(e.list.data(), draw.indices.data(), e.list.size() * 4) || e.rectList != draw.rectList || e.strip != draw.strip) && differences++ < 20)
                    Log("[gpu] kept index list differs from the game's: frame %llu, %zu indices kept, %zu now, at %08X\n", (unsigned long long)indexFrame, e.list.size(), draw.indices.size(), source.base);
            }
        }
        if (draw.indices.empty()) return;
        draw.vertexCount = uint32_t(draw.indices.size());

        // Vertex buffers: when the shader addresses vertices with the plain vertex index, only the
        // vertices between the draw's smallest and largest index are copied (a fetch constant often
        // covers a whole level's geometry). The base is then shifted so index * stride still lands
        // on the copied bytes (unsigned wrap-around is fine).
        static const bool noTrim = GetEnvironmentVariableA("NIER_NO_VERTEX_TRIM", nullptr, 0) > 0;   // debugging
        if (!rangeKnown && ((vs->info.directVertexIndex && !noTrim) || indexStore))
            for (uint32_t index : draw.indices)
                if (index != kRestart) { minIndex = std::min(minIndex, index); maxIndex = std::max(maxIndex, index); }   // a strip keeps its restart markers
        if (indexStore)
        {
            IndexKept& e = *indexEntry;
            e.list = draw.indices;
            e.rectList = draw.rectList;
            e.strip = draw.strip;
            e.minIndex = minIndex;
            e.maxIndex = maxIndex;
            e.contentKey = indexKey;
            e.lastFrame = e.bornFrame = indexFrame;
        }
        if (!(vs->info.directVertexIndex && !noTrim)) { minIndex = 0xFFFFFFFF; maxIndex = 0; }   // the range is only used for trimming
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
        // Most of what is drawn is geometry whose vertices are the same as in the frame before
        // (levels, and characters too: they are posed on the GPU from bone textures), and copying
        // and byte-swapping all of it again for every frame was the largest part of this thread's
        // work: 32 MB a frame in a busy cutscene, which left too little of a 60 Hz frame spare
        // there and held an uncapped one to about 105 a second. So the copy made for a draw is kept,
        // by where it came from and a hash of the game's bytes there, and a later draw of the same
        // bytes, in this frame or a later one, shares it (hashing is about three times as fast as
        // the copy, and nothing is allocated). The whole range is hashed, so a buffer the game has
        // changed in any way is copied afresh. Ranges that keep changing (particles, the 2D layer)
        // are told by three misses in a row and then just copied for two seconds before being
        // tried again; small ranges are not worth it. Copies not drawn from for two seconds are
        // dropped (VertexCacheTrim, once a frame).
        // NIER_NO_VERTEX_CACHE=1: copy for every draw, as before. NIER_VERIFY_VERTEX_CACHE=1: copy
        // anyway and compare with the kept one (testing; differences are logged).
        static const bool noVertexCache = GetEnvironmentVariableA("NIER_NO_VERTEX_CACHE", nullptr, 0) > 0;
        static const bool verifyVertexCache = GetEnvironmentVariableA("NIER_VERIFY_VERTEX_CACHE", nullptr, 0) > 0;
        const uint64_t frameNow = gFrames.load();
        // NIER_VERTEX_CACHE_AB=1 (testing): on and off by turns of 300 frames, to compare within one run
        // (from one run to the next this thread's time differs by more than the cache changes it).
        static const bool alternate = GetEnvironmentVariableA("NIER_VERTEX_CACHE_AB", nullptr, 0) > 0;
        bool kept = false, keptOver = false;   // (keptOver: the copy is from an earlier frame)
        uint64_t contentKey = 0;
        VertexRange* range = nullptr;
        if (!noVertexCache && !(alternate && ((frameNow / 300) & 1)) && totalBytes >= 4096)
        {
            uint64_t identity = 0x9E3779B97F4A7C15ull;
            for (uint32_t i = 0; i < copyCount; i++)
            {
                const uint64_t where[4] = { uint64_t(uintptr_t(copies[i].source)), copies[i].first, copies[i].bytes, copies[i].endian };
                identity = XXH3_64bits_withSeed(where, sizeof(where), identity);
            }
            range = &gVertexRanges[identity];
            if (frameNow >= range->copyUntil)
            {
                const uint32_t phase = uint32_t(frameNow & 7);
                auto sample = [&] {
                    uint64_t h = identity + phase;
                    for (uint32_t i = 0; i < copyCount; i++)
                        for (uint32_t at = phase * 64; at + 64 <= copies[i].bytes; at += 512)
                            h = XXH3_64bits_withSeed(copies[i].source + copies[i].first + at, 64, h);
                    return h;
                };
                const bool sampling = totalBytes >= 16384;
                bool known = range->hashedFrame == frameNow && range->hits >= 60;
                if (!known && sampling && range->hits >= 60 && range->phaseMask == 0xFF && frameNow - range->fullFrame < 600)
                {
                    gFrameVertexHashed += totalBytes / 8;
                    if (sample() == range->phases[phase]) { known = true; range->hashedFrame = frameNow; }
                    else { range->hits = 0; range->phaseMask = 0; }
                }
                if (known) contentKey = range->hashedKey;
                else
                {
                    contentKey = identity;
                    for (uint32_t i = 0; i < copyCount; i++)
                        contentKey = XXH3_64bits_withSeed(copies[i].source + copies[i].first, copies[i].bytes, contentKey);
                    gFrameVertexHashed += totalBytes;
                    const bool same = contentKey == range->hashedKey;
                    range->hits = same ? range->hits + 1 : 0;
                    if (!same) range->phaseMask = 0;
                    else if (sampling) { range->phases[phase] = sample(); range->phaseMask |= 1u << phase; }
                    range->hashedFrame = frameNow;
                    range->hashedKey = contentKey;
                    range->fullFrame = frameNow;
                }
                auto found = gVertexCache.find(contentKey);
                if (found != gVertexCache.end() && found->second.data.size() == totalBytes)
                {
                    draw.vertexData = found->second.data;
                    found->second.lastFrame = frameNow;
                    keptOver = found->second.bornFrame + 2 <= frameNow;
                    range->misses = 0;
                    kept = true;
                    gFrameVertexKept += totalBytes;
                }
                else if (++range->misses >= 3)
                {
                    range->misses = 0;
                    range->copyUntil = frameNow + 120;
                }
            }
        }
        // Worth keeping on the GPU only once it has outlived a frame: bytes that change every frame
        // but are drawn twice in each (a posed character's shadow and scene passes) are shared too.
        draw.vertexKept = kept && keptOver;
        if (!kept)
        {
            draw.vertexData.resize(totalBytes);   // not zero-filled
            gFrameVertexCopied += totalBytes;
        }
        uint32_t written = 0;
        for (uint32_t i = 0; i < copyCount; i++)
        {
            const VertexCopy& copy = copies[i];
            if (!kept) CopySwapped(draw.vertexData.data() + written, copy.source + copy.first, copy.bytes, copy.endian);
            else if (verifyVertexCache)
            {
                static thread_local std::vector<uint8_t> check;
                check.resize(copy.bytes);
                CopySwapped(check.data(), copy.source + copy.first, copy.bytes, copy.endian);
                static int differences = 0;
                if (memcmp(check.data(), draw.vertexData.data() + written, copy.bytes) && differences++ < 20)
                    Log("[gpu] kept vertex data differs from the game's: frame %llu, %u bytes at %p+%u\n", (unsigned long long)frameNow, copy.bytes, copy.source, copy.first);
            }
            draw.vertexBase[copy.slot] = written - copy.first;
            written += copy.bytes;
        }
        if (!kept && contentKey && frameNow >= range->copyUntil)
        {
            VertexKept& entry = gVertexCache[contentKey];
            if (entry.data.size()) gVertexCacheBytes -= entry.data.size();
            entry.data = draw.vertexData;
            entry.lastFrame = frameNow;
            entry.bornFrame = frameNow;
            gVertexCacheBytes += totalBytes;
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
            {
                static std::set<uint32_t> seenSwizzles;   // which component swizzles each format uses (debugging)
                if (seenSwizzles.size() < 32 && seenSwizzles.insert(t.format << 16 | swizzle).second)
                    Log("[gpu] texture format %u (%ux%u) swizzle %c%c%c%c\n", t.format, t.width, t.height,
                        "xyzw01??"[binding.swizzle[0]], "xyzw01??"[binding.swizzle[1]], "xyzw01??"[binding.swizzle[2]], "xyzw01??"[binding.swizzle[3]]);
            }
            binding.point = ((c[3] >> 19) & 3) == 0;
            // Clamp modes (dword 0 bits 10-12 for x, 13-15 for y), mapped as Xenia does: repeat,
            // mirrored repeat, clamp to edge, mirror clamp to edge, clamp halfway (as clamp), mirror
            // clamp halfway, clamp to border, mirror clamp to border (the mirror-clamps as mirror once).
            static const uint8_t kHostAddress[8] = { 1, 2, 0, 3, 0, 3, 4, 3 };   // host: 0 clamp, 1 wrap, 2 mirror, 3 mirror once, 4 border
            binding.addressU = kHostAddress[(c[0] >> 10) & 7];
            binding.addressV = kHostAddress[(c[0] >> 13) & 7];
            if (t.height == 1)
            {
                static int logged = 0;   // one-row textures (bone matrices): how they are filtered
                if (logged++ < 2) Log("[gpu] one-row texture slot %u dimension %u format %u width %u: mag filter %u min filter %u mip filter %u, dword3 %08X\n", slot, t.dimension, t.format, t.width, (c[3] >> 19) & 3, (c[3] >> 21) & 3, (c[3] >> 23) & 3, c[3]);
            }
            binding.baseOnly = ((c[3] >> 23) & 3) == 2;   // mip filter: base map only
            // Exponent adjust (signed, 6 bits): the hardware multiplies sampled values by 2^n. The
            // soft-particle mist reads its depth texture through it; ignored, the depth compare was
            // off by that factor and the mist covered everything.
            const int expAdjust = int32_t(c[3] << 13) >> 26;
            binding.expScale = std::ldexp(1.0f, expAdjust);
            if (expAdjust)
            {
                static std::set<uint32_t> seenExp;
                if (seenExp.size() < 16 && seenExp.insert(t.format << 8 | uint32_t(expAdjust & 0x3F)).second)
                    Log("[gpu] texture format %u (%ux%u) uses exponent adjust %d\n", t.format, t.width, t.height, expAdjust);
            }
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
            if (dumpEntry != dumpShaders.end() && gLogDrawsAt && gFrames.load() >= gLogDrawsAt && dumpEntry->second < 40)
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
                    Log("[gpu] dumped draw to %s (frame %llu, t16 fetch constant dword 0 %08X, vertex fetch 95 %08X %08X)\n", path, gFrames.load(),
                        gRegisters[REG_FETCH_CONSTANTS + 16 * 6], gRegisters[REG_FETCH_CONSTANTS + 95 * 2], gRegisters[REG_FETCH_CONSTANTS + 95 * 2 + 1]);
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
        // Colour clears done as depth fills: D3D clears a colour surface by drawing a depth-only
        // rectangle (always pass, depth write) through a depth surface at the SAME EDRAM base, which
        // overwrites the colour pixels with the 24.8 value. Colour and depth are separate host
        // surfaces here, so the colour target with that base is cleared explicitly. Without this the
        // half-resolution particle layer was never wiped and the village mist piled up frame on frame.
        if (!hasColor && draw.rectList && (draw.depthControl & 0x76) == 0x76 && (r[0x2002] & 0xFFF) == (r[0x2001] & 0xFFF) && draw.vertexData.size() >= 12)
        {
            const float z = std::clamp(reinterpret_cast<const float*>(draw.vertexData.data())[2], 0.0f, 1.0f);
            const uint32_t stencil = (draw.depthControl & 1) ? (draw.stencilRefMask & 0xFF) : 0;
            RendererResolve clear;
            clear.source = draw.depth;      // same base and width as the colour surface
            clear.copyColor = false;
            clear.clearColor = true;
            clear.colorClear = (uint32_t(z * 16777215.0f) << 8) | stencil;   // read as 0xAARRGGBB
            static std::set<uint64_t> logged;
            if (logged.size() < 8 && logged.insert(uint64_t(clear.source.edramBase) << 32 | clear.source.width).second)
                Log("[gpu] depth-fill colour clear: base %X width %u value %08X\n", clear.source.edramBase, clear.source.width, clear.colorClear);
            gFrameCommands.insert(gFrameCommands.end() - 1, std::move(clear));   // before the draw being built
            building = &std::get<RendererDraw>(gFrameCommands.back());
        }
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
        RendererPreparePipeline(draw);   // start creating a new pipeline now, ahead of the render thread
        pending.keep = true;
#undef draw
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
        struct OpTimer
        {
            uint32_t opcode;
            bool on;
            LARGE_INTEGER begin{};
            OpTimer(uint32_t opcode, bool on) : opcode(opcode), on(on) { if (on) QueryPerformanceCounter(&begin); }
            ~OpTimer()
            {
                if (!on) return;
                LARGE_INTEGER end;
                QueryPerformanceCounter(&end);
                gFrameOpTicks[opcode & 255] += uint64_t(end.QuadPart - begin.QuadPart);
                gFrameOpLongest[opcode & 255] = (std::max)(gFrameOpLongest[opcode & 255], uint64_t(end.QuadPart - begin.QuadPart));
                gFrameOpCount[opcode & 255]++;
            }
        } timer(opcode, gLogPacing && opcode != PM4_INDIRECT_BUFFER && opcode != PM4_INDIRECT_BUFFER_PFD && opcode != PM4_WAIT_REG_MEM);

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
            Progress();
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
                Progress();
            }
            break;
        }
        case PM4_WAIT_REG_MEM:
        {
            uint32_t waitInfo = s.Next(), poll = s.Next(), reference = s.Next(), mask = s.Next();
            LARGE_INTEGER waitBegin, waitEnd;
            QueryPerformanceCounter(&waitBegin);
            for (DWORD start = GetTickCount(); !Compare(waitInfo, PollValue(waitInfo, poll) & mask, reference); SwitchToThread())
            {
                if (GetTickCount() - start > 200)
                {
                    if (gWaitTimeoutsLogged++ < 8) Log("[gpu] WAIT_REG_MEM gave up: info 0x%X poll 0x%X ref 0x%X mask 0x%X\n", waitInfo, poll, reference, mask);
                    break;
                }
            }
            QueryPerformanceCounter(&waitEnd);
            gFrameWaitTicks += uint64_t(waitEnd.QuadPart - waitBegin.QuadPart);
            {
                // Per distinct wait (condition and polled location): how often and how long (GpuDumpStats).
                std::lock_guard lock(gStatsMutex);
                WaitStats& stats = gWaitStats[(uint64_t(waitInfo) << 32) | poll];
                stats.count++;
                stats.ticks += uint64_t(waitEnd.QuadPart - waitBegin.QuadPart);
                stats.lastReference = reference;
                stats.mask = mask;
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
            NoteFrameTiming();
            {
                // NIER_POKE_F32=<guest address hex>:<value>[,...]: writes those floats into the game's
                // memory every frame (testing what a value is for).
                static const std::vector<std::pair<uint32_t, float>> pokes = [] {
                    std::vector<std::pair<uint32_t, float>> list;
                    char value[1024];
                    if (GetEnvironmentVariableA("NIER_POKE_F32", value, sizeof(value)))
                        for (char* item = strtok(value, ","); item; item = strtok(nullptr, ","))
                            if (const char* colon = strchr(item, ':')) list.push_back({ uint32_t(strtoul(item, nullptr, 16)), strtof(colon + 1, nullptr) });
                    return list;
                }();
                for (const auto& [address, number] : pokes)
                {
                    uint32_t bits;
                    memcpy(&bits, &number, 4);
                    W32(address, bits);
                }
            }
            VertexCacheTrim(frame);
            if (frame == gCaptureFrame + 1)
            {
                LogSetCaptureFile(nullptr);
                Log("[capture] frame %llu captured\n", gCaptureFrame);
                gCaptureFrame = ~0ull;
            }
            {
                // NIER_CAPTURE_AT=<frame>: the same as pressing F12 at that frame (test runs).
                static const uint64_t captureAt = [] {
                    char value[32];
                    return GetEnvironmentVariableA("NIER_CAPTURE_AT", value, sizeof(value)) ? strtoull(value, nullptr, 10) : ~0ull;
                }();
                if (frame == captureAt) gCaptureRequested = true;
                // NIER_RECORD_AT=<frame>[,<seconds>]: the same as pressing F9 at that frame.
                static const std::pair<uint64_t, double> recordAt = [] {
                    char value[32];
                    std::pair<uint64_t, double> at{ ~0ull, 10.0 };
                    if (GetEnvironmentVariableA("NIER_RECORD_AT", value, sizeof(value)))
                    {
                        at.first = strtoull(value, nullptr, 10);
                        if (const char* comma = strchr(value, ',')) at.second = atof(comma + 1);
                    }
                    return at;
                }();
                if (frame == recordAt.first || gRecordingRequested.exchange(false))
                {
                    CreateDirectoryA("../captures", nullptr);
                    char path[96];
                    snprintf(path, sizeof(path), "../captures/seq_f%llu.nseq", frame);
                    FrameSequenceStart(path, frame == recordAt.first ? recordAt.second : 10.0, ShaderWords);
                }
                // NIER_RES_AT=<frame>,<index>[;...]: sets the Resolution setting at those frames, as the
                // menu would (test runs).
                static const std::map<uint64_t, int> resolutionAt = [] {
                    char value[256];
                    std::map<uint64_t, int> at;
                    if (GetEnvironmentVariableA("NIER_RES_AT", value, sizeof(value)))
                        for (char* item = strtok(value, ";"); item; item = strtok(nullptr, ";"))
                            if (const char* comma = strchr(item, ',')) at[strtoull(item, nullptr, 10)] = atoi(comma + 1);
                    return at;
                }();
                if (auto change = resolutionAt.find(frame); change != resolutionAt.end()) Settings().resolutionScale = change->second;
                CoverTick(frame);   // NIER_COVER (cover.cpp)
                // NIER_FPS_AT=<frame>,<index>: sets the Frame Rate setting at that frame (test runs: the
                // scripted route to a save is timed for 60 fps).
                static const std::pair<uint64_t, int> rateAt = [] {
                    char value[64];
                    if (!GetEnvironmentVariableA("NIER_FPS_AT", value, sizeof(value)) || !strchr(value, ',')) return std::pair<uint64_t, int>{ ~0ull, 0 };
                    return std::pair<uint64_t, int>{ strtoull(value, nullptr, 10), atoi(strchr(value, ',') + 1) };
                }();
                if (frame == rateAt.first) Settings().frameRate60 = rateAt.second;
            }
            if (gCaptureRequested.exchange(false))
            {
                // The frame starting now is logged draw by draw and its image saved.
                gCaptureFrame = frame;
                gSaveCommandsAt = frame + 1;   // its commands are submitted at the next swap
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
            const uint32_t stage = addressType & 3;
            StoreShader(stage, std::move(words));
            if (stage <= 1)
            {
                static std::set<uint64_t> logged;   // where each shader's microcode lives (debugging)
                if (logged.insert(gShaders[stage].hash).second)
                    Log("[gpu] %s shader %016llX microcode at %08X, %u dwords (frame %llu)\n", stage ? "pixel" : "vertex", gShaders[stage].hash, guest, startSize & 0xFFFF, gFrames.load());
            }
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
        // The game often waits for the GPU to pass a marker right after submitting it, so new commands
        // are picked up by polling (yielding) for a few milliseconds after the last work; only a longer
        // idle stretch sleeps. Sleep(1) wakes 1-2 ms late, which used to push 60 fps frames past their vblank.
        LARGE_INTEGER frequency, lastWork, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&lastWork);
        const int64_t pollTicks = frequency.QuadPart * 4 / 1000;
        while (true)
        {
            uint32_t write = R32(MMIO_BASE + REG_CP_RB_WPTR * 4) & (gRingDwords - 1);
            if (write == gReadIndex)
            {
                LARGE_INTEGER before;
                QueryPerformanceCounter(&before);
                if (before.QuadPart - lastWork.QuadPart > pollTicks) Sleep(1);
                else SwitchToThread();
                QueryPerformanceCounter(&now);
                gFrameIdleTicks += uint64_t(now.QuadPart - before.QuadPart);
                continue;
            }
            ExecuteStream({ gRingBase, gRingDwords, gReadIndex, write }, 0);
            gReadIndex = write;
            W32(MMIO_BASE + REG_CP_RB_RPTR * 4, gReadIndex);
            if (gWritebackAddress) W32(gWritebackAddress, gReadIndex);
            Progress();
            QueryPerformanceCounter(&lastWork);
        }
    }

    // Uncapped frame rate: there is no fixed vblank; one is raised as soon as the game queues a flip
    // (QueueFlipEveryVblank sets this), and at least 60 a second otherwise.
    HANDLE gVblankKick = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::atomic<uint32_t> gFlipsQueued{ 0 };   // flips the game has queued (QueueFlipEveryVblank)

    void VblankThread()
    {
        PPCContext ctx;
        uint32_t pcr = AttachGuestThread(ctx);
        LARGE_INTEGER frequency, next;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next);
        // 60 Hz exactly, as the console's HD video modes are (not NTSC's 59.94): on a PC display the
        // game then keeps step with the refresh instead of falling a frame behind every 17 seconds.
        while (true)
        {
            const int rate = GuestRefreshHz();   // 60 Hz, or the Frame Rate setting's; 0 unlocked
            if (rate == 0)
            {
                // Uncapped: a vblank as soon as a flip is queued, but no sooner after the last one than
                // one refresh of the display with VSync on (a limiter at the display's rate), or a
                // millisecond with it off (menus would run at thousands of frames a second). Nothing
                // here waits for the display's own refreshes: that made every late frame cost a whole
                // refresh (240, 120, 80...), where a variable refresh display shows it when it is ready.
                // The Frame Rate setting's own limit (90, 120, 144, 240) works the same way: the larger
                // of the two gaps applies.
                int64_t gap = Settings().vsync ? frequency.QuadPart / std::max<int64_t>(RendererDisplayHz(), 30) : frequency.QuadPart / 1000;
                if (const int cap = FrameRateCap()) gap = std::max<int64_t>(gap, frequency.QuadPart / cap);
                // The earliest moment for the next vblank steps on by the gap from the one before, so the
                // rate averages the limit exactly; after a pause it starts again from now.
                static int64_t earliest = 0;
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                earliest += gap;
                if (earliest < now.QuadPart - gap) earliest = now.QuadPart;
                for (;;)
                {
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart >= earliest) break;
                    if (earliest - now.QuadPart > frequency.QuadPart * 2 / 1000) Sleep(1);
                    else SwitchToThread();
                }
                WaitForSingleObject(gVblankKick, 16);
                QueryPerformanceCounter(&next);
                W32(MMIO_BASE + REG_VBLANK_STATUS * 4, 1);
                gVblanks++;
                DispatchInterrupt(ctx, pcr, 0, 2);
                continue;
            }
            const int64_t period = frequency.QuadPart / rate;
            // Sleep until about 2 ms before the vblank, then yield until it: Sleep(1) alone made
            // vblanks up to a millisecond late, and late vblanks delay 60 fps flips.
            next.QuadPart += period;
            const int64_t margin = frequency.QuadPart * 2 / 1000;
            // A late frame is not held for the next tick. The game flips on a vblank, so a frame that
            // took 17 ms waited for the tick at 33: any scene a little too heavy for 60 ran at exactly
            // 30 (the console's double-buffered vsync; on the Steam Deck the village overview sat at
            // 30-33 fps with frames 33 ms apart). When the last vblank found no new flip and one is
            // queued now, the vblank comes at once and the ticks carry on from there, so such a scene
            // runs at the rate it can manage. Only on the 60 fps setting and only with this timer: a
            // display whose own refreshes are followed (above) keeps to them.
            // NIER_FIXED_VBLANK=1: ticks only, as before.
            static const bool fixedTicks = GetEnvironmentVariableA("NIER_FIXED_VBLANK", nullptr, 0) > 0;
            static uint32_t flipsSeen = 0;
            static bool lastEmpty = false;
            const bool adaptive = !fixedTicks && rate == 60 && Settings().frameRate60 == 1;
            if (RendererWaitDisplayVblank()) QueryPerformanceCounter(&next);   // in step with the display
            else for (LARGE_INTEGER now;;)
            {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= next.QuadPart) break;
                if (adaptive && lastEmpty && gFlipsQueued.load() != flipsSeen) { next = now; break; }
                if (adaptive && lastEmpty) WaitForSingleObject(gVblankKick, 1);
                else if (next.QuadPart - now.QuadPart > margin) Sleep(1);
                else SwitchToThread();
            }
            {
                const uint32_t flips = gFlipsQueued.load();
                lastEmpty = flips == flipsSeen;
                flipsSeen = flips;
            }
            W32(MMIO_BASE + REG_VBLANK_STATUS * 4, 1);
            gVblanks++;
            // NIER_LOG_PACING=1: a vblank that came late, or whose handler in the game took long.
            static const bool logPacing = GetEnvironmentVariableA("NIER_LOG_PACING", nullptr, 0) > 0;
            static LARGE_INTEGER lastVblank{};
            LARGE_INTEGER before, after;
            QueryPerformanceCounter(&before);
            DispatchInterrupt(ctx, pcr, 0, 2);
            if (logPacing)
            {
                QueryPerformanceCounter(&after);
                const double k = 1000.0 / double(frequency.QuadPart);
                const double gap = lastVblank.QuadPart ? double(before.QuadPart - lastVblank.QuadPart) * k : 0.0, handler = double(after.QuadPart - before.QuadPart) * k;
                if (gap > 1500.0 / rate || handler > 3.0)
                    Log("[pace] vblank %llu (frame %llu): %.1f ms after the one before, its handler took %.1f ms\n", (unsigned long long)gVblanks.load(), (unsigned long long)gFrames.load(), gap, handler);
                lastVblank = before;
            }
        }
    }

    void WriteVideoMode(uint32_t mode)
    {
        // NIER_VIDEO_HZ=<rate> reports another refresh rate to the game (testing how it times itself).
        static const float forcedRefresh = [] { char v[16]; return GetEnvironmentVariableA("NIER_VIDEO_HZ", v, sizeof(v)) ? float(atof(v)) : 0.0f; }();
        float refresh = forcedRefresh > 0.0f ? forcedRefresh : 60.0f;
        uint32_t refreshBits;
        memcpy(&refreshBits, &refresh, 4);
        W32(mode + 0x00, 1280);
        W32(mode + 0x04, 720);
        W32(mode + 0x08, 0);        // progressive
        // NIER_VIDEO_WIDE=0 reports a 4:3 display (testing what the game does with the picture's shape).
        static const uint32_t wide = [] { char v[8]; return GetEnvironmentVariableA("NIER_VIDEO_WIDE", v, sizeof(v)) ? uint32_t(atoi(v)) : 1u; }();
        W32(mode + 0x0C, wide);     // widescreen
        W32(mode + 0x10, 1);        // high definition
        W32(mode + 0x14, refreshBits);
        W32(mode + 0x18, 1);        // NTSC
        W32(mode + 0x1C, 0x8A);
        W32(mode + 0x20, 1);
    }
}

uint64_t GpuFrameCount() { return gFrames.load(); }
double GpuCommandWorkMs() { return gCommandWorkMs.load(); }

void GpuResetFrameTiming()
{
    std::lock_guard lock(gStatsMutex);
    gOnTime = {};
    gLate = {};
    for (auto& bin : gFrameTimeBins) bin = 0;
    gWaitStats.clear();
}

// 60 fps (the PC Settings page's Frame Rate; NIER_60FPS / NIER_30FPS override): D3D queues each flip from the swap interrupt (sub_8257A4B0, reached only
// through the indirect call table). Its swap word carries the present interval in bits 8-11 (2 in
// 3D scenes: 30 fps); the flip may happen that many vblanks after the previous one, and the GPU waits
// for it. An interval of 1 lets 3D scenes present every vblank, like the Series X's FPS Boost.
extern "C" PPC_FUNC(__imp__sub_8257A4B0);
namespace
{
    PPC_FUNC(QueueFlipEveryVblank)
    {
        const uint32_t word = ctx.r3.u32, interval = (word >> 8) & 0xF;
        static std::atomic<int> logged{ 0 };
        if (interval > 1 && Settings().frameRate60)
        {
            if (logged++ == 0) Log("[gpu] 60 fps: present interval %u lowered to 1\n", interval);
            ctx.r3.u64 = (word & ~0xF00u) | 0x100u;
        }
        __imp__sub_8257A4B0(ctx, base);
        gFlipsQueued++;
        SetEvent(gVblankKick);   // unlocked: the flip's vblank comes at once; 60 fps: a late frame's does (VblankThread)
    }
}

void GpuInstallHooks()
{
    PPC_LOOKUP_FUNC(g_base, 0x8257A4B0) = QueueFlipEveryVblank;   // checks the Frame Rate setting on every flip
    static const char* const kNames[] = { "30 fps cap", "60 fps", "90 fps", "120 fps", "144 fps", "240 fps", "uncapped" };
    Log("[gpu] frame rate: %s\n", kNames[std::clamp(Settings().frameRate60.load(), 0, 6)]);
}

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
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const auto ms = [&](uint64_t ticks, uint64_t frames) { return frames ? double(ticks) * 1000.0 / frequency.QuadPart / frames : 0.0; };
        Log("[gpu] frame times in vblanks: <1.25: %llu, 1.25-1.75: %llu, 1.75-2.25: %llu, 2.25-2.75: %llu, 2.75-3.5: %llu, more: %llu\n",
            gFrameTimeBins[0], gFrameTimeBins[1], gFrameTimeBins[2], gFrameTimeBins[3], gFrameTimeBins[4], gFrameTimeBins[5]);
        for (const auto* bucket : { &gOnTime, &gLate })
            Log("[gpu] %s frames: %llu, average %.2f ms = %.2f waiting (WAIT_REG_MEM) + %.2f idle (ring empty) + %.2f working\n", bucket == &gOnTime ? "on-time" : "late",
                (unsigned long long)bucket->frames, ms(bucket->total, bucket->frames), ms(bucket->wait, bucket->frames), ms(bucket->idle, bucket->frames),
                ms(bucket->total - bucket->wait - bucket->idle, bucket->frames));
        for (const auto& [key, stats] : gWaitStats)
            Log("[gpu] WAIT_REG_MEM info 0x%X poll 0x%X (last ref 0x%X mask 0x%X): %llu waits, %.1f ms total\n", uint32_t(key >> 32), uint32_t(key),
                stats.lastReference, stats.mask, (unsigned long long)stats.count, double(stats.ticks) * 1000.0 / frequency.QuadPart);
    }
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

// Mid-asm hook at the top of the D3D library's spin (sub_82579D50), reached only when what it waits
// for has not happened yet: sleeps until the command thread gets further, at most a millisecond
// (the loop has other ways out, and its own time-out). NIER_SPIN_WAIT=1 leaves the spin as it was.
void GpuSpinWait()
{
    static const bool spin = GetEnvironmentVariableA("NIER_SPIN_WAIT", nullptr, 0) > 0;
    if (spin) return;
    thread_local uint32_t seen = 0;
    gProgressWaiters++;
    WaitOnAddress(&gProgress, &seen, sizeof(seen), 1);
    gProgressWaiters--;
    seen = gProgress.load();
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
void GpuRequestRecording() { gRecordingRequested = true; }

// Writes a resolve's pixels into guest memory the way the copy hardware lays out a 32-bit colour
// destination: tiled with the destination pitch, red/blue swapped if requested, then endian-swapped.
void GpuWriteResolvedPixels(const RendererResolve& resolve, const uint8_t* rgba, uint32_t rowPitch)
{
    static std::set<uint32_t> loggedFormats;
    if (loggedFormats.size() < 8 && loggedFormats.insert(resolve.destFormat).second)
        Log("[gpu] writing resolves back to guest memory (destination format %u, endian %u, e.g. %08X %ux%u)\n", resolve.destFormat, resolve.destEndian, resolve.destBase, resolve.width, resolve.height);
    // NIER_LOG_WRITEBACK=1: which guest frame is being built when each read-back arrives, and its average.
    static const bool logged = GetEnvironmentVariableA("NIER_LOG_WRITEBACK", nullptr, 0) > 0;
    if (logged && resolve.width * resolve.height <= 64)
    {
        uint32_t sum[4] = {};
        for (uint32_t y = 0; y < resolve.height; y++)
            for (uint32_t x = 0; x < resolve.width; x++)
                for (int k = 0; k < 4; k++) sum[k] += rgba[size_t(y) * rowPitch + x * 4 + k];
        const float n = float(resolve.width * resolve.height);
        Log("[writeback] %08X %ux%u arrives while guest frame %llu is built: average %.1f %.1f %.1f %.1f\n", resolve.destBase, resolve.width, resolve.height,
            (unsigned long long)gFrames.load() + 1, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
    }
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
