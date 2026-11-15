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
//   P <vs hash> <ps hash> <blend> <write mask> <depth control> <stencil masks> <flags>
//
// Everything is queued behind what the game itself needs (renderer: urgent jobs go first).
// NIER_NO_PREWARM=1 turns it off.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "guest.h"
#include "renderer.h"
#include "xenos_shader.h"

namespace
{
    void PrewarmThread()
    {
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
        while (fgets(line.data(), int(line.size()), list))
        {
            if (line[0] == 'S')
            {
                char stage[8], source[260];
                unsigned long long hash, offset;
                unsigned count;
                int consumed = 0;
                if (sscanf(line.data(), "S %7s %llx %259s %llx %u%n", stage, &hash, source, &offset, &count, &consumed) != 5 || count == 0 || count > 0x4000) continue;
                shaderLines++;
                const bool pixel = !strcmp(stage, "ps");
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
                for (const char* patch = line.data() + consumed; *patch;)
                {
                    unsigned index, value;
                    int used = 0;
                    if (sscanf(patch, " %u:%x%n", &index, &value, &used) != 2) break;
                    if (index < count) words[index] = value;
                    patch += used;
                    if (*patch == ',') patch++;
                }
                uint64_t actual = 1469598103934665603ull;   // as gpu.cpp StoreShader names shaders
                for (uint32_t word : words) actual = (actual ^ word) * 1099511628211ull;
                if (actual != hash) { mismatched++; continue; }
                TranslatedShader translated = TranslateShader(words, pixel, hash);
                if (!translated.ok) { untranslated++; continue; }
                auto shader = std::make_shared<GuestShader>();
                shader->hash = hash;
                shader->hlsl = std::move(translated.hlsl);
                shader->pixel = pixel;
                shaders[pixel ? 1 : 0][hash] = shader;
                RendererPrewarmShader(shader);
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
    }
}

// Called once the game's image is loaded.
void PrewarmStart()
{
    if (GetEnvironmentVariableA("NIER_NO_PREWARM", nullptr, 0) > 0) return;
    std::thread(PrewarmThread).detach();
}
