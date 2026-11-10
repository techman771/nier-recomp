// Xbox system services: asynchronous requests, content enumeration and a local signed-in profile.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
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

namespace
{
    // XCONTENT_DATA: device +0, type +4, display name (128 UTF-16 chars) +8, file name +0x108.
    constexpr uint32_t kContentDataBytes = 0x134;

    struct Enumerator
    {
        std::vector<std::array<uint8_t, kContentDataBytes>> items;
        size_t position = 0;
        uint32_t itemsPerCall = 1;
    };
    std::mutex gEnumeratorMutex;
    std::map<uint32_t, Enumerator> gEnumerators;
    uint32_t gNextEnumerator = 0xFA000001;

    std::string SaveFolder() { return g_gameDir + "\\..\\save"; }

    // Each package's XCONTENT_DATA is kept beside its folder so enumeration can return it unchanged.
    std::string MetadataPath(const std::string& package) { return SaveFolder() + "\\" + package + ".content"; }

    std::vector<std::array<uint8_t, kContentDataBytes>> ListContent(uint32_t type)
    {
        std::vector<std::array<uint8_t, kContentDataBytes>> items;
        WIN32_FIND_DATAA found;
        HANDLE search = FindFirstFileA((SaveFolder() + "\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return items;
        do
        {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.') continue;
            std::array<uint8_t, kContentDataBytes> item{};
            if (FILE* file = fopen(MetadataPath(found.cFileName).c_str(), "rb"))
            {
                fread(item.data(), 1, item.size(), file);
                fclose(file);
            }
            else
            {
                // A package from before metadata existed: describe it as a saved game named after its folder.
                item[7] = 1;
                for (size_t i = 0; found.cFileName[i] && i < 127; i++) item[8 + i * 2 + 1] = uint8_t(found.cFileName[i]);
                strncpy((char*)item.data() + 0x108, found.cFileName, 41);
            }
            item[0] = 0; item[1] = 0; item[2] = 0; item[3] = 1;   // device 1, big-endian
            const uint32_t itemType = (uint32_t(item[4]) << 24) | (uint32_t(item[5]) << 16) | (uint32_t(item[6]) << 8) | item[7];
            if (type == 0 || itemType == type) items.push_back(item);
        } while (FindNextFileA(search, &found));
        FindClose(search);
        return items;
    }
}

GUEST_IMPORT(XamContentCreateEnumerator)
{
    const uint32_t device = ctx.r4.u32, type = ctx.r5.u32, itemsPerCall = ctx.r7.u32;
    const uint32_t bufferSizeOut = ctx.r8.u32, handleOut = ctx.r9.u32;
    Enumerator enumerator;
    if (device == 0 || device == 1) enumerator.items = ListContent(type);
    enumerator.itemsPerCall = itemsPerCall ? itemsPerCall : 1;
    const uint32_t bufferSize = enumerator.itemsPerCall * kContentDataBytes;
    uint32_t handle;
    {
        std::lock_guard lock(gEnumeratorMutex);
        handle = gNextEnumerator++;
        Log("[xam] content enumerator: type %u device %u, %zu items\n", type, device, enumerator.items.size());
        gEnumerators[handle] = std::move(enumerator);
    }
    if (bufferSizeOut) W32(bufferSizeOut, bufferSize);
    if (handleOut) W32(handleOut, handle);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamEnumerate)
{
    const uint32_t handle = ctx.r3.u32, buffer = ctx.r5.u32, bufferLength = ctx.r6.u32;
    const uint32_t itemsReturned = ctx.r7.u32, overlapped = ctx.r8.u32;
    uint32_t count = 0;
    {
        std::lock_guard lock(gEnumeratorMutex);
        auto found = gEnumerators.find(handle);
        if (found != gEnumerators.end())
        {
            Enumerator& e = found->second;
            const uint32_t room = std::min<uint32_t>(e.itemsPerCall, bufferLength / kContentDataBytes);
            while (count < room && e.position < e.items.size())
            {
                memcpy(g_base + buffer + count * kContentDataBytes, e.items[e.position].data(), kContentDataBytes);
                e.position++;
                count++;
            }
        }
    }
    const uint32_t result = count ? 0 : X_ERROR_NO_MORE_FILES;
    if (itemsReturned) W32(itemsReturned, count);
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result, count);
        ctx.r3.u64 = X_ERROR_IO_PENDING;
        return;
    }
    ctx.r3.u64 = result;
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
        if (FILE* file = fopen(MetadataPath(name).c_str(), "wb"))
        {
            fwrite(g_base + content, 1, kContentDataBytes, file);
            fclose(file);
        }
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

// There's no on-screen keyboard yet: accept the game's suggested text, or NIER_PLAYER_NAME if set.
namespace
{
    // Stands in for the console's on-screen keyboard. Windows-only for now; the cross-platform
    // version belongs with the SDL platform layer.
    struct KeyboardRequest
    {
        std::wstring title, description, text;
        uint32_t buffer = 0, length = 0, overlapped = 0;
        bool accepted = false;
    };

    constexpr int kEditId = 100;

    std::wstring ReadGuestWide(uint32_t address)
    {
        std::wstring text;
        if (address) for (uint32_t i = 0; i < 512 && R16(address + i * 2); i++) text += wchar_t(R16(address + i * 2));
        return text;
    }

    LRESULT CALLBACK KeyboardProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        auto* request = reinterpret_cast<KeyboardRequest*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message)
        {
        case WM_COMMAND:
            if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)
            {
                if (LOWORD(w) == IDOK && request)
                {
                    wchar_t text[256];
                    GetDlgItemTextW(window, kEditId, text, 256);
                    request->text = text;
                    request->accepted = true;
                }
                DestroyWindow(window);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w, l);
        }
    }

    void RunKeyboard(KeyboardRequest& request)
    {
        static bool registered = false;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered)
        {
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = KeyboardProc;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = L"NierKeyboard";
            windowClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&windowClass);
            registered = true;
        }
        const std::wstring title = request.title.empty() ? L"Enter text" : request.title;
        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"NierKeyboard", title.c_str(), WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 420, 170, nullptr, nullptr, instance, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(&request));
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label = CreateWindowW(L"STATIC", request.description.c_str(), WS_CHILD | WS_VISIBLE, 12, 10, 380, 36, window, nullptr, instance, nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", request.text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 50, 380, 24, window, HMENU(INT_PTR(kEditId)), instance, nullptr);
        HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 212, 88, 85, 26, window, HMENU(INT_PTR(IDOK)), instance, nullptr);
        HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 307, 88, 85, 26, window, HMENU(INT_PTR(IDCANCEL)), instance, nullptr);
        for (HWND child : { label, edit, ok, cancel }) SendMessageW(child, WM_SETFONT, WPARAM(font), TRUE);
        if (request.length > 1) SendMessageW(edit, EM_LIMITTEXT, request.length - 1, 0);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        SetFocus(edit);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(window, &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    void FinishKeyboard(const KeyboardRequest& request)
    {
        constexpr uint32_t kCancelled = 1223;
        std::wstring text = request.text;
        if (request.length) text.resize(std::min<size_t>(text.size(), request.length - 1));
        if (request.accepted)
        {
            for (size_t i = 0; i < text.size(); i++) W16(request.buffer + uint32_t(i) * 2, uint16_t(text[i]));
            W16(request.buffer + uint32_t(text.size()) * 2, 0);
        }
        std::string ascii;
        for (wchar_t c : text) ascii += c < 128 ? char(c) : '?';
        Log("[xam] keyboard %s \"%s\"\n", request.accepted ? "returned" : "cancelled", ascii.c_str());
        CompleteOverlapped(request.overlapped, request.accepted ? 0 : kCancelled, 0);
    }
}

// Arguments: user, flags, default text, title, description, buffer, length (characters), overlapped.
GUEST_IMPORT(XamShowKeyboardUI)
{
    auto request = std::make_shared<KeyboardRequest>();
    request->text = ReadGuestWide(ctx.r5.u32);
    request->title = ReadGuestWide(ctx.r6.u32);
    request->description = ReadGuestWide(ctx.r7.u32);
    request->buffer = ctx.r8.u32;
    request->length = ctx.r9.u32;
    request->overlapped = ctx.r10.u32;

    // Scripted runs set the answer in advance instead of showing a window.
    char preset[64];
    if (GetEnvironmentVariableA("NIER_PLAYER_NAME", preset, sizeof(preset)))
    {
        request->text.assign(preset, preset + strlen(preset));
        request->accepted = true;
        FinishKeyboard(*request);
        ctx.r3.u64 = request->overlapped ? X_ERROR_IO_PENDING : 0;
        return;
    }

    if (!request->overlapped)
    {
        RunKeyboard(*request);
        FinishKeyboard(*request);
        ctx.r3.u64 = request->accepted ? 0 : 1223;
        return;
    }
    std::thread([request] { RunKeyboard(*request); FinishKeyboard(*request); }).detach();
    ctx.r3.u64 = X_ERROR_IO_PENDING;
}

// Thumbnails are only shown in the console's storage menus, so they're accepted and not kept.
// Arguments, from NieR's call site: user, content data, buffer, size, overlapped.
GUEST_IMPORT(XamContentSetThumbnail)
{
    const uint32_t overlapped = ctx.r7.u32;
    CompleteOverlapped(overlapped, 0, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : 0;
}

GUEST_IMPORT(XamContentGetThumbnail)
{
    constexpr uint32_t kFileNotFound = 2;
    const uint32_t sizePointer = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (sizePointer) W32(sizePointer, 0);
    CompleteOverlapped(overlapped, kFileNotFound, 0);
    ctx.r3.u64 = overlapped ? X_ERROR_IO_PENDING : kFileNotFound;
}

GUEST_IMPORT(XamContentGetCreator)
{
    const uint32_t isCreator = ctx.r5.u32, xuid = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (isCreator) W32(isCreator, 1);
    if (xuid) W64(xuid, kXuid);
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
