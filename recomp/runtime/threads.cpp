// Guest threads, dispatcher objects, waits and locks.
//
// Waitable state lives in guest memory so code that inspects dispatcher headers directly stays
// correct. All waits share one mutex and condition variable: simple and correct, and worth
// replacing only if profiling shows contention.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>
#include "guest.h"

namespace
{
    constexpr uint32_t PCR_SIZE = 0x1000, TLS_SIZE = 0x400, KTHREAD_SIZE = 0xB00;
    constexpr uint32_t MAX_TLS_SLOTS = TLS_SIZE / 4;

    enum : uint8_t { OBJ_NOTIFICATION = 0, OBJ_SYNC = 1, OBJ_MUTANT = 2, OBJ_SEMAPHORE = 5, OBJ_THREAD = 6 };

    thread_local PPCContext* tContext = nullptr;
    thread_local uint32_t tKThread = 0;
    thread_local uint32_t tPcr = 0;

    std::mutex gDispatch;
    std::condition_variable gDispatchChanged;

    std::mutex gHandles;
    std::map<uint32_t, HandleEntry> gHandleTable;
    uint32_t gNextHandle = 0xF8000000;

    std::atomic<uint32_t> gTlsNext{ 0 };
    std::atomic<uint32_t> gNextCpu{ 0 };
    std::atomic<uint32_t> gNextThreadId{ 1 };

    struct GuestThreadStart
    {
        uint32_t stackSize, xapiStartup, start, param, kthread, pcr;
        bool hasXapi;
    };

    // Lays out PCR, TLS, KTHREAD and stack in one guest block. Offsets are the ones game and CRT read through r13.
    uint32_t CreateThreadBlock(uint32_t stackSize, uint32_t& kthread, uint32_t& stackTop)
    {
        stackSize = (stackSize + 0xFFF) & ~0xFFFu;
        if (stackSize < 0x40000) stackSize = 0x40000;
        uint32_t block = GuestAlloc(PCR_SIZE + TLS_SIZE + KTHREAD_SIZE + stackSize, 0x1000, Region::Virtual);
        uint32_t tls = block + PCR_SIZE;
        kthread = tls + TLS_SIZE;
        uint32_t stackBottom = kthread + KTHREAD_SIZE;
        stackTop = stackBottom + stackSize;

        W32(block + 0x00, tls);
        W32(block + 0x70, stackTop);
        W32(block + 0x74, stackBottom);
        W32(block + 0x100, kthread);
        W8(block + 0x10C, uint8_t(gNextCpu.fetch_add(1) % 6));

        W8(kthread, OBJ_THREAD);
        W32(kthread + 0x14C, gNextThreadId.fetch_add(1));
        return block;
    }

    void EnterGuest(PPCContext& ctx, uint32_t pcr, uint32_t kthread, uint32_t stackTop)
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.r1.u64 = stackTop - 0x100;
        ctx.r13.u64 = pcr;
        ctx.fpscr.loadFromHost();
        tContext = &ctx;
        tKThread = kthread;
        tPcr = pcr;
        RegisterGuestThread(&ctx);
    }

    void CallGuest(PPCContext& ctx, uint32_t address)
    {
        PPCFunc* function = PPC_LOOKUP_FUNC(g_base, address);
        if (!function) FatalGuest("call to an address with no recompiled function", ctx);
        function(ctx, g_base);
    }

    void MarkThreadFinished(uint32_t kthread)
    {
        {
            std::lock_guard lock(gDispatch);
            W32(kthread + 4, 1);
        }
        gDispatchChanged.notify_all();
    }

    DWORD WINAPI HostThreadMain(void* param)
    {
        GuestThreadStart start = *static_cast<GuestThreadStart*>(param);
        delete static_cast<GuestThreadStart*>(param);
        static_assert(sizeof(PPCContext) < 0x10000);
        PPCContext ctx;
        EnterGuest(ctx, start.pcr, start.kthread, R32(start.pcr + 0x70));
        if (start.hasXapi)
        {
            ctx.r3.u64 = start.start;
            ctx.r4.u64 = start.param;
            CallGuest(ctx, start.xapiStartup);
        }
        else
        {
            ctx.r3.u64 = start.param;
            CallGuest(ctx, start.start);
        }
        MarkThreadFinished(start.kthread);
        return 0;
    }

    uint8_t ObjectType(uint32_t object) { return R8(object) & 0x7F; }

    bool IsSignaled(uint32_t object)
    {
        return int32_t(R32(object + 4)) > 0 || (ObjectType(object) <= OBJ_SYNC && R32(object + 4) != 0);
    }

    void Consume(uint32_t object)
    {
        switch (ObjectType(object))
        {
        case OBJ_SYNC: W32(object + 4, 0); break;
        case OBJ_SEMAPHORE: W32(object + 4, R32(object + 4) - 1); break;
        default: break;
        }
    }

    // Timeouts are 100ns units: negative is relative, positive is absolute, a null pointer is forever.
    bool DecodeTimeout(uint32_t pointer, std::chrono::steady_clock::time_point& deadline)
    {
        if (!pointer) return false;
        int64_t value = int64_t(R64(pointer));
        int64_t relative = value;
        if (value > 0)
        {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            relative = int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - value;
        }
        deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(-relative / 10);
        return true;
    }

    uint32_t WaitObjects(const std::vector<uint32_t>& objects, bool waitAll, uint32_t timeoutPointer)
    {
        std::chrono::steady_clock::time_point deadline;
        bool hasDeadline = DecodeTimeout(timeoutPointer, deadline);

        std::unique_lock lock(gDispatch);
        while (true)
        {
            if (waitAll)
            {
                bool ready = true;
                for (uint32_t object : objects) ready = ready && IsSignaled(object);
                if (ready)
                {
                    for (uint32_t object : objects) Consume(object);
                    return X_STATUS_SUCCESS;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (!IsSignaled(objects[i])) continue;
                    Consume(objects[i]);
                    return X_STATUS_SUCCESS + uint32_t(i);
                }
            }
            if (hasDeadline)
            {
                if (gDispatchChanged.wait_until(lock, deadline) == std::cv_status::timeout) return X_STATUS_TIMEOUT;
            }
            else
            {
                gDispatchChanged.wait(lock);
            }
        }
    }

    uint32_t ObjectFromHandle(uint32_t handle)
    {
        if (handle == 0xFFFFFFFE) return tKThread;
        HandleEntry entry;
        return HandleGet(handle, entry) ? entry.object : 0;
    }

    uint32_t AllocDispatcher(uint8_t type, int32_t signal, uint32_t size)
    {
        uint32_t object = GuestAlloc(size, 0x10, Region::Heap);
        W8(object, type);
        W32(object + 4, uint32_t(signal));
        W32(object + 8, object + 8);
        W32(object + 12, object + 8);
        return object;
    }

    void SpinAcquire(uint32_t address, uint32_t value)
    {
        auto* lock = (volatile LONG*)(g_base + address);
        for (uint32_t spins = 0; InterlockedCompareExchange(lock, __builtin_bswap32(value), 0) != 0; spins++)
        {
            if (spins > 64) SwitchToThread();
            else YieldProcessor();
        }
    }
}

PPCContext* CurrentContext() { return tContext; }
uint32_t CurrentKThread() { return tKThread; }

uint32_t HandleCreate(const HandleEntry& entry)
{
    std::lock_guard lock(gHandles);
    uint32_t handle = gNextHandle;
    gNextHandle += 4;
    gHandleTable[handle] = entry;
    return handle;
}

bool HandleGet(uint32_t handle, HandleEntry& out)
{
    std::lock_guard lock(gHandles);
    auto it = gHandleTable.find(handle);
    if (it == gHandleTable.end()) return false;
    out = it->second;
    return true;
}

bool HandleClose(uint32_t handle)
{
    std::lock_guard lock(gHandles);
    auto it = gHandleTable.find(handle);
    if (it == gHandleTable.end()) return false;
    if (it->second.kind == HandleKind::File && it->second.host) CloseHandle(it->second.host);
    gHandleTable.erase(it);
    return true;
}

void SetEventObject(uint32_t object)
{
    {
        std::lock_guard lock(gDispatch);
        W32(object + 4, 1);
    }
    gDispatchChanged.notify_all();
}

void ThreadsInit() {}

uint32_t AttachGuestThread(PPCContext& ctx)
{
    uint32_t kthread, stackTop;
    uint32_t pcr = CreateThreadBlock(0x40000, kthread, stackTop);
    EnterGuest(ctx, pcr, kthread, stackTop);
    return pcr;
}

void CallGuestFunction(PPCContext& ctx, uint32_t address) { CallGuest(ctx, address); }

void RunMainThread(uint32_t entry)
{
    uint32_t kthread, stackTop;
    uint32_t pcr = CreateThreadBlock(0x100000, kthread, stackTop);
    static PPCContext ctx;
    EnterGuest(ctx, pcr, kthread, stackTop);
    CallGuest(ctx, entry);
    Log("guest entry point returned, r3=0x%08X\n", ctx.r3.u32);
}

GUEST_IMPORT(ExCreateThread)
{
    uint32_t handlePtr = ctx.r3.u32, stackSize = ctx.r4.u32, idPtr = ctx.r5.u32;
    uint32_t xapi = ctx.r6.u32, start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;

    uint32_t kthread, stackTop;
    uint32_t pcr = CreateThreadBlock(stackSize, kthread, stackTop);
    auto* info = new GuestThreadStart{ stackSize, xapi, start, param, kthread, pcr, xapi != 0 };
    bool suspended = flags & 1;
    HANDLE host = CreateThread(nullptr, 64 * 1024 * 1024, HostThreadMain, info, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!host) { ctx.r3.u64 = X_STATUS_NO_MEMORY; return; }

    HandleEntry entry{ HandleKind::Thread, kthread, host, {} };
    uint32_t handle = HandleCreate(entry);
    if (handlePtr) W32(handlePtr, handle);
    if (idPtr) W32(idPtr, R32(kthread + 0x14C));
    Log("thread created: handle 0x%08X start 0x%08X xapi 0x%08X param 0x%08X%s\n", handle, start, xapi, param, suspended ? " (suspended)" : "");
    if (!suspended) ResumeThread(host);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(ExTerminateThread)
{
    Log("thread exiting, code 0x%08X\n", ctx.r3.u32);
    MarkThreadFinished(tKThread);
    ExitThread(ctx.r3.u32);
}

GUEST_IMPORT(NtResumeThread)
{
    HandleEntry entry;
    if (!HandleGet(ctx.r3.u32, entry) || entry.kind != HandleKind::Thread) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    DWORD previous = ResumeThread(entry.host);
    if (ctx.r4.u32) W32(ctx.r4.u32, previous);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtSuspendThread)
{
    HandleEntry entry;
    if (!HandleGet(ctx.r3.u32, entry) || entry.kind != HandleKind::Thread) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    DWORD previous = SuspendThread(entry.host);
    if (ctx.r4.u32) W32(ctx.r4.u32, previous);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(KeSetAffinityThread) { ctx.r3.u64 = 0x3F; }
GUEST_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }

GUEST_IMPORT(KeDelayExecutionThread)
{
    int64_t value = int64_t(R64(ctx.r5.u32));
    if (value < 0) Sleep(DWORD(-value / 10000));
    else SwitchToThread();
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(KeWaitForSingleObject)
{
    ctx.r3.u64 = WaitObjects({ ctx.r3.u32 }, false, ctx.r7.u32);
}

GUEST_IMPORT(KeWaitForMultipleObjects)
{
    std::vector<uint32_t> objects(ctx.r3.u32);
    for (uint32_t i = 0; i < ctx.r3.u32; i++) objects[i] = R32(ctx.r4.u32 + i * 4);
    ctx.r3.u64 = WaitObjects(objects, ctx.r5.u32 == 0, ctx.r9.u32);
}

GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    uint32_t object = ObjectFromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = WaitObjects({ object }, false, ctx.r6.u32);
}

GUEST_IMPORT(KeSetEvent)
{
    uint32_t object = ctx.r3.u32, previous;
    {
        std::lock_guard lock(gDispatch);
        previous = R32(object + 4);
        W32(object + 4, 1);
    }
    gDispatchChanged.notify_all();
    ctx.r3.u64 = previous;
}

GUEST_IMPORT(KeResetEvent)
{
    uint32_t object = ctx.r3.u32, previous;
    {
        std::lock_guard lock(gDispatch);
        previous = R32(object + 4);
        W32(object + 4, 0);
    }
    ctx.r3.u64 = previous;
}

GUEST_IMPORT(NtCreateEvent)
{
    uint32_t object = AllocDispatcher(ctx.r5.u32 ? OBJ_SYNC : OBJ_NOTIFICATION, ctx.r6.u32 ? 1 : 0, 0x20);
    W32(ctx.r3.u32, HandleCreate({ HandleKind::Event, object }));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtSetEvent)
{
    uint32_t object = ObjectFromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t previous;
    {
        std::lock_guard lock(gDispatch);
        previous = R32(object + 4);
        W32(object + 4, 1);
    }
    gDispatchChanged.notify_all();
    if (ctx.r4.u32) W32(ctx.r4.u32, previous);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtClearEvent)
{
    uint32_t object = ObjectFromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    std::lock_guard lock(gDispatch);
    W32(object + 4, 0);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtCreateSemaphore)
{
    uint32_t object = AllocDispatcher(OBJ_SEMAPHORE, int32_t(ctx.r5.u32), 0x20);
    W32(object + 0x10, ctx.r6.u32);
    W32(ctx.r3.u32, HandleCreate({ HandleKind::Semaphore, object }));
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtReleaseSemaphore)
{
    uint32_t object = ObjectFromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    uint32_t previous;
    {
        std::lock_guard lock(gDispatch);
        previous = R32(object + 4);
        W32(object + 4, previous + ctx.r4.u32);
    }
    gDispatchChanged.notify_all();
    if (ctx.r5.u32) W32(ctx.r5.u32, previous);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(NtClose)
{
    ctx.r3.u64 = HandleClose(ctx.r3.u32) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

GUEST_IMPORT(ObReferenceObjectByHandle)
{
    uint32_t object = ObjectFromHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = X_STATUS_INVALID_HANDLE; return; }
    W32(ctx.r5.u32, object);
    ctx.r3.u64 = X_STATUS_SUCCESS;
}

GUEST_IMPORT(ObReferenceObject) { ctx.r3.u64 = X_STATUS_SUCCESS; }
GUEST_IMPORT(ObDereferenceObject) { ctx.r3.u64 = X_STATUS_SUCCESS; }

// RTL critical sections: header at +0, lock count +0x10, recursion +0x14, owner +0x18.
static void InitCriticalSection(uint32_t cs)
{
    W8(cs, 1);
    W32(cs + 4, 0);
    W32(cs + 8, cs + 8);
    W32(cs + 12, cs + 8);
    W32(cs + 0x10, 0xFFFFFFFF);
    W32(cs + 0x14, 0);
    W32(cs + 0x18, 0);
}

GUEST_IMPORT(RtlInitializeCriticalSection) { InitCriticalSection(ctx.r3.u32); ctx.r3.u64 = 0; }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { InitCriticalSection(ctx.r3.u32); ctx.r3.u64 = 0; }

static bool TryEnter(uint32_t cs, uint32_t self)
{
    auto* owner = (volatile LONG*)(g_base + cs + 0x18);
    LONG swapped = __builtin_bswap32(self);
    LONG previous = InterlockedCompareExchange(owner, swapped, 0);
    if (previous == 0)
    {
        W32(cs + 0x14, 1);
        W32(cs + 0x10, 0);
        return true;
    }
    if (previous == swapped)
    {
        W32(cs + 0x14, R32(cs + 0x14) + 1);
        W32(cs + 0x10, R32(cs + 0x10) + 1);
        return true;
    }
    return false;
}

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32, self = tKThread;
    for (uint32_t spins = 0; !TryEnter(cs, self); spins++)
    {
        if (spins < 256) YieldProcessor();
        else if (spins < 2048) SwitchToThread();
        else Sleep(1);
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = TryEnter(ctx.r3.u32, tKThread) ? 1 : 0;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t recursion = R32(cs + 0x14) - 1;
    W32(cs + 0x14, recursion);
    W32(cs + 0x10, R32(cs + 0x10) - 1);
    if (recursion == 0)
    {
        MemoryBarrier();
        InterlockedExchange((volatile LONG*)(g_base + cs + 0x18), 0);
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ctx.r3.u32, tKThread); ctx.r3.u64 = 0; }
GUEST_IMPORT(KfReleaseSpinLock) { InterlockedExchange((volatile LONG*)(g_base + ctx.r3.u32), 0); ctx.r3.u64 = 0; }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ctx.r3.u32, tKThread); ctx.r3.u64 = 0; }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { InterlockedExchange((volatile LONG*)(g_base + ctx.r3.u32), 0); ctx.r3.u64 = 0; }
GUEST_IMPORT(KeEnterCriticalRegion) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeLeaveCriticalRegion) { ctx.r3.u64 = 0; }

GUEST_IMPORT(KeTlsAlloc)
{
    uint32_t slot = gTlsNext.fetch_add(1);
    ctx.r3.u64 = slot < MAX_TLS_SLOTS ? slot : 0xFFFFFFFF;
}

GUEST_IMPORT(KeTlsFree) { ctx.r3.u64 = 1; }

GUEST_IMPORT(KeTlsGetValue)
{
    ctx.r3.u64 = ctx.r3.u32 < MAX_TLS_SLOTS ? R32(R32(tPcr) + ctx.r3.u32 * 4) : 0;
}

GUEST_IMPORT(KeTlsSetValue)
{
    if (ctx.r3.u32 >= MAX_TLS_SLOTS) { ctx.r3.u64 = 0; return; }
    W32(R32(tPcr) + ctx.r3.u32 * 4, ctx.r4.u32);
    ctx.r3.u64 = 1;
}
