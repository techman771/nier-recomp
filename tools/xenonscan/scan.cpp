// Read-only image survey built on XenonUtils. Usage: scan <xex>
// Reports sections, decode health, recompiler helper addresses and indirect-jump idioms.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <file.h>
#include <image.h>
#include <disasm.h>

struct Pattern { const char* name; std::vector<uint8_t> bytes; };

static const char* Mnemonic(const Section& s, size_t off)
{
    ppc_insn insn;
    ppc::Disassemble(s.data + off, s.base + off, insn);
    return insn.opcode ? insn.opcode->name : "???";
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: scan <xex>"); return 1; }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    printf("base 0x%zX size 0x%X entry 0x%zX\n", image.base, image.size, image.entry_point);

    const std::vector<Pattern> patterns = {
        {"restgprlr_14", {0xE9,0xC1,0xFF,0x68}},
        {"savegprlr_14", {0xF9,0xC1,0xFF,0x68}},
        {"restfpr_14",   {0xC9,0xCC,0xFF,0x70}},
        {"savefpr_14",   {0xD9,0xCC,0xFF,0x70}},
        {"restvmx_14",   {0x39,0x60,0xFE,0xE0,0x7D,0xCB,0x60,0xCE}},
        {"savevmx_14",   {0x39,0x60,0xFE,0xE0,0x7D,0xCB,0x61,0xCE}},
        {"restvmx_64",   {0x39,0x60,0xFC,0x00,0x10,0x0B,0x60,0xCB}},
        {"savevmx_64",   {0x39,0x60,0xFC,0x00,0x10,0x0B,0x61,0xCB}},
    };

    std::map<std::string, size_t> idioms;
    std::map<std::string, size_t> idiomSample;
    size_t bctrTotal = 0;

    for (const auto& s : image.sections)
    {
        printf("section %-10s base 0x%08zX size 0x%08X flags %d\n", s.name.c_str(), s.base, s.size, (int)s.flags);
        if (!(s.flags & SectionFlags_Code)) continue;

        size_t valid = 0, words = s.size / 4;
        for (size_t off = 0; off + 4 <= s.size; off += 4)
        {
            ppc_insn insn;
            ppc::Disassemble(s.data + off, s.base + off, insn);
            if (insn.opcode) valid++;
            if (insn.opcode && !strcmp(insn.opcode->name, "bctr") && off >= 40)
            {
                bctrTotal++;
                std::string key;
                for (int back = 9; back >= 1; back--)
                {
                    key += Mnemonic(s, off - back * 4);
                    if (back > 1) key += ' ';
                }
                idiomSample.emplace(key, s.base + off);
                idioms[key]++;
            }
        }
        printf("  decoded %zu / %zu words (%.1f%%)\n", valid, words, 100.0 * valid / words);

        for (const auto& p : patterns)
        {
            const uint8_t* cur = s.data;
            const uint8_t* end = s.data + s.size;
            int hits = 0;
            while ((cur = std::search(cur, end, p.bytes.begin(), p.bytes.end())) != end)
            {
                size_t off = cur - s.data;
                if (off % 4 == 0)
                {
                    printf("  %-13s 0x%08zX\n", p.name, s.base + off);
                    hits++;
                }
                cur += 4;
                if (hits >= 6) break;
            }
            if (!hits) printf("  %-13s NOT FOUND\n", p.name);
        }
    }

    printf("bctr total %zu, distinct 9-insn prefixes %zu\n", bctrTotal, idioms.size());

    // Count by the last three instructions, which is the part that identifies the idiom.
    std::map<std::string, size_t> tails;
    for (const auto& [k, n] : idioms)
    {
        size_t pos = k.size();
        for (int i = 0; i < 3 && pos != std::string::npos && pos > 0; i++) pos = k.rfind(' ', pos - 1);
        tails[pos == std::string::npos ? k : k.substr(pos + 1)] += n;
    }
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [k, n] : tails) sorted.emplace_back(n, k);
    std::sort(sorted.rbegin(), sorted.rend());
    puts("top idioms before bctr (last 3 insns):");
    for (size_t i = 0; i < sorted.size() && i < 15; i++)
        printf("  %6zu  %s\n", sorted[i].first, sorted[i].second.c_str());

    std::vector<std::pair<size_t, std::string>> full;
    for (const auto& [k, n] : idioms) full.emplace_back(n, k);
    std::sort(full.rbegin(), full.rend());
    puts("top full prefixes (9 insns before bctr):");
    for (size_t i = 0; i < full.size() && i < 12; i++)
        printf("  %6zu  e.g. 0x%08zX  %s\n", full[i].first, idiomSample[full[i].second], full[i].second.c_str());
    return 0;
}
