// File access. Guest paths map into the game folder, which is read-only. Paths under a save
// content root map into a separate save folder and are the only writable ones.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include "guest.h"

namespace
{
    std::mutex gSaveRootsMutex;
    std::map<std::string, std::string> gSaveRoots;

    std::string NormalizeRoot(std::string name)
    {
        while (!name.empty() && name.back() == ':') name.pop_back();
        for (char& c : name) c = char(std::toupper((unsigned char)c));
        return name;
    }
}

void SaveRootRegister(const std::string& name, const std::string& directory)
{
    std::lock_guard lock(gSaveRootsMutex);
    gSaveRoots[NormalizeRoot(name)] = directory;
}

void SaveRootRemove(const std::string& name)
{
    std::lock_guard lock(gSaveRootsMutex);
    gSaveRoots.erase(NormalizeRoot(name));
}

bool SaveRootFind(const std::string& name, std::string& directory)
{
    std::lock_guard lock(gSaveRootsMutex);
    auto found = gSaveRoots.find(NormalizeRoot(name));
    if (found == gSaveRoots.end()) return false;
    directory = found->second;
    return true;
}

namespace
{
    std::string ReadName(uint32_t attributes)
    {
        uint32_t name = R32(attributes + 4);
        if (!name) return {};
        return std::string(GuestStr(R32(name + 4)), R16(name));
    }

    // Accepts "\Device\Cdrom0\x", "game:\x", "d:\x" and "\??\game:\x" forms.
    std::string HostPath(uint32_t attributes, bool* writable = nullptr)
    {
        std::string guest = ReadName(attributes);
        std::replace(guest.begin(), guest.end(), '/', '\\');
        std::string relative = guest;

        uint32_t root = R32(attributes);
        HandleEntry rootEntry;
        if (root && root != 0xFFFFFFFD && HandleGet(root, rootEntry) && rootEntry.kind == HandleKind::File)
            return rootEntry.path + "\\" + relative;

        auto colon = relative.find(":\\");
        static const char kCdrom[] = "\\Device\\Cdrom0";
        if (colon != std::string::npos)
        {
            std::string saveDirectory;
            std::string rootName = relative.substr(0, colon);
            if (rootName.rfind("\\??\\", 0) == 0) rootName = rootName.substr(4);
            if (SaveRootFind(rootName, saveDirectory))
            {
                if (writable) *writable = true;
                return saveDirectory + "\\" + relative.substr(colon + 2);
            }
            relative = relative.substr(colon + 2);
        }
        else if (relative.rfind(kCdrom, 0) == 0) relative = relative.substr(sizeof(kCdrom) - 1);
        while (!relative.empty() && relative[0] == '\\') relative.erase(0, 1);
        return relative.empty() ? g_gameDir : g_gameDir + "\\" + relative;
    }

    void WriteNetworkOpenInfo(uint32_t info, const WIN32_FILE_ATTRIBUTE_DATA& data)
    {
        auto time = [](const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; };
        uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        W64(info + 0x00, time(data.ftCreationTime));
        W64(info + 0x08, time(data.ftLastAccessTime));
        W64(info + 0x10, time(data.ftLastWriteTime));
        W64(info + 0x18, time(data.ftLastWriteTime));
        W64(info + 0x20, (size + 0x7FF) & ~0x7FFull);
        W64(info + 0x28, size);
        W32(info + 0x30, data.dwFileAttributes);
        W32(info + 0x34, 0);
    }

    void OpenCommon(PPCContext& ctx, uint32_t handlePtr, uint32_t access, uint32_t attributes, uint32_t iosb, uint32_t disposition)
    {
        bool writable = false;
        std::string path = HostPath(attributes, &writable);
        bool wantsWrite = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
        Log("open \"%s\" -> %s%s\n", ReadName(attributes).c_str(), path.c_str(), wantsWrite && !writable ? " (write refused)" : "");

        uint32_t status = X_STATUS_SUCCESS;
        HANDLE host = INVALID_HANDLE_VALUE;
        if (wantsWrite && !writable) status = X_STATUS_ACCESS_DENIED;
        else
        {
            // Guest dispositions: supersede, open, create, open-if, overwrite, overwrite-if.
            static const DWORD kCreation[] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS };
            const DWORD creation = (writable && disposition <= 5) ? kCreation[disposition] : OPEN_EXISTING;
            const DWORD hostAccess = writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
            const DWORD share = writable ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
            host = CreateFileA(path.c_str(), hostAccess, share, nullptr, creation, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (host == INVALID_HANDLE_VALUE) status = X_STATUS_OBJECT_NAME_NOT_FOUND;
        }
        if (iosb) { W32(iosb, status); W32(iosb + 4, status == X_STATUS_SUCCESS ? 1 : 0); }
        if (status == X_STATUS_SUCCESS) W32(handlePtr, HandleCreate({ HandleKind::File, 0, host, path }));
        ctx.r3.u64 = status;
    }

    bool FileEntry(uint32_t handle, HandleEntry& entry)
    {
        return HandleGet(handle, entry) && entry.kind == HandleKind::File;
    }
}

GUEST_IMPORT(NtCreateFile) { OpenCommon(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32); }
GUEST_IMPORT(NtOpenFile) { OpenCommon(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1); }

GUEST_IMPORT(NtWriteFile)
{
    HandleEntry entry;
    if (!FileEntry(ctx.r3.u32, entry)) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offset = ctx.r10.u32;
    if (offset)
    {
        LARGE_INTEGER position;
        position.QuadPart = int64_t(R64(offset));
        if (position.QuadPart >= 0) SetFilePointerEx(entry.host, position, nullptr, FILE_BEGIN);
    }
    DWORD done = 0;
    uint32_t status = WriteFile(entry.host, g_base + buffer, length, &done, nullptr) ? X_STATUS_SUCCESS : 0xC0000185;
    if (iosb) { W32(iosb, status); W32(iosb + 4, done); }
    if (event)
    {
        HandleEntry eventEntry;
        if (HandleGet(event, eventEntry)) SetEventObject(eventEntry.object);
    }
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    HandleEntry entry;
    if (!FileEntry(ctx.r3.u32, entry)) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    FlushFileBuffers(entry.host);
    if (ctx.r4.u32) { W32(ctx.r4.u32, X_STATUS_SUCCESS); W32(ctx.r4.u32 + 4, 0); }
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtReadFile)
{
    HandleEntry entry;
    if (!FileEntry(ctx.r3.u32, entry)) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t event = ctx.r4.u32, iosb = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offset = ctx.r10.u32;
    if (offset)
    {
        LARGE_INTEGER position;
        position.QuadPart = int64_t(R64(offset));
        if (position.QuadPart >= 0) SetFilePointerEx(entry.host, position, nullptr, FILE_BEGIN);
    }
    DWORD done = 0;
    BOOL ok = ReadFile(entry.host, g_base + buffer, length, &done, nullptr);
    uint32_t status = !ok ? 0xC0000185 : (done == 0 && length != 0) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    if (entry.path.size() > 4 && _stricmp(entry.path.c_str() + entry.path.size() - 4, ".SFD") == 0)
    {
        static std::atomic<uint32_t> movieReads{ 0 };
        static std::atomic<uint64_t> movieBytes{ 0 };
        uint32_t count = movieReads.fetch_add(1) + 1;
        uint64_t bytes = movieBytes.fetch_add(done) + done;
        if (count <= 4 || count % 32 == 0) Log("[movie] read #%u, %u bytes this read, %llu total\n", count, done, bytes);
    }
    if (iosb) { W32(iosb, status); W32(iosb + 4, done); }
    if (event)
    {
        HandleEntry eventEntry;
        if (HandleGet(event, eventEntry)) SetEventObject(eventEntry.object);
    }
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtQueryFullAttributesFile)
{
    std::string path = HostPath(ctx.r3.u32);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) { ctx.r3.u64 = X_STATUS_OBJECT_NAME_NOT_FOUND; return; }
    WriteNetworkOpenInfo(ctx.r4.u32, data);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtQueryInformationFile)
{
    HandleEntry entry;
    if (!FileEntry(ctx.r3.u32, entry)) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, cls = ctx.r7.u32;
    WIN32_FILE_ATTRIBUTE_DATA data;
    GetFileAttributesExA(entry.path.c_str(), GetFileExInfoStandard, &data);
    uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    uint32_t status = X_STATUS_SUCCESS;
    switch (cls)
    {
    case 5:  // standard information
        W64(info, (size + 0x7FF) & ~0x7FFull);
        W64(info + 8, size);
        W32(info + 0x10, 1);
        W8(info + 0x14, 0);
        W8(info + 0x15, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
        break;
    case 14: // position
    {
        LARGE_INTEGER zero{}, position{};
        SetFilePointerEx(entry.host, zero, &position, FILE_CURRENT);
        W64(info, uint64_t(position.QuadPart));
        break;
    }
    case 34: // network open
        WriteNetworkOpenInfo(info, data);
        break;
    default:
        Log("NtQueryInformationFile: unhandled class %u\n", cls);
        status = X_STATUS_NOT_IMPLEMENTED;
        break;
    }
    if (iosb) { W32(iosb, status); W32(iosb + 4, status == X_STATUS_SUCCESS ? length : 0); }
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtSetInformationFile)
{
    HandleEntry entry;
    if (!FileEntry(ctx.r3.u32, entry)) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t iosb = ctx.r4.u32, info = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t status = X_STATUS_SUCCESS;
    if (cls == 14)
    {
        LARGE_INTEGER position;
        position.QuadPart = int64_t(R64(info));
        SetFilePointerEx(entry.host, position, nullptr, FILE_BEGIN);
    }
    else if (cls == 20)  // end of file
    {
        LARGE_INTEGER position;
        position.QuadPart = int64_t(R64(info));
        SetFilePointerEx(entry.host, position, nullptr, FILE_BEGIN);
        SetEndOfFile(entry.host);
    }
    else if (cls == 19)  // allocation size: only a hint
    {
    }
    else
    {
        Log("NtSetInformationFile: unhandled class %u\n", cls);
        status = X_STATUS_NOT_IMPLEMENTED;
    }
    if (iosb) { W32(iosb, status); W32(iosb + 4, 0); }
    ctx.r3.u64 = status;
}
