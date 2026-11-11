// Unpacks default.xex into guest memory, builds the indirect-call table and fills data imports.
#include <windows.h>
#include <file.h>
#include <image.h>
#include <xex.h>
#include "guest.h"

std::string g_gameDir;

namespace
{
    uint32_t gTimeBundle = 0;
    uint32_t gXexHeader = 0;

    struct VariableImport { uint16_t ordinal; const char* name; uint32_t size; };
    const VariableImport kVariables[] = {
        { 0x000C, "ExConsoleGameRegion", 8 }, { 0x000E, "ExEventObjectType", 0x100 }, { 0x0012, "ExMutantObjectType", 0x100 },
        { 0x0017, "ExSemaphoreObjectType", 0x100 }, { 0x001B, "ExThreadObjectType", 0x100 }, { 0x001C, "ExTimerObjectType", 0x100 },
        { 0x0036, "IoCompletionObjectType", 0x100 }, { 0x003A, "IoDeviceObjectType", 0x100 }, { 0x003E, "IoFileObjectType", 0x100 },
        { 0x0059, "KeDebugMonitorData", 0x100 }, { 0x00AD, "KeTimeStampBundle", 0x20 }, { 0x00B5, "KiBugCheckData", 0x100 },
        { 0x00CB, "Nls844UnicodeCaseTable", 0x10000 }, { 0x0106, "ObDirectoryObjectType", 0x100 }, { 0x0112, "ObSymbolicLinkObjectType", 0x100 },
        { 0x0156, "XboxHardwareInfo", 0x10 }, { 0x0157, "XboxKrnlBaseVersion", 8 }, { 0x0158, "XboxKrnlVersion", 8 },
        { 0x0193, "XexExecutableModuleHandle", 8 }, { 0x01AE, "ExLoadedCommandLine", 0x400 }, { 0x01AF, "ExLoadedImageName", 0x100 },
        { 0x01BE, "VdGlobalDevice", 0x100 }, { 0x01BF, "VdGlobalXamDevice", 0x100 }, { 0x01C0, "VdGpuClockInMHz", 8 },
        { 0x01C1, "VdHSIOCalibrationLock", 0x100 }, { 0x025C, "HalFsbInterruptCount", 8 }, { 0x0266, "KeCertMonitorData", 0x100 },
        { 0x026B, "VdHSIOTrainCount", 8 }, { 0x026D, "VdHSIOTrainingStatus", 8 }, { 0x026E, "RgcBindInfo", 0x100 },
        { 0x02AB, "HalFsbResetCount", 8 }, { 0x02DB, "UsbdBootEnumerationDoneEvent", 0x20 }, { 0x02DC, "StfsDeviceErrorEvent", 0x20 },
        { 0x02F1, "UsbdDriverLoadRequiredEvent", 0x20 }, { 0x0345, "XboxKrnlVersion4Digit", 8 },
    };

    void InitVariable(uint16_t ordinal, uint32_t block, const std::vector<uint8_t>& xex)
    {
        switch (ordinal)
        {
        case 0x000C: W32(block, 0x000000FF); break;                 // all regions
        case 0x00AD: gTimeBundle = block; UpdateTimeBundle(); break;
        case 0x0156: W32(block, 0x00000020); W8(block + 4, 6); break; // six hardware threads
        case 0x0157: case 0x0158:
            W16(block, 2); W16(block + 2, 0); W16(block + 4, 9328); W16(block + 6, 0);
            break;
        case 0x01AE: strcpy((char*)g_base + block, "\"default.xex\""); break;
        case 0x01AF: strcpy((char*)g_base + block, "\\Device\\Cdrom0\\default.xex"); break;
        case 0x01C0: W32(block, 500); break;
        case 0x0193:
        {
            // A module handle is a loader entry whose field at +0x58 points at the XEX header.
            uint32_t headerSize = __builtin_bswap32(*(uint32_t*)(xex.data() + 8));
            gXexHeader = GuestAlloc(headerSize, 0x10, Region::Heap);
            memcpy(g_base + gXexHeader, xex.data(), headerSize);
            uint32_t entry = GuestAlloc(0x100, 0x10, Region::Heap);
            W32(entry + 0x58, gXexHeader);
            W32(block, entry);
            break;
        }
        default: break;
        }
    }
}

uint32_t XexHeaderGuest() { return gXexHeader; }

void UpdateTimeBundle()
{
    if (!gTimeBundle) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t system = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    static const uint64_t start = system;
    uint64_t elapsed = system - start;   // 100 ns units
    // NIER_FIXED_TIME=1 (experiment): elapsed time advances 1/60 s per frame the game submits.
    static const bool fixed = GetEnvironmentVariableA("NIER_FIXED_TIME", nullptr, 0) > 0;
    if (fixed) elapsed = FixedElapsed100ns();
    W64(gTimeBundle, elapsed);
    W64(gTimeBundle + 8, start + elapsed);
    W32(gTimeBundle + 0x10, uint32_t(elapsed / 10000));
}

uint32_t LoadGame(const std::string& gameDir)
{
    g_gameDir = gameDir;
    const std::string path = gameDir + "\\default.xex";
    auto file = LoadFile(path);
    if (file.empty()) { Log("cannot read %s\n", path.c_str()); ExitProcess(1); }

    auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data) { Log("cannot unpack %s\n", path.c_str()); ExitProcess(1); }
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        Log("this executable does not match the recompiled code (base 0x%zX size 0x%X)\n", image.base, image.size);
        ExitProcess(1);
    }

    // With basic compression the unpacked buffer only covers the file's blocks, which can be
    // smaller than the image size. Everything past it is zero, as guest memory already is.
    size_t backed = image.size;
    auto* format = reinterpret_cast<const Xex2OptFileFormatInfo*>(getOptHeaderPtr(file.data(), XEX_HEADER_FILE_FORMAT_INFO));
    if (format && format->compressionType == XEX_COMPRESSION_BASIC)
    {
        auto* blocks = reinterpret_cast<const Xex2FileBasicCompressionBlock*>(format + 1);
        size_t count = (format->infoSize / sizeof(Xex2FileBasicCompressionInfo)) - 1;
        backed = 0;
        for (size_t i = 0; i < count; i++) backed += uint32_t(blocks[i].dataSize) + uint32_t(blocks[i].zeroSize);
    }
    memcpy(g_base + image.base, image.data.get(), std::min<size_t>(backed, image.size));
    Log("image: 0x%X bytes, 0x%zX backed by the file\n", image.size, backed);
    // NIER_DUMP_IMAGE=1 writes the decompressed image to captures/image.bin (debugging; game-derived).
    if (GetEnvironmentVariableA("NIER_DUMP_IMAGE", nullptr, 0) > 0)
    {
        CreateDirectoryA("..\\captures", nullptr);
        if (FILE* file = fopen("..\\captures\\image.bin", "wb"))
        {
            fwrite(g_base + image.base, 1, image.size, file);
            fclose(file);
            Log("image dumped to captures/image.bin\n");
        }
    }

    size_t mapped = 0;
    for (auto* m = PPCFuncMappings; m->host; ++m, ++mapped)
        PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
    Log("mapped %zu functions, entry 0x%08zX\n", mapped, image.entry_point);
    GpuInstallHooks();
    XamInstallHooks();
    // NIER_POKE32=<address>=<value>,... (experiments): overwrites 32-bit words of the loaded image.
    if (char pokes[256]; GetEnvironmentVariableA("NIER_POKE32", pokes, sizeof(pokes)))
        for (char* item = strtok(pokes, ","); item; item = strtok(nullptr, ","))
            if (char* equals = strchr(item, '='))
            {
                const uint32_t address = strtoul(item, nullptr, 16), value = strtoul(equals + 1, nullptr, 16);
                Log("[poke] %08X: %08X -> %08X\n", address, R32(address), value);
                W32(address, value);
            }

    // ParseImage leaves non-thunk import slots holding their record in host byte order.
    auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
    size_t filled = 0, unknown = 0;
    auto* library = (const Xex2ImportLibrary*)((const char*)imports + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
    for (uint32_t i = 0; i < imports->numImports; i++)
    {
        auto* descriptors = (const Xex2ImportDescriptor*)(library + 1);
        bool kernel = library->name == 1;
        for (uint32_t d = 0; d < library->numberOfImports; d++)
        {
            uint32_t slot = descriptors[d].firstThunk;
            uint32_t record = *(uint32_t*)(g_base + slot);
            if ((record >> 24) != 0) continue;  // rewritten function thunk
            uint16_t ordinal = uint16_t(record);
            *(uint32_t*)(g_base + slot) = 0;
            if (!kernel) continue;
            for (const auto& variable : kVariables)
            {
                if (variable.ordinal != ordinal) continue;
                uint32_t block = GuestAlloc(variable.size, 0x10, Region::Heap);
                W32(slot, block);
                InitVariable(ordinal, block, file);
                Log("data import %s -> 0x%08X\n", variable.name, block);
                filled++;
            }
        }
        library = (const Xex2ImportLibrary*)((const char*)(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
    }
    Log("filled %zu data imports\n", filled);
    (void)unknown;
    return uint32_t(image.entry_point);
}
