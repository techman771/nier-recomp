// Xbox system services: asynchronous requests, content enumeration and a local signed-in profile.
#include <windows.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include "guest.h"

namespace
{
    constexpr uint32_t X_ERROR_IO_PENDING = 997, X_ERROR_NO_MORE_FILES = 18;
    constexpr uint64_t kXuid = 0xE000000000000001ull;
    constexpr char kGamertag[] = "Player";
    std::atomic<int> gMessagesLogged{ 0 };

    // XOVERLAPPED: result +0, length +4, event handle +0xC, extended error +0x18.
    void CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
    {
        if (!overlapped) return;
        W32(overlapped + 0x18, result);
        W32(overlapped + 4, length);
        MemoryBarrier();
        W32(overlapped, result);
        uint32_t event = R32(overlapped + 0x0C);
        HandleEntry entry;
        if (event && HandleGet(event, entry)) SetEventObject(entry.object);
    }

    void LogMessage(const char* name, PPCContext& ctx)
    {
        if (gMessagesLogged.fetch_add(1) < 24)
            Log("[xam] %s app 0x%X message 0x%X r5=%08X r6=%08X r7=%08X lr=%08llX\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.lr);
    }
}

namespace
{
    // Delivered once, as a console does for a freshly started title with a profile signed in.
    constexpr uint32_t XN_SYS_SIGNINCHANGED = 0x0A, XN_SYS_STORAGEDEVICESCHANGED = 0x0B, XN_SYS_INPUTDEVICESCHANGED = 0x12;
    std::mutex gNotifyMutex;
    std::deque<std::pair<uint32_t, uint32_t>> gNotifications;
    std::atomic<int> gNotificationsLogged{ 0 };
    constexpr uint32_t kListenerHandle = 0xFB000001;
}

GUEST_IMPORT(XamNotifyCreateListener)
{
    {
        std::lock_guard lock(gNotifyMutex);
        gNotifications.push_back({ XN_SYS_SIGNINCHANGED, 1 });
        gNotifications.push_back({ XN_SYS_STORAGEDEVICESCHANGED, 0 });
        gNotifications.push_back({ XN_SYS_INPUTDEVICESCHANGED, 0 });
    }
    Log("[xam] notification listener created, mask 0x%X\n", ctx.r3.u32);
    ctx.r3.u64 = kListenerHandle;
}

GUEST_IMPORT(XNotifyGetNext)
{
    uint32_t match = ctx.r4.u32, idOut = ctx.r5.u32, parameterOut = ctx.r6.u32;
    std::lock_guard lock(gNotifyMutex);
    for (auto it = gNotifications.begin(); it != gNotifications.end(); ++it)
    {
        if (match && it->first != match) continue;
        if (idOut) W32(idOut, it->first);
        if (parameterOut) W32(parameterOut, it->second);
        if (gNotificationsLogged.fetch_add(1) < 12) Log("[xam] delivered notification 0x%X param %u\n", it->first, it->second);
        gNotifications.erase(it);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XMsgStartIORequest)
{
    LogMessage("XMsgStartIORequest", ctx);
    uint32_t overlapped = ctx.r5.u32;
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

GUEST_IMPORT(XMsgStartIORequestEx)
{
    LogMessage("XMsgStartIORequestEx", ctx);
    uint32_t overlapped = ctx.r5.u32;
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

GUEST_IMPORT(XMsgInProcessCall)
{
    LogMessage("XMsgInProcessCall", ctx);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentCreateEnumerator)
{
    uint32_t bufferSize = ctx.r9.u32, handleOut = ctx.r10.u32;
    if (bufferSize) W32(bufferSize, 0x200);
    if (handleOut) W32(handleOut, 0xFA000001);
    ctx.r3.u64 = 0;
}

// Reports that no save data or downloadable content exists.
GUEST_IMPORT(XamEnumerate)
{
    uint32_t itemsReturned = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (itemsReturned) W32(itemsReturned, 0);
    if (overlapped)
    {
        CompleteOverlapped(overlapped, X_ERROR_NO_MORE_FILES, 0);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = X_ERROR_NO_MORE_FILES;
}

// Maps a save package to a folder beside the game files. X_CONTENT_DATA holds its file name at +0x108.
GUEST_IMPORT(XamContentCreateEx)
{
    constexpr uint32_t kCreated = 1, kOpened = 2;
    constexpr uint32_t kCreateNew = 1, kOpenExisting = 3, kTruncateExisting = 5;
    constexpr uint32_t kPathNotFound = 3, kAlreadyExists = 183;
    const std::string root = GuestStr(ctx.r4.u32);
    const uint32_t content = ctx.r5.u32, flags = ctx.r6.u32, dispositionOut = ctx.r7.u32, licenseOut = ctx.r8.u32;
    const uint32_t overlapped = StackArg(ctx, 8);

    char name[43]{};
    memcpy(name, g_base + content + 0x108, 42);
    const std::string saveFolder = g_gameDir + "\\..\\save";
    const std::string directory = saveFolder + "\\" + name;
    CreateDirectoryA(saveFolder.c_str(), nullptr);

    const DWORD attributes = GetFileAttributesA(directory.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    const uint32_t mode = flags & 0xF;
    uint32_t result = 0;
    if (exists && mode == kCreateNew) result = kAlreadyExists;
    else if (!exists && (mode == kOpenExisting || mode == kTruncateExisting)) result = kPathNotFound;
    else
    {
        if (!exists) CreateDirectoryA(directory.c_str(), nullptr);
        SaveRootRegister(root, directory);
        if (dispositionOut) W32(dispositionOut, exists ? kOpened : kCreated);
        if (licenseOut) W32(licenseOut, 0);
    }
    Log("[xam] content %s root \"%s\" package \"%s\" flags 0x%X -> %u\n", exists ? "open" : "create", root.c_str(), name, flags, result);
    CompleteOverlapped(overlapped, result, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : result;
}

GUEST_IMPORT(XamContentClose)
{
    SaveRootRemove(GuestStr(ctx.r3.u32));
    const uint32_t overlapped = ctx.r4.u32;
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

// A console shows a picker here; one virtual hard drive means there's only one possible answer.
GUEST_IMPORT(XamShowDeviceSelectorUI)
{
    const uint32_t deviceOut = ctx.r7.u32, overlapped = ctx.r8.u32;
    Log("[xam] device selector: device pointer %08X overlapped %08X\n", deviceOut, overlapped);
    if (deviceOut) W32(deviceOut, 1);
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

GUEST_IMPORT(XamContentGetDeviceState)
{
    const uint32_t device = ctx.r3.u32, overlapped = ctx.r4.u32;
    const uint32_t result = device == 1 ? 0 : 1167;
    CompleteOverlapped(overlapped, result, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : result;
}

GUEST_IMPORT(XamContentGetDeviceData)
{
    const uint32_t device = ctx.r3.u32, data = ctx.r4.u32;
    if (device != 1) { ctx.r3.u64 = 1167; return; }
    memset(g_base + data, 0, 0x50);
    W32(data, 1);                       // device id
    W32(data + 4, 1);                   // hard drive
    W64(data + 8, 0x100000000ull);
    W64(data + 0x10, 0x100000000ull);
    const char name[] = "Hard Drive";
    for (size_t i = 0; i < sizeof(name); i++) W16(data + 0x18 + uint32_t(i) * 2, uint16_t(name[i]));
    ctx.r3.u64 = 0;
}

// Answers every requested setting with "not set", so the game falls back to its defaults.
GUEST_IMPORT(XamUserReadProfileSettings)
{
    constexpr uint32_t kSettingBytes = 0x28;
    const uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, sizePointer = ctx.r9.u32, buffer = ctx.r10.u32;
    const uint32_t overlapped = StackArg(ctx, 8);
    const uint32_t needed = 8 + settingCount * kSettingBytes;
    if (!buffer || R32(sizePointer) < needed)
    {
        W32(sizePointer, needed);
        CompleteOverlapped(overlapped, 122, 0);
        ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 122;
        return;
    }
    memset(g_base + buffer, 0, needed);
    W32(buffer, settingCount);
    W32(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++) W32(buffer + 8 + i * kSettingBytes + 0x10, R32(settingIds + i * 4));
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

GUEST_IMPORT(XamUserGetSigninState) { ctx.r3.u64 = ctx.r3.u32 == 0 ? 1 : 0; }

GUEST_IMPORT(XamUserGetXUID)
{
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = 0x534; return; }
    W64(ctx.r5.u32, kXuid);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetSigninInfo)
{
    uint32_t info = ctx.r5.u32;
    memset(g_base + info, 0, 0x28);
    if (ctx.r3.u32 != 0) { ctx.r3.u64 = 0x534; return; }
    W64(info, kXuid);
    W32(info + 0x0C, 1);
    memcpy(g_base + info + 0x18, kGamertag, sizeof(kGamertag));
    ctx.r3.u64 = 0;
}
