// Translates Xenos shader microcode to HLSL. HLSL is also the route to Vulkan later (through DXC).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct TranslatedShader
{
    bool ok = false;
    std::string error;
    std::string hlsl;
    uint64_t hash = 0;
    bool pixel = false;
    std::set<uint32_t> vertexSlots;   // vertex fetch constant slots the shader reads
    std::set<uint32_t> textureSlots;
    std::set<uint32_t> cubeSlots;     // texture slots sampled as cube maps (a subset of textureSlots)
    // Vertex fetches all address with the unmodified vertex index (r0.x), so a draw only reads
    // vertices between its smallest and largest index. Per slot: stride and the furthest byte a
    // vertex's fetches read (both in bytes).
    bool directVertexIndex = false;
    struct VertexUse { uint32_t stride = 0, end = 0; };
    std::map<uint32_t, VertexUse> vertexUse;
};

TranslatedShader TranslateShader(const std::vector<uint32_t>& words, bool pixel, uint64_t hash);
