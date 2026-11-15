// Builds recomp/config/pipeline_list.txt, the list the runtime replays at boot to compile shaders and
// create pipelines ahead of use (recomp/runtime/prewarm.cpp describes the format).
//
// Input: recomp/pipelines_seen.txt (every vertex shader / pixel shader / render state combination
// the game has used on this machine) and recomp/shaders/*.bin (the microcode of every shader seen,
// host-order dwords). Each shader is looked for in the game's own files: the archives, and the
// loaded executable image (recomp/captures/image.bin, from NIER_DUMP_IMAGE=1). The list records
// where it was found, never the code itself. Vertex shaders are stored with their vertex fetch
// instructions left blank and filled in by the console's D3D at runtime, so a stored copy may
// differ from what was captured in some words: those words are recorded as patches.
//
// Usage: pipeline_list <seen.txt> <shader dir> <output> <game dir> <image.bin or -> <archive relative to game dir>...
#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct Shader
{
    bool pixel = false;
    uint64_t hash = 0;
    std::vector<uint8_t> code;      // big-endian, as stored
    std::vector<uint32_t> words;    // host order
    // best copy found
    int source = -1;
    uint64_t offset = 0;
    uint32_t differing = UINT32_MAX;
    std::vector<uint32_t> patches;  // word indices that differ in the stored copy
};

static uint32_t Be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int main(int argc, char** argv)
{
    if (argc < 7) { fprintf(stderr, "usage: pipeline_list <seen.txt> <shader dir> <output> <game dir> <image.bin or -> <archive>...\n"); return 1; }
    const std::string seenPath = argv[1], shaderDir = argv[2], outputPath = argv[3], gameDir = argv[4], imagePath = argv[5];

    // The pipelines, and through them the shaders that matter.
    std::vector<std::string> pipelineLines;
    std::set<std::pair<bool, uint64_t>> wanted;
    {
        FILE* seen = fopen(seenPath.c_str(), "r");
        if (!seen) { fprintf(stderr, "cannot read %s\n", seenPath.c_str()); return 1; }
        char line[256];
        std::set<std::string> unique;
        while (fgets(line, sizeof(line), seen))
        {
            unsigned long long vs, ps;
            unsigned blend, mask, depth, stencil, flags;
            if (sscanf(line, "P %llx %llx %x %x %x %x %x", &vs, &ps, &blend, &mask, &depth, &stencil, &flags) != 7) continue;
            line[strcspn(line, "\r\n")] = 0;
            if (!unique.insert(line).second) continue;
            pipelineLines.push_back(line);
            wanted.insert({ false, vs });
            wanted.insert({ true, ps });
        }
        fclose(seen);
    }

    std::vector<Shader> shaders;
    for (const auto& [pixel, hash] : wanted)
    {
        char name[512];
        snprintf(name, sizeof(name), "%s/%s_%016llX.bin", shaderDir.c_str(), pixel ? "ps" : "vs", (unsigned long long)hash);
        FILE* file = fopen(name, "rb");
        if (!file) continue;
        Shader shader;
        shader.pixel = pixel;
        shader.hash = hash;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        shader.words.resize(size_t(size) / 4);
        if (fread(shader.words.data(), 4, shader.words.size(), file) != shader.words.size()) shader.words.clear();
        fclose(file);
        if (shader.words.size() < 6) continue;
        for (uint32_t word : shader.words)
            for (int shift = 24; shift >= 0; shift -= 8) shader.code.push_back(uint8_t(word >> shift));
        shaders.push_back(std::move(shader));
    }
    printf("%zu pipelines use %zu shaders; %zu of those have their microcode in %s\n", pipelineLines.size(), wanted.size(), shaders.size(), shaderDir.c_str());

    // Anchors: 8 bytes from the end of each shader (the start can hold patched fetches), and 8 from
    // its middle in case the end is common to too many. A stored copy must contain one of them.
    struct Anchor { uint32_t shader, offset; };
    std::unordered_map<uint64_t, std::vector<Anchor>> anchors;
    std::vector<uint8_t> prefilter(1 << 24, 0);
    auto addAnchor = [&](uint32_t index, size_t offset) {
        const uint8_t* p = shaders[index].code.data() + offset;
        uint64_t key;
        memcpy(&key, p, 8);
        if (key == 0) return;
        anchors[key].push_back({ index, uint32_t(offset) });
        prefilter[uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]] = 1;
    };
    for (uint32_t i = 0; i < shaders.size(); i++)
    {
        const size_t size = shaders[i].code.size();
        addAnchor(i, size - 12);
        addAnchor(i, (size / 24) * 12);
        addAnchor(i, (size / 16) * 12);
    }

    std::vector<std::string> sources;
    auto scan = [&](const std::string& path, const std::string& name) {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", path.c_str()); return; }
        const int source = int(sources.size());
        sources.push_back(name);
        const size_t chunkSize = 256u << 20, overlap = 1u << 17;
        std::vector<uint8_t> data(chunkSize + 2 * overlap);
        uint64_t base = 0;
        size_t kept = 0, scanFrom = 0;
        while (true)
        {
            const size_t got = fread(data.data() + kept, 1, chunkSize, file);
            const size_t have = kept + got;
            if (have < 16) break;
            // Positions whose anchor and whole shader lie inside what is loaded; the rest waits for
            // the next chunk (the overlap is larger than any shader).
            const size_t limit = got == 0 ? have - 8 : have - std::min(have, overlap);
            for (size_t p = scanFrom; p < limit; p++)
            {
                if (!prefilter[uint32_t(data[p]) << 16 | uint32_t(data[p + 1]) << 8 | data[p + 2]]) continue;
                uint64_t key;
                memcpy(&key, data.data() + p, 8);
                auto found = anchors.find(key);
                if (found == anchors.end()) continue;
                for (const Anchor& anchor : found->second)
                {
                    Shader& shader = shaders[anchor.shader];
                    if (shader.differing == 0 || p < anchor.offset || p - anchor.offset + shader.code.size() > have) continue;
                    const uint8_t* stored = data.data() + p - anchor.offset;
                    uint32_t differing = 0;
                    const uint32_t count = uint32_t(shader.words.size()), allowed = std::min(shader.differing, count / 3);
                    for (uint32_t w = 0; w < count && differing <= allowed; w++) differing += Be(stored + w * 4) != shader.words[w];
                    if (differing >= shader.differing || differing > count / 3) continue;
                    shader.differing = differing;
                    shader.source = source;
                    shader.offset = base + p - anchor.offset;
                    shader.patches.clear();
                    for (uint32_t w = 0; w < count; w++) if (Be(stored + w * 4) != shader.words[w]) shader.patches.push_back(w);
                }
            }
            if (got == 0) break;
            // Keep the unsearched tail and as much again before it, for a shader that starts earlier.
            const size_t keep = std::min(have, 2 * overlap);
            memmove(data.data(), data.data() + have - keep, keep);
            base += have - keep;
            kept = keep;
            scanFrom = keep - std::min(keep, overlap);
        }
        fclose(file);
        printf("searched %s\n", name.c_str());
    };
    if (imagePath != "-") scan(imagePath, "image");
    for (int i = 6; i < argc; i++) scan(gameDir + "/" + argv[i], argv[i]);

    uint32_t exact[2] = {}, patched[2] = {}, missing[2] = {}, patchWords = 0;
    std::vector<const Shader*> found;
    for (const Shader& shader : shaders)
    {
        if (shader.source < 0) { missing[shader.pixel]++; continue; }
        (shader.differing ? patched : exact)[shader.pixel]++;
        patchWords += shader.differing;
        found.push_back(&shader);
    }
    std::sort(found.begin(), found.end(), [](const Shader* a, const Shader* b) { return a->source != b->source ? a->source < b->source : a->offset < b->offset; });
    std::set<std::pair<bool, uint64_t>> available;
    for (const Shader* shader : found) available.insert({ shader->pixel, shader->hash });

    FILE* out = fopen(outputPath.c_str(), "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", outputPath.c_str()); return 1; }
    fprintf(out, "# Pipeline list: generated by tools/pipeline_list, read by recomp/runtime/prewarm.cpp. It names where\n"
                 "# each shader is in the player's own copy of the game and which render states it is used with.\n");
    for (const Shader* shader : found)
    {
        // In the image the offset becomes the guest address the image is loaded at.
        const bool image = sources[shader->source] == "image";
        fprintf(out, "S %s %016llX %s %llX %zu", shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, sources[shader->source].c_str(),
                (unsigned long long)(shader->offset + (image ? 0x82000000ull : 0)), shader->words.size());
        for (size_t i = 0; i < shader->patches.size(); i++) fprintf(out, "%c%u:%X", i ? ',' : ' ', shader->patches[i], shader->words[shader->patches[i]]);
        fprintf(out, "\n");
    }
    uint32_t covered = 0;
    for (const std::string& line : pipelineLines)
    {
        unsigned long long vs, ps;
        sscanf(line.c_str(), "P %llx %llx", &vs, &ps);
        if (!available.count({ false, vs }) || !available.count({ true, ps })) continue;
        fprintf(out, "%s\n", line.c_str());
        covered++;
    }
    fclose(out);
    printf("vertex shaders: %u stored as captured, %u stored with patched words, %u not found\n", exact[0], patched[0], missing[0]);
    printf("pixel shaders:  %u stored as captured, %u stored with patched words, %u not found\n", exact[1], patched[1], missing[1]);
    printf("%u patched words in all; %u of %zu pipelines can be made ahead of time\n", patchWords, covered, pipelineLines.size());
    return 0;
}
