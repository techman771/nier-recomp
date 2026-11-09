// Translates Xenos shader microcode to HLSL. HLSL is also the route to Vulkan later (through DXC).
#pragma once
#include <cstdint>
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
};

TranslatedShader TranslateShader(const std::vector<uint32_t>& words, bool pixel, uint64_t hash);
