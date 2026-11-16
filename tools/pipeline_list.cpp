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
// Most model files in the archives are "lzo" containers (recomp/runtime/lzo1x.h): each one found in an
// archive is unpacked and searched too, and a shader inside one gets a Z line (the container's place
// in the archive and the shader's place in the unpacked data).
//
// Shaders nobody has drawn yet: every pixel shader stored in the archives' model, effect and 2D
// files is listed as well (EnumerateStored), so the game can compile them all once, ahead of play.
// Such a file comes as a pair: X.??V holds the shader headers (magic 102A1100 / 102A1101), X.??P the
// code. The code parts of a group of headers follow one another in header order, each rounded up to
// 32 bytes. In a model file (*.MDP) there is one group, at the end of the (unpacked) data, starting
// on a 4 KB boundary; an effect file (*.EFP) holds several groups in different places. So each
// header's code is first looked for right after the one before it, and when what is there does not
// read as microcode (PlausibleMicrocode), the file is searched for a place where it, and the next
// header's after it, do. Vertex shaders are not listed: what the game draws with is a patched copy
// (see above), known only once it has been drawn.
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
#include "../recomp/runtime/lzo1x.h"

struct Shader
{
    bool pixel = false;
    uint64_t hash = 0;
    std::vector<uint8_t> code;      // big-endian, as stored
    std::vector<uint32_t> words;    // host order
    // best copy found
    int source = -1;
    uint64_t offset = 0;
    int64_t container = -1;         // inside an lzo container at this offset of the source (offset: in its unpacked data)
    uint32_t differing = UINT32_MAX;
    std::vector<uint32_t> patches;  // word indices that differ in the stored copy
};

static uint32_t Be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static uint64_t Be64(const uint8_t* p) { return uint64_t(Be(p)) << 32 | Be(p + 4); }

// A table of the archive format ("@UTF"): the columns asked for, by name, for every row.
struct UtfTable
{
    std::vector<std::map<std::string, uint64_t>> numbers;
    std::vector<std::map<std::string, std::string>> strings;
};

static bool ReadUtf(const std::vector<uint8_t>& data, size_t at, UtfTable& table)
{
    if (at + 0x20 > data.size() || memcmp(&data[at], "@UTF", 4)) return false;
    const size_t size = Be(&data[at + 4]);
    if (at + 8 + size > data.size()) return false;
    const uint8_t* t = &data[at + 8];
    const uint32_t rowsAt = Be(t), stringsAt = Be(t + 4);
    const uint32_t columns = uint32_t(t[16]) << 8 | t[17], rowLength = uint32_t(t[18]) << 8 | t[19], rows = Be(t + 20);
    auto text = [&](uint32_t offset) { return stringsAt + offset < size ? std::string((const char*)t + stringsAt + offset) : std::string(); };
    static const int kSizes[16] = { 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8, 0, 0, 0, 0 };
    struct Column { std::string name; int storage, kind; size_t constantAt; };
    std::vector<Column> list;
    size_t p = 0x18;
    for (uint32_t i = 0; i < columns; i++)
    {
        if (p + 5 > size) return false;
        Column column;
        column.storage = t[p] & 0xF0;
        column.kind = t[p] & 0x0F;
        column.name = text(Be(t + p + 1));
        p += 5;
        column.constantAt = p;
        if (column.storage == 0x30) p += kSizes[column.kind];
        list.push_back(column);
    }
    auto read = [&](const Column& column, size_t where, std::map<std::string, uint64_t>& numbers, std::map<std::string, std::string>& strings) {
        if (where + kSizes[column.kind] > size) return;
        if (column.kind == 0xA) strings[column.name] = text(Be(t + where));
        else if (column.kind < 8)
        {
            uint64_t value = 0;
            for (int i = 0; i < kSizes[column.kind]; i++) value = value << 8 | t[where + i];
            numbers[column.name] = value;
        }
    };
    for (uint32_t r = 0; r < rows; r++)
    {
        size_t where = rowsAt + size_t(r) * rowLength;
        table.numbers.emplace_back();
        table.strings.emplace_back();
        for (const Column& column : list)
        {
            if (column.storage == 0x50) { read(column, where, table.numbers.back(), table.strings.back()); where += kSizes[column.kind]; }
            else if (column.storage == 0x30) read(column, column.constantAt, table.numbers.back(), table.strings.back());
        }
    }
    return true;
}

// The archive's own compression of a file ("CRILAYLA"): the first 0x100 bytes are stored after the
// packed data as they are, the rest is a bit stream read from its end backwards.
static bool Crilayla(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
{
    if (in.size() < 0x110 || memcmp(in.data(), "CRILAYLA", 8)) return false;
    uint32_t size, packed;
    memcpy(&size, &in[8], 4);
    memcpy(&packed, &in[12], 4);
    if (size > (256u << 20) || 16ull + packed + 0x100 > in.size()) return false;
    out.assign(size_t(size) + 0x100, 0);
    memcpy(out.data(), &in[16 + packed], 0x100);
    int64_t inPos = 16 + int64_t(packed) - 1;
    uint32_t pool = 0;
    int left = 0;
    bool bad = false;
    auto bits = [&](int count) -> uint32_t {
        while (left < count)
        {
            if (inPos < 16) { bad = true; return 0; }
            pool = pool << 8 | in[size_t(inPos--)];
            left += 8;
        }
        left -= count;
        return (pool >> left) & ((1u << count) - 1);
    };
    const int64_t end = 0x100 + int64_t(size) - 1;
    int64_t done = 0;
    while (done < size && !bad)
    {
        if (!bits(1)) { out[size_t(end - done++)] = uint8_t(bits(8)); continue; }
        int64_t from = end - done + bits(13) + 3;
        uint32_t length = 3;
        static const int kLevels[4] = { 2, 3, 5, 8 };
        int level = 0;
        for (; level < 4; level++)
        {
            const uint32_t value = bits(kLevels[level]);
            length += value;
            if (value != (1u << kLevels[level]) - 1) break;
        }
        if (level == 4) for (uint32_t value = 255; value == 255;) { value = bits(8); length += value; }
        for (uint32_t i = 0; i < length && done < size; i++, done++, from--)
        {
            if (from > end) { bad = true; break; }
            out[size_t(end - done)] = out[size_t(from)];
        }
    }
    return !bad;
}

// Unpacks the lzo container at `container` of `file` whole. False when it is not one or is damaged.
static bool UnpackContainer(FILE* file, uint64_t container, std::vector<uint8_t>& plain)
{
    uint8_t header[0x20];
    if (_fseeki64(file, int64_t(container), SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "lzo", 4)) return false;
    const uint32_t slot = Be(header + 8), count = Be(header + 12);
    if (slot == 0 || count == 0 || count > 0x4000) return false;
    plain.clear();
    std::vector<uint8_t> packed;
    for (uint32_t b = 0; b < count; b++)
    {
        uint8_t blockHeader[12];
        if (_fseeki64(file, int64_t(LzoBlockHeaderAt(container, slot, b)), SEEK_SET) != 0 || fread(blockHeader, 1, 12, file) != 12) return false;
        LzoBlock block;
        block.offset = Be(blockHeader); block.unpacked = Be(blockHeader + 4); block.packed = Be(blockHeader + 8);
        if (block.offset != plain.size() || block.unpacked > (64u << 20) || block.packed > slot) return false;
        packed.resize(block.packed);
        plain.resize(size_t(block.offset) + block.unpacked);
        if (fread(packed.data(), 1, block.packed, file) != block.packed || !LzoUnpackBlock(block, packed.data(), plain.data() + block.offset)) return false;
    }
    return true;
}

// Whether `words` dwords at `code` read as shader microcode: it opens with control flow instructions
// (two to every three dwords) that run blocks of the instructions after them, and one of them ends
// the program. Bytes that are something else (a wrong guess at where the code is) fail this.
static bool PlausibleMicrocode(const uint8_t* code, uint32_t words)
{
    const uint32_t instructions = words / 3;
    if (instructions < 2) return false;
    uint32_t firstBlock = UINT32_MAX, covered = 0;
    for (uint32_t t = 0; t < instructions && t < firstBlock; t++)
    {
        const uint32_t d0 = Be(code + t * 12), d1 = Be(code + t * 12 + 4), d2 = Be(code + t * 12 + 8);
        const uint64_t pair[2] = { d0 | uint64_t(d1 & 0xFFFF) << 32, (d1 >> 16) | uint64_t(d2) << 16 };
        for (const uint64_t flow : pair)
        {
            const uint32_t opcode = uint32_t(flow >> 44) & 0xF;
            if (opcode == 0 || opcode == 12) continue;   // nothing, or an output allocation
            if (opcode > 6) return false;                // loops, calls and jumps: this game's shaders have none
            const uint32_t address = uint32_t(flow) & 0xFFF, count = uint32_t(flow >> 12) & 7;
            if (address == 0 || address + count > instructions) return false;
            firstBlock = std::min(firstBlock, address);
            covered = std::max(covered, address + count);
            // The ending kind: the blocks start right after the control flow and reach the shader's end.
            if (opcode % 2 == 0) return firstBlock == t + 1 && covered + 2 >= instructions;
        }
    }
    return false;
}

// A pixel shader found by its header, drawn or not.
struct Stored
{
    uint64_t hash = 0;
    int64_t container = -1;
    uint64_t offset = 0;
    uint32_t words = 0;
};

// Every pixel shader of the V/P file pairs in the archive at `path`.
static void EnumerateStored(const std::string& path, std::map<uint64_t, Stored>& stored, uint32_t& pairs, uint32_t& headersSeen, std::map<std::string, std::pair<uint32_t, uint32_t>>& byKind)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return;
    auto readAt = [&](uint64_t at, size_t size, std::vector<uint8_t>& out) {
        out.resize(size);
        return _fseeki64(file, int64_t(at), SEEK_SET) == 0 && fread(out.data(), 1, size, file) == size;
    };
    std::vector<uint8_t> head, tocData;
    UtfTable archive, toc;
    if (!readAt(0, 0x800, head) || memcmp(head.data(), "CPK ", 4) || !ReadUtf(head, 0x10, archive) || archive.numbers.empty()) { fclose(file); return; }
    const uint64_t tocAt = archive.numbers[0]["TocOffset"], contentAt = archive.numbers[0]["ContentOffset"], tocSize = archive.numbers[0]["TocSize"];
    if (!tocAt || !tocSize || !readAt(tocAt, size_t(tocSize), tocData) || !ReadUtf(tocData, 0x10, toc)) { fclose(file); return; }
    const uint64_t base = std::min(tocAt, contentAt ? contentAt : tocAt);
    struct Entry { uint64_t at; uint32_t size; };
    std::map<std::string, Entry> entries;
    for (size_t r = 0; r < toc.numbers.size(); r++)
        entries[toc.strings[r]["DirName"] + "/" + toc.strings[r]["FileName"]] = { base + toc.numbers[r]["FileOffset"], uint32_t(toc.numbers[r]["FileSize"]) };

    std::vector<uint8_t> packed, headers, plain;
    for (const auto& [name, entry] : entries)
    {
        if (name.size() < 5 || name.back() != 'P' || name[name.size() - 4] != '.') continue;
        std::string headerName = name;
        headerName.back() = 'V';
        const auto pair = entries.find(headerName);
        if (pair == entries.end() || pair->second.size < 0x40 || pair->second.size > (64u << 20)) continue;
        if (!readAt(pair->second.at, pair->second.size, packed)) continue;
        if (!memcmp(packed.data(), "CRILAYLA", 8)) { if (!Crilayla(packed, headers)) continue; }
        else headers = packed;

        // The headers, in file order: physical size, and where the microcode is inside that part.
        struct Header { bool pixel; uint32_t physical, start, size; };
        std::vector<Header> list;
        uint64_t span = 0;
        for (size_t p = 0; p + 0x28 <= headers.size(); p++)
        {
            if (headers[p] != 0x10 || headers[p + 1] != 0x2A || headers[p + 2] != 0x11 || headers[p + 3] > 1) continue;
            const uint32_t virtualSize = Be(&headers[p + 4]), physical = Be(&headers[p + 8]), record = Be(&headers[p + 24]);
            if (virtualSize >= 0x8000 || record + 8 > virtualSize || p + record + 8 > headers.size()) continue;
            list.push_back({ headers[p + 3] == 0, physical, Be(&headers[p + record]), Be(&headers[p + record + 4]) });
            span += (uint64_t(physical) + 31) / 32 * 32;
        }
        if (list.empty()) continue;

        // The code file: an lzo container, or as it is.
        int64_t container = -1;
        uint8_t magic[4] = {};
        if (_fseeki64(file, int64_t(entry.at), SEEK_SET) != 0 || fread(magic, 1, 4, file) != 4) continue;
        if (!memcmp(magic, "lzo", 4)) { if (!UnpackContainer(file, entry.at, plain)) continue; container = int64_t(entry.at); }
        else if (!readAt(entry.at, entry.size, plain)) continue;
        if (span > plain.size()) continue;
        pairs++;
        uint64_t position = (plain.size() - span) / 0x1000 * 0x1000;   // a model file's one group
        std::pair<uint32_t, uint32_t>& kind = byKind[name.substr(name.size() - 3)];   // pixel shader headers, and those whose code was found
        const auto readsAsCode = [&](const Header& header, uint64_t partAt) {
            return header.size >= 12 && header.size % 12 == 0 && header.size <= 0x10000 && uint64_t(header.start) + header.size <= header.physical &&
                   partAt + header.start + header.size <= plain.size() && PlausibleMicrocode(&plain[size_t(partAt + header.start)], header.size / 4);
        };
        for (size_t i = 0; i < list.size(); i++)
        {
            const Header& header = list[i];
            const uint64_t step = (uint64_t(header.physical) + 31) / 32 * 32;
            headersSeen++;
            kind.first += header.pixel;
            bool here = readsAsCode(header, position);
            if (!here)
            {
                // A new group: from where the last one ended first, then from the start of the file.
                uint64_t alone = UINT64_MAX;
                const uint64_t from = (position + 31) / 32 * 32;
                for (int pass = 0; pass < 2 && !here; pass++)
                    for (uint64_t q = pass ? 0 : from; q + 32 <= (pass ? std::min<uint64_t>(from, plain.size()) : plain.size()); q += 32)
                    {
                        if (!readsAsCode(header, q)) continue;
                        if (i + 1 >= list.size() || readsAsCode(list[i + 1], q + step)) { position = q; here = true; break; }
                        if (alone == UINT64_MAX) alone = q;
                    }
                if (!here && alone != UINT64_MAX) { position = alone; here = true; }
            }
            const uint64_t at = position + header.start;
            position += step;
            if (!here || !header.pixel) continue;
            kind.second++;
            uint64_t hash = 1469598103934665603ull;   // as gpu.cpp StoreShader names shaders
            for (uint32_t w = 0; w < header.size / 4; w++) hash = (hash ^ Be(&plain[size_t(at) + w * 4])) * 1099511628211ull;
            if (stored.count(hash)) continue;
            stored[hash] = { hash, container, container >= 0 ? at : entry.at + at, header.size / 4 };
        }
    }
    fclose(file);
}

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
    // Looks for the shaders at positions [from, limit) of `data` (`have` bytes, which start `base`
    // bytes into the source or, with `container` set, into that container's unpacked data).
    auto search = [&](const uint8_t* data, size_t from, size_t limit, size_t have, int source, int64_t container, uint64_t base) {
        for (size_t p = from; p < limit; p++)
        {
            if (!prefilter[uint32_t(data[p]) << 16 | uint32_t(data[p + 1]) << 8 | data[p + 2]]) continue;
            uint64_t key;
            memcpy(&key, data + p, 8);
            auto found = anchors.find(key);
            if (found == anchors.end()) continue;
            for (const Anchor& anchor : found->second)
            {
                Shader& shader = shaders[anchor.shader];
                if (shader.differing == 0 || p < anchor.offset || p - anchor.offset + shader.code.size() > have) continue;
                const uint8_t* stored = data + p - anchor.offset;
                uint32_t differing = 0;
                const uint32_t count = uint32_t(shader.words.size()), allowed = std::min(shader.differing, count / 3);
                for (uint32_t w = 0; w < count && differing <= allowed; w++) differing += Be(stored + w * 4) != shader.words[w];
                if (differing >= shader.differing || differing > count / 3) continue;
                shader.differing = differing;
                shader.source = source;
                shader.container = container;
                shader.offset = base + p - anchor.offset;
                shader.patches.clear();
                for (uint32_t w = 0; w < count; w++) if (Be(stored + w * 4) != shader.words[w]) shader.patches.push_back(w);
            }
        }
    };
    auto scan = [&](const std::string& path, const std::string& name) {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", path.c_str()); return; }
        const int source = int(sources.size());
        sources.push_back(name);
        const size_t chunkSize = 256u << 20, overlap = 1u << 17;
        std::vector<uint8_t> data(chunkSize + 2 * overlap);
        std::vector<uint64_t> containers;   // lzo containers start on 0x800 boundaries
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
            search(data.data(), scanFrom, limit, have, source, -1, base);
            for (uint64_t at = (base + scanFrom + 0x7FF) & ~0x7FFull; at < base + limit; at += 0x800)
                if (!memcmp(data.data() + (at - base), "lzo\0", 4)) containers.push_back(at);
            if (got == 0) break;
            // Keep the unsearched tail and as much again before it, for a shader that starts earlier.
            const size_t keep = std::min(have, 2 * overlap);
            memmove(data.data(), data.data() + have - keep, keep);
            base += have - keep;
            kept = keep;
            scanFrom = keep - std::min(keep, overlap);
        }
        // The containers: each unpacked whole, then searched like the file itself.
        uint32_t unpackedContainers = 0, damaged = 0;
        std::vector<uint8_t> packed, plain;
        for (const uint64_t container : containers)
        {
            uint8_t header[0x20];
            if (_fseeki64(file, int64_t(container), SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header)) continue;
            const uint32_t slot = Be(header + 8), count = Be(header + 12);
            if (slot == 0 || count == 0 || count > 0x4000) continue;
            plain.clear();
            bool good = true;
            for (uint32_t b = 0; b < count && good; b++)
            {
                uint8_t blockHeader[12];
                good = _fseeki64(file, int64_t(LzoBlockHeaderAt(container, slot, b)), SEEK_SET) == 0 && fread(blockHeader, 1, 12, file) == 12;
                if (!good) break;
                LzoBlock block;
                block.offset = Be(blockHeader); block.unpacked = Be(blockHeader + 4); block.packed = Be(blockHeader + 8);
                good = block.offset == plain.size() && block.unpacked <= (64u << 20) && block.packed <= slot;
                if (!good) break;
                packed.resize(block.packed);
                plain.resize(size_t(block.offset) + block.unpacked);
                good = fread(packed.data(), 1, block.packed, file) == block.packed && LzoUnpackBlock(block, packed.data(), plain.data() + block.offset);
            }
            if (!good) { damaged++; continue; }
            unpackedContainers++;
            if (plain.size() > 16) search(plain.data(), 0, plain.size() - 8, plain.size(), source, int64_t(container), 0);
        }
        fclose(file);
        printf("searched %s (%u lzo containers unpacked, %u not readable)\n", name.c_str(), unpackedContainers, damaged);
    };
    if (imagePath != "-") scan(imagePath, "image");
    for (int i = 6; i < argc; i++) scan(gameDir + "/" + argv[i], argv[i]);

    // Every stored pixel shader, drawn or not (after the recorded ones in the list, without pipelines).
    std::vector<std::pair<int, Stored>> extras;
    for (int i = 6; i < argc; i++)
    {
        std::map<uint64_t, Stored> stored;
        uint32_t pairs = 0, headersSeen = 0;
        std::map<std::string, std::pair<uint32_t, uint32_t>> byKind;
        EnumerateStored(gameDir + "/" + argv[i], stored, pairs, headersSeen, byKind);
        for (const auto& [kind, counts] : byKind) printf("  %s: code found for %u of %u pixel shader headers\n", kind.c_str(), counts.second, counts.first);
        int source = -1;
        for (size_t n = 0; n < sources.size(); n++) if (sources[n] == argv[i]) source = int(n);
        size_t recorded = 0;
        for (const auto& [hash, shader] : stored)
        {
            if (wanted.count({ true, hash })) { recorded++; continue; }
            bool listed = false;
            for (const auto& [otherSource, other] : extras) listed |= other.hash == hash;
            if (!listed && source >= 0) extras.push_back({ source, shader });
        }
        printf("%s: %u file pairs with %u shader headers, %zu distinct pixel shaders (%zu of them recorded already)\n", argv[i], pairs, headersSeen, stored.size(), recorded);
    }

    uint32_t exact[2] = {}, patched[2] = {}, missing[2] = {}, patchWords = 0;
    std::vector<const Shader*> found;
    for (const Shader& shader : shaders)
    {
        if (shader.source < 0) { missing[shader.pixel]++; continue; }
        (shader.differing ? patched : exact)[shader.pixel]++;
        patchWords += shader.differing;
        found.push_back(&shader);
    }
    std::sort(found.begin(), found.end(), [](const Shader* a, const Shader* b) {
        return a->source != b->source ? a->source < b->source : a->container != b->container ? a->container < b->container : a->offset < b->offset; });
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
        if (shader->container >= 0)
            fprintf(out, "Z %s %016llX %s %llX %llX %zu", shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, sources[shader->source].c_str(),
                    (unsigned long long)shader->container, (unsigned long long)shader->offset, shader->words.size());
        else
            fprintf(out, "S %s %016llX %s %llX %zu", shader->pixel ? "ps" : "vs", (unsigned long long)shader->hash, sources[shader->source].c_str(),
                    (unsigned long long)(shader->offset + (image ? 0x82000000ull : 0)), shader->words.size());
        for (size_t i = 0; i < shader->patches.size(); i++) fprintf(out, "%c%u:%X", i ? ',' : ' ', shader->patches[i], shader->words[shader->patches[i]]);
        fprintf(out, "\n");
    }
    for (const auto& [source, shader] : extras)
    {
        if (shader.container >= 0)
            fprintf(out, "Z ps %016llX %s %llX %llX %u\n", (unsigned long long)shader.hash, sources[source].c_str(), (unsigned long long)shader.container, (unsigned long long)shader.offset, shader.words);
        else
            fprintf(out, "S ps %016llX %s %llX %u\n", (unsigned long long)shader.hash, sources[source].c_str(), (unsigned long long)shader.offset, shader.words);
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
    printf("%zu more pixel shaders listed that have not been drawn yet\n", extras.size());
    return 0;
}
