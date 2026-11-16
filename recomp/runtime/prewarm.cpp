// Ahead-of-time shader and pipeline creation from the pipeline list (recomp/config/pipeline_list.txt,
// written by tools/pipeline_list from what earlier play recorded in recomp/pipelines_seen.txt).
//
// The first time the game draws with a new combination of vertex shader, pixel shader and render
// state, the HLSL has to be compiled and the driver has to build a pipeline: 20-100 ms each, a hitch.
// The list names those combinations. It holds no shader code: each shader line says where its
// microcode is in the player's own copy of the game (a file and offset, or an address in the loaded
// executable), how long it is, the words the console's D3D patches into it at runtime (vertex fetch
// instructions, which depend on the model's vertex layout), and the hash the result must have. A
// shader whose bytes don't hash to that is skipped, so a different release of the game does no harm.
//
//   S <vs|ps> <hash> <source> <offset hex> <dwords> [<index>:<value hex>,...]
//   Z <vs|ps> <hash> <source> <container offset hex> <offset in its unpacked data hex> <dwords> [...]
//     (a shader inside one of the archives' "lzo" containers, lzo1x.h: most model files are)
//   P <vs hash> <ps hash> <blend> <write mask> <depth control> <stencil masks> <flags>
//
// Shader lines that no P line uses are pixel shaders the files hold but nobody has drawn yet
// (tools/pipeline_list lists every one it finds). They are compiled once, the first time the game
// starts, so that meeting one in play costs no compile: a note in the shader cache folder
// (prepared_<hash of the list>.txt) says it has been done, and later starts leave them alone, since
// the compiled shader is then read from the cache when it is first drawn. NIER_PREWARM_ALL=1 does
// them regardless.
//
// Everything is queued behind what the game itself needs (renderer: urgent jobs go first).
// NIER_NO_PREWARM=1 turns it off.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "guest.h"
#include "lzo1x.h"
#include "renderer.h"
#include "xenos_shader.h"

namespace
{
    std::atomic<bool> gListQueued{ false };   // everything on the list has been handed to the renderer (or there is no list)
    std::string gPreparedNote;                // set when the undrawn shaders were queued too: PrewarmWait writes it once they are done

    void PrewarmThread()
    {
        struct Done { ~Done() { gListQueued = true; } } done;
        while (!RendererReady()) Sleep(50);
        FILE* list = fopen("..\\config\\pipeline_list.txt", "r");
        if (!list) return;
        LARGE_INTEGER frequency, begin, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);

        std::map<std::string, FILE*> files;
        std::unordered_map<uint64_t, std::shared_ptr<GuestShader>> shaders[2];   // by stage (0 vertex, 1 pixel) and hash
        uint32_t shaderLines = 0, unreadable = 0, mismatched = 0, untranslated = 0, pipelines = 0, pipelinesSkipped = 0;
        std::vector<char> line(1 << 16);

        // First the pipelines: the shaders they use are wanted on every start, the rest only once.
        std::unordered_set<uint64_t> used[2];
        uint64_t listHash = 1469598103934665603ull;
        while (fgets(line.data(), int(line.size()), list))
        {
            for (const char* c = line.data(); *c; c++) listHash = (listHash ^ uint8_t(*c)) * 1099511628211ull;
            unsigned long long vs, ps;
            if (line[0] == 'P' && sscanf(line.data(), "P %llx %llx", &vs, &ps) == 2) { used[0].insert(vs); used[1].insert(ps); }
        }
        rewind(list);
        char note[96];
        snprintf(note, sizeof(note), "..\\shadercache\\prepared_%016llX.txt", (unsigned long long)listHash);
        const bool undrawnToo = GetFileAttributesA(note) == INVALID_FILE_ATTRIBUTES || GetEnvironmentVariableA("NIER_PREWARM_ALL", nullptr, 0) > 0;
        uint32_t undrawn = 0;

        std::map<std::string, uint32_t> reasons;   // why shaders could not be translated

        // The words as stored, then: the list's patches, the hash check, translation, and the compile queued.
        const auto place = [&](std::vector<uint32_t>& words, const char* patch, bool pixel, uint64_t hash) {
            while (*patch)
            {
                unsigned index, value;
                int used = 0;
                if (sscanf(patch, " %u:%x%n", &index, &value, &used) != 2) break;
                if (index < words.size()) words[index] = value;
                patch += used;
                if (*patch == ',') patch++;
            }
            uint64_t actual = 1469598103934665603ull;   // as gpu.cpp StoreShader names shaders
            for (uint32_t word : words) actual = (actual ^ word) * 1099511628211ull;
            if (actual != hash) { mismatched++; return; }
            TranslatedShader translated = TranslateShader(words, pixel, hash);
            if (!translated.ok) { untranslated++; reasons[translated.error]++; return; }
            auto shader = std::make_shared<GuestShader>();
            shader->hash = hash;
            shader->hlsl = std::move(translated.hlsl);
            shader->pixel = pixel;
            shaders[pixel ? 1 : 0][hash] = shader;
            RendererPrewarmShader(shader);
        };

        // lzo containers (lzo1x.h): the block headers of each one used, read once, and the block
        // unpacked last (the list is in file order, so neighbouring shaders share it).
        struct Container { uint32_t slot = 0; std::vector<LzoBlock> blocks; };
        std::map<std::pair<std::string, uint64_t>, Container> containers;
        const Container* unpackedFrom = nullptr;
        size_t unpackedBlock = 0;
        std::vector<uint8_t> unpacked, packed;
        const auto readUnpacked = [&](FILE* file, const char* source, uint64_t containerAt, uint64_t offset, std::vector<uint8_t>& out) {
            auto [found, added] = containers.try_emplace({ source, containerAt });
            Container& container = found->second;
            if (added)
            {
                uint8_t header[0x20];
                if (_fseeki64(file, int64_t(containerAt), SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "lzo", 4)) return false;
                container.slot = LzoBe32(header + 8);
                const uint32_t count = LzoBe32(header + 12);
                if (container.slot == 0 || count == 0 || count > 0x4000) return false;
                for (uint32_t b = 0; b < count; b++)
                {
                    uint8_t blockHeader[12];
                    const uint64_t at = LzoBlockHeaderAt(containerAt, container.slot, b);
                    if (_fseeki64(file, int64_t(at), SEEK_SET) != 0 || fread(blockHeader, 1, 12, file) != 12) break;
                    LzoBlock block;
                    block.dataAt = at + 12;
                    block.offset = LzoBe32(blockHeader); block.unpacked = LzoBe32(blockHeader + 4); block.packed = LzoBe32(blockHeader + 8);
                    if (block.unpacked > (64u << 20) || block.packed > container.slot) break;
                    container.blocks.push_back(block);
                }
            }
            size_t done = 0;
            while (done < out.size())
            {
                const uint64_t at = offset + done;
                size_t index = 0;
                while (index < container.blocks.size() && !(at >= container.blocks[index].offset && at < uint64_t(container.blocks[index].offset) + container.blocks[index].unpacked)) index++;
                if (index == container.blocks.size()) return false;
                const LzoBlock& block = container.blocks[index];
                if (unpackedFrom != &container || unpackedBlock != index)
                {
                    unpackedFrom = nullptr;
                    packed.resize(block.packed);
                    unpacked.resize(block.unpacked);
                    if (_fseeki64(file, int64_t(block.dataAt), SEEK_SET) != 0 || fread(packed.data(), 1, block.packed, file) != block.packed ||
                        !LzoUnpackBlock(block, packed.data(), unpacked.data())) return false;
                    unpackedFrom = &container;
                    unpackedBlock = index;
                }
                const size_t inBlock = size_t(at - block.offset), take = (std::min)(out.size() - done, size_t(block.unpacked) - inBlock);
                memcpy(out.data() + done, unpacked.data() + inBlock, take);
                done += take;
            }
            return true;
        };

        while (fgets(line.data(), int(line.size()), list))
        {
            if (line[0] == 'S')
            {
                char stage[8], source[260];
                unsigned long long hash, offset;
                unsigned count;
                int consumed = 0;
                if (sscanf(line.data(), "S %7s %llx %259s %llx %u%n", stage, &hash, source, &offset, &count, &consumed) != 5 || count == 0 || count > 0x4000) continue;
                const bool pixel = !strcmp(stage, "ps");
                if (!used[pixel ? 1 : 0].count(hash)) { if (!undrawnToo) continue; undrawn++; }
                shaderLines++;
                std::vector<uint32_t> words(count);
                if (!strcmp(source, "image"))
                {
                    // In the loaded executable: `offset` is a guest address.
                    if (offset < 0x82000000ull || offset + uint64_t(count) * 4 > 0x90000000ull) { unreadable++; continue; }
                    for (unsigned i = 0; i < count; i++) words[i] = R32(uint32_t(offset) + i * 4);
                }
                else
                {
                    auto opened = files.find(source);
                    if (opened == files.end()) opened = files.emplace(source, fopen((g_gameDir + "\\" + source).c_str(), "rb")).first;
                    FILE* file = opened->second;
                    if (!file || _fseeki64(file, int64_t(offset), SEEK_SET) != 0 || fread(words.data(), 4, count, file) != count) { unreadable++; continue; }
                    for (uint32_t& word : words) word = __builtin_bswap32(word);   // stored big-endian
                }
                place(words, line.data() + consumed, pixel, hash);
            }
            else if (line[0] == 'Z')
            {
                // Inside an lzo container: where the container starts in the file, and where the
                // shader is in the container's unpacked data.
                char stage[8], source[260];
                unsigned long long hash, containerAt, offset;
                unsigned count;
                int consumed = 0;
                if (sscanf(line.data(), "Z %7s %llx %259s %llx %llx %u%n", stage, &hash, source, &containerAt, &offset, &count, &consumed) != 6 || count == 0 || count > 0x4000) continue;
                if (!used[strcmp(stage, "ps") ? 0 : 1].count(hash)) { if (!undrawnToo) continue; undrawn++; }
                shaderLines++;
                auto opened = files.find(source);
                if (opened == files.end()) opened = files.emplace(source, fopen((g_gameDir + "\\" + source).c_str(), "rb")).first;
                FILE* file = opened->second;
                std::vector<uint8_t> bytes(size_t(count) * 4);
                if (!file || !readUnpacked(file, source, containerAt, offset, bytes)) { unreadable++; continue; }
                std::vector<uint32_t> words(count);
                for (unsigned i = 0; i < count; i++) words[i] = LzoBe32(&bytes[i * 4]);
                place(words, line.data() + consumed, !strcmp(stage, "ps"), hash);
            }
            else if (line[0] == 'P')
            {
                unsigned long long vs, ps;
                unsigned blend, mask, depth, stencil, flags;
                if (sscanf(line.data(), "P %llx %llx %x %x %x %x %x", &vs, &ps, &blend, &mask, &depth, &stencil, &flags) != 7) continue;
                auto vertex = shaders[0].find(vs), pixel = shaders[1].find(ps);
                if (vertex == shaders[0].end() || pixel == shaders[1].end()) { pipelinesSkipped++; continue; }
                RendererPrewarmPipeline(vertex->second, pixel->second, blend, mask, depth, stencil, flags);
                pipelines++;
            }
        }
        fclose(list);
        for (auto& [name, file] : files) if (file) fclose(file);
        QueryPerformanceCounter(&end);
        Log("[prewarm] pipeline list: %zu of %u shaders and %u pipelines queued in %.0f ms (%u unreadable, %u different from the list, %u untranslated; %u pipelines without their shaders)\n",
            shaders[0].size() + shaders[1].size(), shaderLines, pipelines, double(end.QuadPart - begin.QuadPart) * 1000.0 / double(frequency.QuadPart),
            unreadable, mismatched, untranslated, pipelinesSkipped);
        for (const auto& [reason, count] : reasons) Log("[prewarm]   %u untranslated: %s\n", count, reason.c_str());
        if (undrawn) { Log("[prewarm] %u of those shaders have not been drawn yet: compiled this once\n", undrawn); gPreparedNote = note; }
    }
}

// Called once the game's image is loaded.
void PrewarmStart()
{
    if (GetEnvironmentVariableA("NIER_NO_PREWARM", nullptr, 0) > 0) { gListQueued = true; return; }
    std::thread(PrewarmThread).detach();
}

// Holds the game's start until everything on the list is built, so nothing on it is compiled
// during play. With warm caches (the game's compiled shaders and the driver's pipelines) that is
// well under a second and nothing is shown; otherwise, the first time and after a driver update,
// a "Preparing shaders" picture with a bar. NIER_NO_PREWARM_WAIT=1: don't wait (the list is then
// built in the background while the game runs, as before). NIER_PREWARM_SHOW=<ms>: the picture stays
// at least that long, whatever the caches hold (to look at it).
void PrewarmWait()
{
    if (GetEnvironmentVariableA("NIER_NO_PREWARM_WAIT", nullptr, 0) > 0) return;
    const ULONGLONG begin = GetTickCount64();
    char show[16];
    const ULONGLONG showFor = GetEnvironmentVariableA("NIER_PREWARM_SHOW", show, sizeof(show)) ? ULONGLONG(atoi(show)) : 0;
    while (!RendererReady()) { if (GetTickCount64() - begin > 20000) return; Sleep(10); }
    RendererPrewarmBoost(true);
    std::vector<uint8_t> rgba;
    uint32_t shownDone = UINT32_MAX, lastDone = 0;
    uint64_t frame = 0;
    bool shown = false;
    ULONGLONG progressAt = GetTickCount64();
    while (true)
    {
        uint32_t done = 0, total = 0;
        RendererPrewarmProgress(done, total);
        if (gListQueued && done >= total && GetTickCount64() - begin >= showFor) break;
        const ULONGLONG now = GetTickCount64();
        if (done != lastDone) { lastDone = done; progressAt = now; }
        if (now - progressAt > 30000) { Log("[prewarm] nothing finished for 30 s: starting the game without the rest\n"); gPreparedNote.clear(); break; }
        // Nothing is shown for the first 0.7 s: warm caches are done by then.
        if (now - begin > 700 || showFor)
        {
            if (done != shownDone)
            {
                uint32_t width = 0, height = 0;
                if (DrawProgressScreen(done, total, rgba, width, height)) RendererSetOverlay(rgba.data(), width, height);
                shownDone = done;
                shown = true;
            }
            RendererFrame empty;   // a black picture under the overlay
            empty.frame = frame++;
            RendererSubmitFrame(std::move(empty));
        }
        Sleep(30);
    }
    RendererPrewarmBoost(false);
    if (shown)
    {
        RendererSetOverlay(nullptr, 0, 0);
        RendererFrame empty;
        empty.frame = frame++;
        RendererSubmitFrame(std::move(empty));
    }
    uint32_t done = 0, total = 0;
    RendererPrewarmProgress(done, total);
    Log("[prewarm] %u of %u shaders and pipelines ready after %llu ms%s\n", done, total, GetTickCount64() - begin, shown ? " (progress shown)" : "");
    if (!gPreparedNote.empty())
    {
        CreateDirectoryA("..\\shadercache", nullptr);
        if (FILE* file = fopen(gPreparedNote.c_str(), "w")) { fprintf(file, "Every shader on the pipeline list with this hash has been compiled into this folder.\n"); fclose(file); }
    }
}
