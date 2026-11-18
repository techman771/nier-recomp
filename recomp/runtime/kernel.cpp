// Small kernel and runtime-library imports.
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include "guest.h"

GUEST_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; }  // title process

GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
    uint16_t length = source ? uint16_t(strlen(GuestStr(source))) : 0;
    W16(destination, length);
    W16(destination + 2, source ? length + 1 : 0);
    W32(destination + 4, source);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
    uint16_t length = 0;
    if (source) while (R16(source + length) != 0) length += 2;
    W16(destination, length);
    W16(destination + 2, source ? length + 2 : 0);
    W32(destination + 4, source);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t source = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32, matched = 0;
    while (matched + 4 <= length && R32(source + matched) == pattern) matched += 4;
    ctx.r3.u64 = matched;
}

GUEST_IMPORT(RtlFillMemoryUlong)
{
    uint32_t destination = ctx.r3.u32, length = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= length; i += 4) W32(destination + i, pattern);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t status = ctx.r3.u32;
    ctx.r3.u64 = status == 0 ? 0 : status == X_STATUS_OBJECT_NAME_NOT_FOUND ? 2 : status == X_STATUS_ACCESS_DENIED ? 5 : status == X_STATUS_END_OF_FILE ? 38 : 317;
}

// TIME_FIELDS is eight 16-bit values: year, month, day, hour, minute, second, milliseconds, weekday.
GUEST_IMPORT(RtlTimeToTimeFields)
{
    const uint64_t time = R64(ctx.r3.u32);
    const uint32_t fields = ctx.r4.u32;
    FILETIME ft{ DWORD(time), DWORD(time >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    const uint16_t values[8] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, st.wDayOfWeek };
    for (int i = 0; i < 8; i++) W16(fields + i * 2, values[i]);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    const uint32_t fields = ctx.r3.u32, timeOut = ctx.r4.u32;
    SYSTEMTIME st{};
    st.wYear = R16(fields); st.wMonth = R16(fields + 2); st.wDay = R16(fields + 4); st.wHour = R16(fields + 6);
    st.wMinute = R16(fields + 8); st.wSecond = R16(fields + 10); st.wMilliseconds = R16(fields + 12);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) { ctx.r3.u64 = 0; return; }
    W64(timeOut, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(KeQuerySystemTime)
{
    static const bool logCallers = GetEnvironmentVariableA("NIER_LOG_CLOCK", nullptr, 0) > 0;
    if (logCallers)
    {
        static std::mutex mutex;
        static std::set<std::string> seen;
        std::lock_guard lock(mutex);
        const std::string chain = GuestCallChain(8);
        if (seen.insert(chain).second) Log("[clock] KeQuerySystemTime from%s\n", chain.c_str());
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    W64(ctx.r3.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 0;
}

// The recompiler implements mftb with the host TSC, so the guest must see the TSC's rate.
static uint64_t TscRate()
{
    static const uint64_t rate = [] {
        LARGE_INTEGER frequency, begin, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        uint64_t tscBegin = __rdtsc();
        do QueryPerformanceCounter(&end); while (end.QuadPart - begin.QuadPart < frequency.QuadPart / 20);
        uint64_t tscEnd = __rdtsc();
        return (tscEnd - tscBegin) * frequency.QuadPart / (end.QuadPart - begin.QuadPart);
    }();
    return rate;
}

GUEST_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = TscRate(); }

// NIER_FIXED_TIME=1 (experiment): the game's QueryPerformanceCounter (sub_825611A0, which reads
// mftb) advances 1/60 s per frame the game submits, plus the real time since that frame, never
// going backwards. A game running below real time then measures 60 fps frame steps.
namespace
{
    std::atomic<uint64_t> gGuestSwaps{ 0 }, gTscAtSwap{ 0 }, gFixedLast{ 0 };
}

void NoteGuestSwap()
{
    gTscAtSwap = __rdtsc();
    gGuestSwaps++;
}

// Fixed-step elapsed time for the kernel time stamp bundle: 1/60 s per submitted frame plus the
// real time since that frame, never going backwards.
uint64_t FixedElapsed100ns()
{
    static std::atomic<uint64_t> last{ 0 };
    const uint64_t swapTsc = gTscAtSwap.load(), now = __rdtsc();
    const uint64_t since = (swapTsc && now > swapTsc) ? (now - swapTsc) * 10000000ull / TscRate() : 0;
    uint64_t value = gGuestSwaps.load() * 166667ull + since;
    uint64_t previous = last.load();
    while (value > previous && !last.compare_exchange_weak(previous, value)) {}
    return value > previous ? value : previous;
}

extern "C" PPC_FUNC(__imp__sub_825611A0);
extern "C" PPC_FUNC(sub_825611A0)
{
    static const bool fixed = GetEnvironmentVariableA("NIER_FIXED_TIME", nullptr, 0) > 0;
    // NIER_LOG_CLOCK=1: logs each distinct caller of the game's QueryPerformanceCounter, once.
    static const bool logCallers = GetEnvironmentVariableA("NIER_LOG_CLOCK", nullptr, 0) > 0;
    if (logCallers)
    {
        static std::mutex mutex;
        static std::set<uint32_t> seen;
        std::lock_guard lock(mutex);
        if (seen.insert(uint32_t(ctx.lr)).second) Log("[clock] QueryPerformanceCounter from %08X:%s\n", uint32_t(ctx.lr), GuestCallChain(8).c_str());
    }
    if (!fixed)
    {
        __imp__sub_825611A0(ctx, base);
        return;
    }
    static const uint64_t start = [] { Log("[time] fixed 60 fps time steps for QueryPerformanceCounter\n"); return __rdtsc(); }();
    const uint64_t swapTsc = gTscAtSwap.load(), now = __rdtsc();
    const uint64_t since = (swapTsc && now > swapTsc) ? now - swapTsc : 0;
    uint64_t value = start + gGuestSwaps.load() * (TscRate() / 60) + since;
    uint64_t last = gFixedLast.load();
    while (value > last && !gFixedLast.compare_exchange_weak(last, value)) {}
    if (value < last) value = last;
    W64(ctx.r3.u32, value);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(DbgPrint)
{
    Log("[guest] %s\n", GuestStr(ctx.r3.u32));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KeBugCheck) { FatalGuest("KeBugCheck", ctx); }
GUEST_IMPORT(KeBugCheckEx) { FatalGuest("KeBugCheckEx", ctx); }

GUEST_IMPORT(RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32, key = ctx.r4.u32;
    uint32_t count = R32(header + 0x14);
    ctx.r3.u64 = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t entry = header + 0x18 + i * 8;
        if (R32(entry) != key) continue;
        // Keys ending in 0x00/0x01 store the value inline; the others store a header offset.
        uint32_t low = key & 0xFF;
        ctx.r3.u64 = (low == 0 || low == 1) ? entry + 4 : header + R32(entry + 4);
        return;
    }
}

GUEST_IMPORT(XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }
