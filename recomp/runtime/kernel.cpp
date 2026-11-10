// Small kernel and runtime-library imports.
#include <windows.h>
#include <cstring>
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
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    W64(ctx.r3.u32, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 0;
}

// The recompiler implements mftb with the host TSC, so the guest must see the TSC's rate.
GUEST_IMPORT(KeQueryPerformanceFrequency)
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
    ctx.r3.u64 = rate;
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
