// Guest address space and allocators, plus the imports that expose them.
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include "guest.h"

uint8_t* g_base = nullptr;

namespace
{
    constexpr uint32_t PAGE = 0x1000;

    struct RangeAllocator
    {
        std::map<uint32_t, uint32_t> freeRanges;  // address -> size
        std::map<uint32_t, uint32_t> live;        // address -> size
        uint32_t granule = 0x10;

        uint32_t Alloc(uint32_t size, uint32_t align)
        {
            size = (size + granule - 1) & ~(granule - 1);
            align = align < granule ? granule : align;
            for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it)
            {
                uint32_t begin = (it->first + align - 1) & ~(align - 1);
                if (begin + size > it->first + it->second || begin < it->first) continue;
                uint32_t rangeStart = it->first, rangeSize = it->second;
                freeRanges.erase(it);
                if (begin > rangeStart) freeRanges[rangeStart] = begin - rangeStart;
                if (begin + size < rangeStart + rangeSize) freeRanges[begin + size] = rangeStart + rangeSize - (begin + size);
                live[begin] = size;
                return begin;
            }
            return 0;
        }

        bool Free(uint32_t address)
        {
            auto it = live.find(address);
            if (it == live.end()) return false;
            uint32_t begin = it->first, size = it->second;
            live.erase(it);
            auto next = freeRanges.lower_bound(begin);
            if (next != freeRanges.end() && next->first == begin + size)
            {
                size += next->second;
                next = freeRanges.erase(next);
            }
            if (next != freeRanges.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == begin)
                {
                    prev->second += size;
                    return true;
                }
            }
            freeRanges[begin] = size;
            return true;
        }
    };

    std::mutex gMutex;
    RangeAllocator gHeap, gVirtual, gPhysical;

    RangeAllocator& Allocator(Region region)
    {
        return region == Region::Heap ? gHeap : region == Region::Virtual ? gVirtual : gPhysical;
    }

    uint32_t RoundPage(uint32_t size, uint32_t page) { return (size + page - 1) & ~(page - 1); }
}

void MemoryInit()
{
    g_base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_base) { Log("cannot commit 4GB of guest memory (error %lu)\n", GetLastError()); ExitProcess(1); }
    DWORD old;
    VirtualProtect(g_base, 0x10000, PAGE_NOACCESS, &old);  // catch guest null pointer use

    gHeap.granule = 0x10;
    gHeap.freeRanges[0x20000000] = 0x20000000;
    gVirtual.granule = PAGE;
    gVirtual.freeRanges[0x40000000] = 0x3F000000;
    gPhysical.granule = PAGE;
    // Physical allocations start above the addresses the executable image masks down to, so a
    // physical address always identifies one guest location.
    gPhysical.freeRanges[0xA6100000] = 0x19E00000;
    Log("guest memory at %p\n", g_base);
}

uint32_t PhysicalToGuest(uint32_t physical)
{
    physical &= 0x1FFFFFFF;
    return (physical >= 0x02000000 && physical < 0x06100000) ? (0x80000000 | physical) : (0xA0000000 | physical);
}

uint32_t GuestAlloc(uint32_t size, uint32_t align, Region region)
{
    uint32_t address;
    {
        std::lock_guard lock(gMutex);
        address = Allocator(region).Alloc(size, align);
    }
    if (address) memset(g_base + address, 0, size);
    else Log("guest allocation failed: size 0x%X region %d\n", size, (int)region);
    return address;
}

void GuestFree(uint32_t address)
{
    std::lock_guard lock(gMutex);
    if (!gHeap.Free(address) && !gVirtual.Free(address) && !gPhysical.Free(address))
        Log("guest free of unknown address 0x%08X\n", address);
}

GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    uint32_t requested = R32(basePtr), size = R32(sizePtr);
    uint32_t page = (type & 0x20000000) ? 0x10000 : PAGE;
    size = RoundPage(size, page);
    if (requested != 0)
    {
        // Committing inside an earlier reservation: the whole space is already committed.
        W32(sizePtr, size);
        ctx.r3.u64 = X_STATUS_SUCCESS;
        return;
    }
    uint32_t address = GuestAlloc(size, page, Region::Virtual);
    if (!address) { ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }
    W32(basePtr, address);
    W32(sizePtr, size);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtFreeVirtualMemory)
{
    uint32_t basePtr = ctx.r3.u32, type = ctx.r5.u32;
    if (type & 0x8000) GuestFree(R32(basePtr));  // MEM_RELEASE; decommit needs no work
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, align = ctx.r8.u32;
    ctx.r3.u64 = GuestAlloc(size, align ? align : PAGE, Region::Physical);
}

GUEST_IMPORT(MmFreePhysicalMemory)
{
    GuestFree(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(MmGetPhysicalAddress)
{
    uint32_t address = ctx.r3.u32;
    static std::atomic<int> warned{ 0 };
    if (address < 0x80000000 && warned.fetch_add(1) < 8)
        Log("[memory] physical address requested for non-physical guest address 0x%08X (lr %08llX); the GPU would not see this memory\n", address, ctx.lr);
    ctx.r3.u64 = address & 0x1FFFFFFF;
}
GUEST_IMPORT(MmQueryAddressProtect) { ctx.r3.u64 = PAGE_READWRITE; }

GUEST_IMPORT(ExAllocatePoolTypeWithTag)
{
    ctx.r3.u64 = GuestAlloc(ctx.r3.u32, 0x10, Region::Heap);
}

GUEST_IMPORT(ExFreePool)
{
    GuestFree(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamAlloc)
{
    uint32_t address = GuestAlloc(ctx.r4.u32, 0x10, Region::Heap);
    W32(ctx.r5.u32, address);
    ctx.r3.u64 = address ? 0 : 0x8007000E;
}
