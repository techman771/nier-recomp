// Downloadable content: Xbox 360 STFS packages (LIVE / CON / PIRS) the user supplies in
// dlc\<title id>\00000002\ beside the game folder (NIER_DLC_DIR elsewhere), as a console stores them.
// The game lists marketplace content (type 2) at boot and opens what it finds; xam.cpp answers with
// these packages, and each package is unpacked once into "<package>.d\" so its files can be opened
// like a save folder's.
//
// STFS layout (Xenia's stfs_container_device, public STFS notes): big-endian header; content type at
// 0x344, header size 0x340, display name (UTF-16BE) 0x411; volume descriptor at 0x379 (+2 block
// separation, +3 file table block count (LE16), +5 file table block (LE24)); data blocks of 0x1000
// interleaved with hash tables, one per 0xAA blocks (level 0) and per 0x70E4 (level 1), doubled
// unless the package keeps one table per level. File table entries are 0x40 bytes: name (0x28),
// flags (+0x28: name length, 0x40 consecutive, 0x80 directory), block count (+0x29, LE24), first
// block (+0x2F, LE24), parent entry (+0x32, BE16, -1 at the root), size (+0x34, BE32).
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "guest.h"
#include "dlc.h"

namespace
{
    constexpr uint32_t kBlock = 0x1000, kTitleId = 0x535107E8, kMarketplaceContent = 2;

    std::string DlcFolder()
    {
        char value[MAX_PATH];
        const std::string base = GetEnvironmentVariableA("NIER_DLC_DIR", value, sizeof(value)) ? std::string(value) : g_gameDir + "\\..\\dlc";
        char title[16];
        snprintf(title, sizeof(title), "%08X", kTitleId);
        return base + "\\" + title + "\\00000002";
    }

    uint32_t U24LE(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16; }
    uint32_t U32BE(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

    struct Package
    {
        FILE* file = nullptr;
        std::vector<uint8_t> header;
        uint32_t firstTable = 0, shift = 0;

        bool Open(const std::string& path)
        {
            file = fopen(path.c_str(), "rb");
            if (!file) return false;
            header.resize(0xA000);
            if (fread(header.data(), 1, header.size(), file) != header.size()) return false;
            if (memcmp(header.data(), "LIVE", 4) && memcmp(header.data(), "CON ", 4) && memcmp(header.data(), "PIRS", 4)) return false;
            firstTable = (U32BE(&header[0x340]) + 0xFFF) & ~0xFFFu;
            shift = (firstTable == 0xB000 || (header[0x379 + 2] & 1)) ? 0 : 1;
            return true;
        }
        ~Package() { if (file) fclose(file); }

        uint32_t ContentType() const { return U32BE(&header[0x344]); }

        uint64_t BlockOffset(uint32_t block) const
        {
            uint64_t number = (uint64_t((block + 0xAA) / 0xAA) << shift) + block;
            if (block >= 0xAA)
            {
                number += uint64_t((block + 0x70E4) / 0x70E4) << shift;
                if (block >= 0x70E4) number += 1ull << shift;
            }
            return firstTable + number * kBlock;
        }

        bool ReadBlock(uint32_t block, uint8_t* out)
        {
            return _fseeki64(file, int64_t(BlockOffset(block)), SEEK_SET) == 0 && fread(out, 1, kBlock, file) == kBlock;
        }
    };

    std::mutex gMountMutex;

    // Unpacks every file of the package into directory. Files stored in consecutive blocks only
    // (all the packages seen so far); a chained file is reported and skipped.
    bool Unpack(Package& package, const std::string& directory)
    {
        const uint8_t* vd = &package.header[0x379];
        const uint32_t tableBlocks = vd[3] | vd[4] << 8, tableBlock = U24LE(vd + 5);
        std::vector<uint8_t> table(size_t(tableBlocks) * kBlock);
        for (uint32_t i = 0; i < tableBlocks; i++)
            if (!package.ReadBlock(tableBlock + i, &table[size_t(i) * kBlock])) return false;

        struct Entry { std::string name; bool directory, consecutive; uint32_t blocks, start, size; int parent; };
        std::vector<Entry> entries;
        for (size_t at = 0; at + 0x40 <= table.size(); at += 0x40)
        {
            const uint8_t* e = &table[at];
            if (e[0] == 0) { entries.push_back({}); continue; }
            entries.push_back({ std::string((const char*)e, e[0x28] & 0x3F), (e[0x28] & 0x80) != 0, (e[0x28] & 0x40) != 0,
                U24LE(e + 0x29), U24LE(e + 0x2F), U32BE(e + 0x34), int16_t(e[0x32] << 8 | e[0x33]) });
        }
        auto pathOf = [&](int index) {
            std::string path;
            for (int i = index, guard = 0; i >= 0 && i < int(entries.size()) && guard < 64; i = entries[i].parent, guard++)
                path = "\\" + entries[i].name + path;
            return directory + path;
        };

        CreateDirectoryA(directory.c_str(), nullptr);
        std::vector<uint8_t> block(kBlock);
        for (size_t i = 0; i < entries.size(); i++)
        {
            const Entry& e = entries[i];
            if (e.name.empty()) continue;
            const std::string path = pathOf(int(i));
            if (e.directory) { CreateDirectoryA(path.c_str(), nullptr); continue; }
            if (!e.consecutive) { Log("[dlc] %s is stored in chained blocks, which aren't supported yet; skipped\n", path.c_str()); continue; }
            FILE* out = fopen(path.c_str(), "wb");
            if (!out) { Log("[dlc] couldn't write %s\n", path.c_str()); return false; }
            uint32_t left = e.size;
            for (uint32_t b = 0; left > 0; b++)
            {
                if (!package.ReadBlock(e.start + b, block.data())) { fclose(out); return false; }
                const uint32_t n = left < kBlock ? left : kBlock;
                fwrite(block.data(), 1, n, out);
                left -= n;
            }
            fclose(out);
        }
        return true;
    }
}

std::vector<std::array<uint8_t, 0x134>> DlcList()
{
    std::vector<std::array<uint8_t, 0x134>> items;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA((DlcFolder() + "\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return items;
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Package package;
        if (!package.Open(DlcFolder() + "\\" + found.cFileName) || package.ContentType() != kMarketplaceContent) continue;
        // XCONTENT_DATA: device (+0), content type (+4), display name (+8, 128 UTF-16BE chars), file name (+0x108, 42 bytes).
        std::array<uint8_t, 0x134> item{};
        item[3] = 1;
        item[7] = uint8_t(kMarketplaceContent);
        memcpy(&item[8], &package.header[0x411], 0x100);
        strncpy((char*)&item[0x108], found.cFileName, 42);
        items.push_back(item);
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return items;
}

bool DlcMount(const std::string& name, std::string& directory)
{
    const std::string path = DlcFolder() + "\\" + name;
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    directory = path + ".d";
    std::lock_guard lock(gMountMutex);
    const std::string done = directory + "\\.unpacked";
    if (GetFileAttributesA(done.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    Package package;
    if (!package.Open(path)) { Log("[dlc] %s isn't an STFS package\n", path.c_str()); return false; }
    Log("[dlc] unpacking %s (once)\n", name.c_str());
    if (!Unpack(package, directory)) { Log("[dlc] unpacking %s failed\n", name.c_str()); return false; }
    if (FILE* marker = fopen(done.c_str(), "wb")) fclose(marker);
    Log("[dlc] unpacked into %s\n", directory.c_str());
    return true;
}
