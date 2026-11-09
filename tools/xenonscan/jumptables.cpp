// Jump table detector for images whose compiler output doesn't match XenonAnalyse's fixed patterns.
// Simulates the straight-line code between the bounds check and the bctr, so instruction order
// doesn't matter, and validates every label against .pdata function bounds.
// Usage: jumptables <xex> <output.toml>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <file.h>
#include <image.h>
#include <disasm.h>

static uint32_t BE32(const void* p)
{
    auto* b = (const uint8_t*)p;
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
static uint16_t BE16(const void* p)
{
    auto* b = (const uint8_t*)p;
    return uint16_t((b[0] << 8) | b[1]);
}

struct Val
{
    enum Kind { Unknown, Const, Index, Loaded, Target } kind = Unknown;
    uint32_t k = 0;      // Const value, or table address for Loaded, or base for Target
    int shift = 0;       // Index scale, or Target value shift
    int size = 0;        // Loaded element size
    uint32_t table = 0;  // Target: table address
};

struct Table
{
    uint32_t base, r, def;
    std::vector<uint32_t> labels;
    const char* kind;
    uint32_t bctr = 0;
    uint32_t tableAddr = 0;
    bool needsBounds = false;
};

static std::map<uint32_t, uint32_t> gFuncs;

static bool FuncOf(uint32_t addr, uint32_t& begin, uint32_t& size)
{
    auto it = gFuncs.upper_bound(addr);
    if (it == gFuncs.begin()) return false;
    --it;
    if (addr >= it->first + it->second) return false;
    begin = it->first;
    size = it->second;
    return true;
}

static void Dump(const Section& s, size_t from, size_t to)
{
    for (size_t o = from; o <= to; o += 4)
    {
        ppc_insn insn;
        ppc::Disassemble(s.data + o, s.base + o, insn);
        printf("    %08zX  %08X  %-8s %s\n", s.base + o, BE32(s.data + o), insn.opcode ? insn.opcode->name : "???", insn.opcode ? insn.op_str : "");
    }
}

// Returns "" on success, otherwise the reason this bctr isn't a resolvable table.
static std::string Resolve(const Image& image, const Section& s, size_t bctrOff, Table& out, size_t& windowStart)
{
    ppc_insn insn;
    size_t branchOff = 0;
    bool found = false;
    bool sawTableLoad = false;
    uint32_t cr = 0, def = 0;

    for (int i = 1; i <= 96 && bctrOff >= size_t(i) * 4; i++)
    {
        size_t o = bctrOff - i * 4;
        ppc::Disassemble(s.data + o, s.base + o, insn);
        if (!insn.opcode) return "not-a-table";
        auto id = insn.opcode->id;
        if (id == PPC_INST_LWZX || id == PPC_INST_LHZX || id == PPC_INST_LBZX) sawTableLoad = true;
        if (id == PPC_INST_B && o >= 4)
        {
            // "ble cr, +8; b default" form used when the default case is out of bc range.
            ppc_insn prev;
            ppc::Disassemble(s.data + o - 4, s.base + o - 4, prev);
            if (prev.opcode && prev.opcode->id == PPC_INST_BLE && prev.operands[1] == s.base + o + 4)
            {
                cr = prev.operands[0];
                def = insn.operands[0];
                branchOff = o;
                found = true;
                break;
            }
        }
        if (id == PPC_INST_BGT || id == PPC_INST_BGTLR || id == PPC_INST_BLE || id == PPC_INST_BLELR)
        {
            cr = insn.operands[0];
            if (insn.opcode->operands[1] != 0) def = insn.operands[1];
            branchOff = o;
            found = true;
            break;
        }
        if (id == PPC_INST_B || id == PPC_INST_BL || id == PPC_INST_BLR || id == PPC_INST_BCTR || id == PPC_INST_BCTRL)
            return sawTableLoad ? "table-load-without-bounds-check" : "not-a-table";
    }
    if (!found) return sawTableLoad ? "table-load-without-bounds-check" : "not-a-table";

    uint32_t v = 0, maxIndex = 0;
    size_t cmpOff = 0;
    found = false;
    for (int i = 1; i <= 24 && branchOff >= size_t(i) * 4; i++)
    {
        size_t o = branchOff - i * 4;
        ppc::Disassemble(s.data + o, s.base + o, insn);
        if (insn.opcode && insn.opcode->id == PPC_INST_CMPLWI && insn.operands[0] == cr)
        {
            v = insn.operands[1];
            maxIndex = insn.operands[2];
            cmpOff = o;
            found = true;
            break;
        }
    }
    if (!found) return sawTableLoad ? "no-cmplwi" : "not-a-table";

    Val regs[32];
    regs[v].kind = Val::Index;
    windowStart = branchOff + 4;

    // The compiler sometimes indexes through a copy of the compared register, or recomputes
    // the same value after the check. Learn both from the code just before the compare.
    uint32_t defWord = 0;
    bool aliasSeen = false;
    for (int i = 1; i <= 12 && cmpOff >= size_t(i) * 4; i++)
    {
        size_t o = cmpOff - i * 4;
        ppc::Disassemble(s.data + o, s.base + o, insn);
        if (!insn.opcode) break;
        auto id = insn.opcode->id;
        if (id == PPC_INST_B || id == PPC_INST_BLR || id == PPC_INST_BCTR) break;
        if (id == PPC_INST_MR && !aliasSeen && (insn.operands[0] == v || insn.operands[1] == v))
        {
            uint32_t other = insn.operands[0] == v ? insn.operands[1] : insn.operands[0];
            regs[other].kind = Val::Index;
            aliasSeen = true;
        }
        if (!defWord && (id == PPC_INST_ADDI || id == PPC_INST_CLRLWI) && insn.operands[0] == v)
            defWord = BE32(s.data + o);
    }

    Val final;
    bool sawMtctr = false;
    for (size_t o = windowStart; o < bctrOff; o += 4)
    {
        ppc::Disassemble(s.data + o, s.base + o, insn);
        if (!insn.opcode) return "invalid-in-window";
        auto id = insn.opcode->id;
        auto* a = insn.operands;
        if (defWord && BE32(s.data + o) == defWord)
        {
            regs[a[0]] = {};
            regs[a[0]].kind = Val::Index;
            continue;
        }
        switch (id)
        {
        case PPC_INST_NOP:
        case PPC_INST_LFS:
        case PPC_INST_LFD:
        case PPC_INST_FMR:
        case PPC_INST_STW:
        case PPC_INST_STB:
        case PPC_INST_STH:
        case PPC_INST_STD:
        case PPC_INST_STFS:
        case PPC_INST_STFD:
            break;
        case PPC_INST_LI:
        case PPC_INST_LWZ:
        case PPC_INST_LBZ:
        case PPC_INST_LHZ:
        case PPC_INST_LHA:
        case PPC_INST_LD:
        case PPC_INST_CLRLWI:
        case PPC_INST_ORI:
            regs[a[0]] = {};
            break;
        case PPC_INST_LIS:
            regs[a[0]] = {};
            regs[a[0]].kind = Val::Const;
            regs[a[0]].k = a[1] << 16;
            break;
        case PPC_INST_ADDI:
            if (regs[a[1]].kind != Val::Const) { regs[a[0]] = {}; break; }
            { uint32_t k = regs[a[1]].k + a[2]; regs[a[0]] = {}; regs[a[0]].kind = Val::Const; regs[a[0]].k = k; }
            break;
        case PPC_INST_MR:
            regs[a[0]] = regs[a[1]];
            break;
        case PPC_INST_RLWINM:
        {
            Val src = regs[a[1]];
            Val res;
            if (a[3] == 0 && a[4] == 31 - a[2] && src.kind == Val::Index && src.shift == 0)
            {
                res.kind = Val::Index;
                res.shift = a[2];
            }
            else if (a[3] == 0 && a[4] == 31 - a[2] && src.kind == Val::Loaded)
            {
                res = src;
                res.shift = a[2];
            }
            regs[a[0]] = res;
            break;
        }
        case PPC_INST_LWZX:
        case PPC_INST_LHZX:
        case PPC_INST_LBZX:
        {
            int size = id == PPC_INST_LWZX ? 4 : id == PPC_INST_LHZX ? 2 : 1;
            int want = size == 4 ? 2 : size == 2 ? 1 : 0;
            Val x = regs[a[1]], y = regs[a[2]];
            if (x.kind == Val::Index) std::swap(x, y);
            Val res;
            if (x.kind == Val::Const && y.kind == Val::Index && y.shift == want)
            {
                res.kind = Val::Loaded;
                res.k = x.k;
                res.size = size;
            }
            regs[a[0]] = res;
            break;
        }
        case PPC_INST_ADD:
        {
            Val x = regs[a[1]], y = regs[a[2]];
            if (x.kind == Val::Loaded) std::swap(x, y);
            Val res;
            if (x.kind == Val::Const && y.kind == Val::Loaded)
            {
                res.kind = Val::Target;
                res.k = x.k;
                res.table = y.k;
                res.size = y.size;
                res.shift = y.shift;
            }
            regs[a[0]] = res;
            break;
        }
        case PPC_INST_MTCTR:
            final = regs[a[0]];
            sawMtctr = true;
            break;
        default:
            return std::string("unknown-insn-in-window:") + insn.opcode->name;
        }
    }
    if (!sawMtctr) return "no-mtctr";

    int indexReg = -1;
    if (regs[v].kind == Val::Index && regs[v].shift == 0) indexReg = v;
    for (int i = 0; i < 32 && indexReg < 0; i++)
        if (regs[i].kind == Val::Index && regs[i].shift == 0) indexReg = i;
    if (final.kind != Val::Loaded && final.kind != Val::Target) return "unresolved-target";
    if (final.kind == Val::Loaded && final.size != 4) return "unresolved-target";
    if (indexReg < 0) return "index-clobbered";

    uint32_t tableAddr = final.kind == Val::Loaded ? final.k : final.table;
    const uint8_t* table = (const uint8_t*)image.Find(tableAddr);
    if (!table) return "table-outside-image";

    uint32_t begin = 0, size = 0;
    bool hasPdata = FuncOf(uint32_t(s.base + bctrOff), begin, size);

    out.base = uint32_t(s.base + windowStart);
    out.r = indexReg;
    out.def = def;
    out.bctr = uint32_t(s.base + bctrOff);
    out.tableAddr = tableAddr;
    out.needsBounds = !hasPdata;
    out.kind = final.kind == Val::Loaded ? "absolute" : final.size == 2 ? "short-offset" : final.shift ? "computed" : "byte-offset";
    out.labels.clear();
    for (uint32_t i = 0; i <= maxIndex; i++)
    {
        uint32_t label;
        if (final.kind == Val::Loaded) label = BE32(table + i * 4);
        else if (final.size == 2) label = final.k + (uint32_t(BE16(table + i * 2)) << final.shift);
        else if (final.size == 1) label = final.k + (uint32_t(table[i]) << final.shift);
        else return "unresolved-target";
        if (label & 3) return "label-misaligned";
        if (hasPdata && (label < begin || label >= begin + size)) return "label-outside-function";
        if (!hasPdata && (label < s.base || label >= s.base + s.size)) return "label-outside-section";
        out.labels.push_back(label);
    }
    return "";
}

// Leaf functions have no .pdata entry, so recover their bounds with a linear sweep of the gap
// between neighbouring .pdata functions. Jump table labels extend reach and inline table data is skipped.
static bool SweepBounds(const Section& s, uint32_t target, const std::map<uint32_t, Table>& byBctr, uint32_t& outBegin, uint32_t& outSize)
{
    auto next = gFuncs.upper_bound(target);
    uint32_t gapEnd = next == gFuncs.end() ? uint32_t(s.base + s.size) : next->first;
    uint32_t gapBegin = uint32_t(s.base);
    if (next != gFuncs.begin())
    {
        auto prev = std::prev(next);
        gapBegin = prev->first + prev->second;
    }

    uint32_t pos = gapBegin;
    while (pos < gapEnd)
    {
        while (pos < gapEnd && BE32(s.data + (pos - s.base)) == 0) pos += 4;
        uint32_t fnBegin = pos, reach = pos;
        bool ended = false;
        while (pos < gapEnd && !ended)
        {
            uint32_t w = BE32(s.data + (pos - s.base));
            uint32_t op = w >> 26;
            bool terminator = w == 0x4E800020 || (op == 18 && (w & 1) == 0);
            auto table = byBctr.find(pos);
            if (w == 0x4E800420)
            {
                terminator = true;
                if (table != byBctr.end())
                {
                    for (auto l : table->second.labels) reach = std::max(reach, l);
                    reach = std::max(reach, table->second.def);
                    if (table->second.tableAddr == pos + 4 && !strcmp(table->second.kind, "absolute"))
                    {
                        pos += 4 + uint32_t(table->second.labels.size()) * 4;
                        continue;
                    }
                }
            }
            if (op == 16 && (w & 1) == 0)
            {
                int32_t d = int32_t(w & 0xFFFC);
                if (d & 0x8000) d -= 0x10000;
                uint32_t t = pos + d;
                if (t > pos && t < gapEnd) reach = std::max(reach, t);
            }
            if (op == 18 && (w & 3) == 0 && pos < reach)
            {
                int32_t d = int32_t(w & 0x03FFFFFC);
                if (d & 0x02000000) d -= 0x04000000;
                uint32_t t = pos + d;
                if (t > pos && t < gapEnd) reach = std::max(reach, t);
            }
            pos += 4;
            if (terminator && pos > reach) ended = true;
        }
        if (target >= fnBegin && target < pos)
        {
            outBegin = fnBegin;
            outSize = pos - fnBegin;
            return true;
        }
    }
    return false;
}

static uint32_t CountCallers(const Image& image, uint32_t target)
{
    uint32_t n = 0;
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (size_t o = 0; o + 4 <= s.size; o += 4)
        {
            uint32_t w = BE32(s.data + o);
            if ((w >> 26) != 18 || (w & 3) != 1) continue;
            int32_t d = int32_t(w & 0x03FFFFFC);
            if (d & 0x02000000) d -= 0x04000000;
            if (uint32_t(s.base + o + d) == target) n++;
        }
    }
    return n;
}

static void FindRegisterBlocks(const Image& image, int primary, const char* label)
{
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (size_t o = 0; o + 4 <= s.size; o += 4)
        {
            int run = 0;
            int next = 14;
            for (size_t c = o; c + 4 <= s.size && c < o + 32 * 4 && next <= 31; c += 4)
            {
                uint32_t w = BE32(s.data + c);
                if ((w >> 26) == uint32_t(primary) && (w & 3) == 0 && ((w >> 16) & 31) == 3 && int((w >> 21) & 31) == next)
                {
                    next++;
                    run++;
                }
            }
            uint32_t first = BE32(s.data + o);
            if (run >= 18 && ((first >> 21) & 31) == 14 && (first >> 26) == uint32_t(primary))
            {
                uint32_t begin = 0, size = 0;
                FuncOf(uint32_t(s.base + o), begin, size);
                printf("%s block at 0x%08zX in function 0x%08X size 0x%X, bl callers %u\n", label, s.base + o, begin, size, CountCallers(image, begin));
                if (begin) Dump(s, begin - s.base, std::min<size_t>(begin - s.base + size - 4, begin - s.base + 26 * 4));
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 4) { puts("usage: jumptables <xex> <switch_tables.toml> <functions_snippet.toml>"); return 1; }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());

    const Section* pdata = image.Find(".pdata");
    if (!pdata) { puts("no .pdata"); return 1; }
    for (uint32_t i = 0; i + 8 <= pdata->size; i += 8)
    {
        uint32_t begin = BE32(pdata->data + i), data = BE32(pdata->data + i + 4);
        if (begin) gFuncs[begin] = ((data >> 8) & 0x3FFFFF) * 4;
    }
    printf("pdata functions: %zu\n", gFuncs.size());

    std::vector<Table> tables;
    std::map<std::string, std::vector<std::pair<const Section*, size_t>>> failures;
    std::map<std::string, int> kinds;
    size_t bctrs = 0, labelTotal = 0;

    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code)) continue;
        for (size_t o = 16; o + 4 <= s.size; o += 4)
        {
            if (BE32(s.data + o) != 0x4E800420) continue;
            bctrs++;
            Table t;
            size_t start = 0;
            std::string why = Resolve(image, s, o, t, start);
            if (why.empty())
            {
                kinds[t.kind]++;
                labelTotal += t.labels.size();
                tables.push_back(std::move(t));
            }
            else failures[why].push_back({ &s, o });
        }
    }

    printf("bctr %zu, tables %zu, labels %zu\n", bctrs, tables.size(), labelTotal);
    for (const auto& [k, n] : kinds) printf("  %-14s %d\n", k.c_str(), n);

    // Completeness check: the compiler's table dispatch always loads through r12/r0.
    size_t missed = 0;
    for (const auto& [why, list] : failures)
    {
        size_t candidates = 0;
        for (const auto& [sec, off] : list)
        {
            bool isCandidate = false;
            for (int i = 1; i <= 6 && off >= size_t(i) * 4; i++)
            {
                uint32_t w = BE32(sec->data + off - i * 4);
                if (w == 0x7C0C002E || w == 0x7C0C022E || w == 0x7C0C00AE) isCandidate = true;
            }
            if (!isCandidate) continue;
            candidates++;
            missed++;
            printf("  UNRESOLVED %s at 0x%08zX\n", why.c_str(), sec->base + off);
            Dump(*sec, off >= 56 ? off - 56 : 0, off);
        }
        printf("skipped %-32s %zu (table-shaped: %zu)\n", why.c_str(), list.size(), candidates);
    }
    printf("table-shaped dispatches left unresolved: %zu\n", missed);

    // Bounds for tables in leaf functions, which have no .pdata entry.
    std::map<uint32_t, Table> byBctr;
    for (const auto& t : tables) byBctr[t.bctr] = t;
    const Section* text = image.Find(".text");
    std::map<uint32_t, uint32_t> extraFuncs;
    std::vector<Table> kept;
    size_t dropped = 0;
    for (auto& t : tables)
    {
        if (t.needsBounds)
        {
            uint32_t b = 0, sz = 0;
            bool ok = SweepBounds(*text, t.bctr, byBctr, b, sz);
            for (auto l : t.labels) ok = ok && l >= b && l < b + sz;
            if (!ok)
            {
                printf("  DROPPED leaf table at bctr 0x%08X (sweep bounds 0x%08X size 0x%X)\n", t.bctr, b, sz);
                dropped++;
                continue;
            }
            extraFuncs[b] = sz;
        }
        kept.push_back(std::move(t));
    }
    tables = std::move(kept);
    printf("leaf functions with explicit bounds: %zu, leaf tables dropped: %zu, final tables: %zu\n", extraFuncs.size(), dropped, tables.size());

    FILE* fn = fopen(argv[3], "w");
    if (!fn) { puts("cannot write functions snippet"); return 1; }
    fputs("functions = [\n", fn);
    for (const auto& [b, sz] : extraFuncs) fprintf(fn, "    { address = 0x%X, size = 0x%X },\n", b, sz);
    fputs("]\n\n", fn);

    // Functions with an exception handler are preceded by 8 bytes of data (handler address, then
    // handler data). Decoded as code these create ghost functions that overlap the real one, and
    // the scan then resumes mid-function and creates more, so they must be skipped.
    std::map<uint32_t, size_t> handlers;
    size_t flagged = 0, badHandlers = 0;
    for (uint32_t i = 0; i + 8 <= pdata->size; i += 8)
    {
        uint32_t begin = BE32(pdata->data + i), data = BE32(pdata->data + i + 4);
        if (!begin || !(data >> 31)) continue;
        flagged++;
        uint32_t handler = BE32(text->data + (begin - 8 - text->base));
        // A real handler is a code address. Anything else means this flag doesn't describe
        // preceding data here, and treating it as data could skip real instructions.
        if (handler < text->base || handler >= text->base + text->size)
        {
            badHandlers++;
            printf("  flagged function 0x%08X has non-address word 0x%08X before it\n", begin, handler);
            continue;
        }
        handlers[handler]++;
    }
    printf("functions with exception data: %zu, distinct handlers: %zu, implausible: %zu\n", flagged, handlers.size(), badHandlers);
    for (const auto& [h, n] : handlers)
    {
        auto sym = image.symbols.find(h);
        printf("  handler 0x%08X used %zu times %s\n", h, n, sym != image.symbols.end() ? sym->name.c_str() : "");
    }
    fputs("invalid_instructions = [\n    { data = 0x00000000, size = 4 }, # Padding\n", fn);
    for (const auto& [h, n] : handlers) fprintf(fn, "    { data = 0x%X, size = 8 }, # Exception handler data, %zu functions\n", h, n);
    fputs("]\n", fn);
    fclose(fn);

    FILE* f = fopen(argv[2], "w");
    if (!f) { puts("cannot write toml"); return 1; }
    fputs("# Generated by tools/xenonscan/jumptables.cpp; every label is validated against .pdata bounds\n", f);
    for (const auto& t : tables)
    {
        fprintf(f, "[[switch]] # %s\nbase = 0x%X\nr = %u\ndefault = 0x%X\nlabels = [\n", t.kind, t.base, t.r, t.def);
        for (auto l : t.labels) fprintf(f, "    0x%X,\n", l);
        fputs("]\n\n", f);
    }
    fclose(f);

    for (const auto& sym : image.symbols)
        if (sym.name == "__imp__RtlUnwind")
            printf("RtlUnwind thunk 0x%zX, bl callers %u\n", sym.address, CountCallers(image, uint32_t(sym.address)));
    for (const auto& sym : image.symbols)
    {
        if (sym.name != "__imp__RtlUnwind") continue;
        for (size_t o = 0; o + 4 <= text->size; o += 4)
        {
            uint32_t w = BE32(text->data + o);
            if ((w >> 26) != 18 || (w & 3) != 1) continue;
            int32_t d = int32_t(w & 0x03FFFFFC);
            if (d & 0x02000000) d -= 0x04000000;
            if (uint32_t(text->base + o + d) != uint32_t(sym.address)) continue;
            uint32_t begin = 0, size = 0;
            FuncOf(uint32_t(text->base + o), begin, size);
            printf("RtlUnwind caller at 0x%08zX in function 0x%08X size 0x%X, callers %u\n", text->base + o, begin, size, begin ? CountCallers(image, begin) : 0);
        }
    }
    puts("longjmp candidate start:");
    Dump(*text, 0x825D5AC0 - text->base, 0x825D5B10 - text->base);
    puts("boundary between candidates:");
    Dump(*text, 0x825D5DA0 - text->base, 0x825D5E20 - text->base);
    printf("bl callers: 0x825D5AC0=%u 0x825D5DE0=%u 0x825D5DD8=%u 0x825D5DD4=%u\n", CountCallers(image, 0x825D5AC0), CountCallers(image, 0x825D5DE0), CountCallers(image, 0x825D5DD8), CountCallers(image, 0x825D5DD4));
    return 0;
}
