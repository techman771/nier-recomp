// Read-only disassembly and function-table lookup. Usage: dis <xex> <address-list-file>
// Each line of the list is "<hex address> [word count]"; addresses may be prefixed with 0x.
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <file.h>
#include <image.h>
#include <disasm.h>

static uint32_t BE32(const void* p)
{
    auto* b = (const uint8_t*)p;
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}

int main(int argc, char** argv)
{
    if (argc < 3) { puts("usage: dis <xex> <address-list-file>"); return 1; }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    std::map<uint32_t, uint32_t> funcs;
    const Section* pdata = image.Find(".pdata");
    for (uint32_t i = 0; i + 8 <= pdata->size; i += 8)
    {
        uint32_t begin = BE32(pdata->data + i), data = BE32(pdata->data + i + 4);
        if (begin) funcs[begin] = ((data >> 8) & 0x3FFFFF) * 4;
    }
    const Section* text = image.Find(".text");

    std::ifstream list(argv[2]);
    std::string line;
    while (std::getline(list, line))
    {
        std::istringstream in(line);
        std::string addrText;
        int count = 8;
        if (!(in >> addrText)) continue;
        in >> count;
        uint32_t addr = uint32_t(std::stoul(addrText, nullptr, 16));

        auto it = funcs.upper_bound(addr);
        if (it != funcs.begin())
        {
            auto f = std::prev(it);
            bool inside = addr < f->first + f->second;
            printf("== %08X: %s pdata 0x%08X size 0x%X (offset 0x%X); next pdata 0x%08X\n", addr, inside ? "INSIDE" : "after", f->first, f->second, addr - f->first, it == funcs.end() ? 0 : it->first);
        }
        for (int i = 0; i < count; i++)
        {
            uint32_t a = addr + i * 4;
            if (a < text->base || a + 4 > text->base + text->size) break;
            ppc_insn insn;
            ppc::Disassemble(text->data + (a - text->base), a, insn);
            printf("  %08X  %08X  %-8s %s\n", a, BE32(text->data + (a - text->base)), insn.opcode ? insn.opcode->name : "???", insn.opcode ? insn.op_str : "");
        }
    }
    return 0;
}
