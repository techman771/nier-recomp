// Process startup, logging and crash reporting.
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"

namespace
{
    std::mutex gLogMutex;
    bool gTraceImports = false;
    constexpr int kRing = 24;
    thread_local const char* tRecent[kRing];
    thread_local int tRecentNext = 0;

    std::vector<std::pair<uintptr_t, uint32_t>> gHostToGuest;
    std::mutex gStubMutex;
    std::map<std::string, int> gStubCounts;

    uint32_t GuestFunctionFor(uintptr_t host)
    {
        // Runtime code links after the recompiled code, so an address past the last function isn't guest code.
        if (gHostToGuest.empty() || host > gHostToGuest.back().first + 0x10000) return 0;
        auto it = std::upper_bound(gHostToGuest.begin(), gHostToGuest.end(), std::make_pair(host, uint32_t(0xFFFFFFFF)));
        return it == gHostToGuest.begin() ? 0 : std::prev(it)->second;
    }

    void BuildFunctionIndex()
    {
        for (auto* m = PPCFuncMappings; m->host; ++m) gHostToGuest.emplace_back(reinterpret_cast<uintptr_t>(m->host), m->guest);
        std::sort(gHostToGuest.begin(), gHostToGuest.end());
    }

    void PrintGuestState(PPCContext* ctx)
    {
        if (ctx)
        {
            Log("  r1=%08X r3=%08X r4=%08X r5=%08X r11=%08X r13=%08X lr=%08llX ctr=%08X\n", ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r11.u32, ctx->r13.u32, ctx->lr, ctx->ctr.u32);
        }
        Log("  recent imports on this thread:");
        for (int i = 0; i < kRing; i++)
        {
            const char* name = tRecent[(tRecentNext + i) % kRing];
            if (name) Log(" %s", name);
        }
        Log("\n");
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        Log("  guest call stack (innermost first):\n");
        uint32_t last = 0;
        int shown = 0;
        for (USHORT i = 0; i < count && shown < 24; i++)
        {
            uint32_t guest = GuestFunctionFor(reinterpret_cast<uintptr_t>(frames[i]));
            if (guest && guest != last) { Log("    sub_%08X\n", guest); shown++; }
            last = guest;
        }
    }

    // Names every frame, runtime and recompiled alike, from nier.pdb.
    void PrintHostStack(CONTEXT context)
    {
        HANDLE process = GetCurrentProcess();
        Log("  host call stack (innermost first):\n");
        for (int i = 0; i < 24 && context.Rip; i++)
        {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymFromAddr(process, context.Rip, &displacement, symbol))
            {
                if (SymGetLineFromAddr64(process, context.Rip, &lineDisplacement, &line))
                    Log("    %s+0x%llX  %s:%lu\n", symbol->Name, displacement, line.FileName, line.LineNumber);
                else
                    Log("    %s+0x%llX\n", symbol->Name, displacement);
            }
            else Log("    %p\n", (void*)context.Rip);

            DWORD64 imageBase = 0;
            auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!entry) break;
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData, &establisher, nullptr);
        }
    }

    LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
    {
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_BREAKPOINT)
            return EXCEPTION_CONTINUE_SEARCH;

        uintptr_t faultAddress = info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
        bool isGuest = faultAddress >= uintptr_t(g_base) && faultAddress < uintptr_t(g_base) + 0x100000000ull;
        Log("\n*** CRASH code %08lX in guest function sub_%08X\n", code, GuestFunctionFor(info->ContextRecord->Rip));
        if (isGuest) Log("  guest address 0x%08llX (%s)\n", (unsigned long long)(faultAddress - uintptr_t(g_base)), info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        else Log("  host address %p\n", (void*)faultAddress);
        PrintHostStack(*info->ContextRecord);
        PrintGuestState(CurrentContext());
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace { FILE* gCaptureLog = nullptr; }

void LogSetCaptureFile(FILE* file)
{
    std::lock_guard lock(gLogMutex);
    if (gCaptureLog) fclose(gCaptureLog);
    gCaptureLog = file;
}

void Log(const char* format, ...)
{
    std::lock_guard lock(gLogMutex);
    va_list arguments;
    va_start(arguments, format);
    if (gCaptureLog)
    {
        va_list copy;
        va_copy(copy, arguments);
        vfprintf(gCaptureLog, format, copy);
        va_end(copy);
    }
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);
}

void TraceImport(const char* name, PPCContext& ctx)
{
    tRecent[tRecentNext] = name;
    tRecentNext = (tRecentNext + 1) % kRing;
    if (gTraceImports) Log("[import] %s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08llX\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.lr);
}

void StubHit(const char* name, PPCContext& ctx)
{
    TraceImport(name, ctx);
    int count;
    {
        std::lock_guard lock(gStubMutex);
        count = ++gStubCounts[name];
    }
    if (count <= 3) Log("[stub] %s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08llX\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.lr);
}

void FatalGuest(const char* reason, PPCContext& ctx)
{
    Log("\n*** FATAL: %s\n", reason);
    PrintGuestState(&ctx);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);
    __builtin_unreachable();
}

void InstallCrashHandler() { AddVectoredExceptionHandler(1, CrashHandler); }

// NIER_WATCH_WRITE=<guest address>,<bytes>: logs which guest functions write that range (debugging).
// The page is write-protected in every window that maps it; a write faults, is logged, and is
// replayed by single-stepping with the page writable, after which the protection is restored.
namespace
{
    uint32_t gWatchStart = 0, gWatchBytes = 0;
    std::vector<uint8_t*> gWatchPages;   // host addresses of the watched page in each window
    thread_local bool tWatchStepping = false;
    std::map<std::string, int> gWatchSeen;

    void ProtectWatch(bool on)
    {
        DWORD old;
        for (uint8_t* page : gWatchPages) VirtualProtect(page, 4096, on ? PAGE_READONLY : PAGE_READWRITE, &old);
    }

    LONG WINAPI WatchHandler(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP && tWatchStepping)
        {
            tWatchStepping = false;
            ProtectWatch(true);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2 || info->ExceptionRecord->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;
        const uint8_t* fault = reinterpret_cast<const uint8_t*>(info->ExceptionRecord->ExceptionInformation[1]);
        bool ours = false;
        for (uint8_t* page : gWatchPages) ours = ours || (fault >= page && fault < page + 4096);
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;

        const uint32_t guest = uint32_t(fault - g_base);
        const uint32_t offset = guest & 0x1FFFFFFF, start = gWatchStart & 0x1FFFFFFF;
        static const uint64_t fromFrame = [] {
            char text[32];
            return GetEnvironmentVariableA("NIER_WATCH_FROM", text, sizeof(text)) ? strtoull(text, nullptr, 10) : 0ull;
        }();
        if (offset + 16 > start && offset < start + gWatchBytes && GpuFrameCount() >= fromFrame)
        {
            // Guest call chain from the faulting context.
            CONTEXT context = *info->ContextRecord;
            std::string chain;
            uint32_t last = 0;
            for (int i = 0, shown = 0; i < 32 && context.Rip && shown < 6; i++)
            {
                const uint32_t function = GuestFunctionFor(context.Rip);
                if (function && function != last)
                {
                    char text[16];
                    snprintf(text, sizeof(text), " %08X", function);
                    chain += text;
                    shown++;
                }
                last = function;
                DWORD64 imageBase = 0;
                auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!entry) break;
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData, &establisher, nullptr);
            }
            if (gWatchSeen[chain]++ < 3)
            {
                Log("[watch] write to %08X (+%u) by%s (frame %llu)\n", guest, offset - start, chain.c_str(), GpuFrameCount());
                if (PPCContext* ctx = CurrentContext())
                    Log("[watch]   r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X r11=%08X r31=%08X lr=%08llX\n",
                        ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32, ctx->r10.u32, ctx->r11.u32, ctx->r31.u32, ctx->lr);
            }
        }
        ProtectWatch(false);
        tWatchStepping = true;
        info->ContextRecord->EFlags |= 0x100;   // trap after the write, then protect again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

void WatchInit()
{
    char value[64];
    if (!GetEnvironmentVariableA("NIER_WATCH_WRITE", value, sizeof(value))) return;
    gWatchStart = strtoul(value, nullptr, 16);
    const char* comma = strchr(value, ',');
    gWatchBytes = comma ? strtoul(comma + 1, nullptr, 0) : 16;
    const uint32_t page = (gWatchStart & 0x1FFFFFFF) & ~0xFFFu;
    if ((gWatchStart >> 29) >= 5)   // physical: watch it in each window
        for (uint32_t window : { 0xA0000000u, 0xC0000000u }) gWatchPages.push_back(g_base + window + page);   // they share storage
    else gWatchPages.push_back(g_base + (gWatchStart & ~0xFFFu));
    AddVectoredExceptionHandler(1, WatchHandler);
    ProtectWatch(true);
    Log("[watch] watching writes to %08X, %u bytes\n", gWatchStart, gWatchBytes);
}

namespace
{
    struct ThreadRecord
    {
        PPCContext* ctx;
        const char** ring;
        int* next;
        HANDLE thread;
    };
    std::mutex gThreadsMutex;
    std::vector<ThreadRecord> gThreads;

    // Captures while the thread is suspended and logs afterwards: a suspended thread may hold
    // the log or heap lock, so nothing here may take a lock or allocate until it has resumed.
    void DumpThread(int index, const ThreadRecord& record)
    {
        constexpr int kMaxFrames = 64;
        uintptr_t frames[kMaxFrames];
        int count = 0;
        uint32_t r1 = 0;
        uint64_t lr = 0;

        if (SuspendThread(record.thread) == DWORD(-1)) return;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(record.thread, &context))
        {
            r1 = record.ctx->r1.u32;
            lr = record.ctx->lr;
            while (count < kMaxFrames && context.Rip)
            {
                frames[count++] = context.Rip;
                DWORD64 imageBase = 0;
                auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!entry) break;
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData, &establisher, nullptr);
            }
        }
        ResumeThread(record.thread);

        Log("thread %d: r1=%08X lr=%08llX last imports:", index, r1, lr);
        for (int i = 3; i >= 1; i--)
        {
            const char* name = record.ring[(*record.next - i + kRing) % kRing];
            Log(" %s", name ? name : "-");
        }
        Log("\n   stack:");
        uint32_t last = 0;
        int shown = 0;
        for (int i = 0; i < count && shown < 12; i++)
        {
            uint32_t guest = GuestFunctionFor(frames[i]);
            if (guest && guest != last) { Log(" %08X", guest); shown++; }
            last = guest;
        }
        Log("\n");
    }

    void DumpAllThreads()
    {
        std::vector<ThreadRecord> copy;
        {
            std::lock_guard lock(gThreadsMutex);
            copy = gThreads;
        }
        GpuDumpStats();
        AudioDumpStats();
        PresenterDumpStats();
        Log("\n=== guest thread dump (%zu threads) ===\n", copy.size());
        for (size_t i = 0; i < copy.size(); i++) DumpThread(int(i), copy[i]);
    }
}

void RegisterGuestThread(PPCContext* ctx)
{
    HANDLE thread;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lock(gThreadsMutex);
    gThreads.push_back({ ctx, tRecent, &tRecentNext, thread });
}

int main(int argc, char** argv)
{
    std::string gameDir = argc > 1 ? argv[1] : "..\\..\\Nier";
    char* trace = nullptr;
    size_t length = 0;
    _dupenv_s(&trace, &length, "NIER_TRACE_IMPORTS");
    gTraceImports = trace != nullptr;

    timeBeginPeriod(1);  // the audio and vblank schedules need Sleep(1) to be close to 1ms
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    InstallCrashHandler();
    MemoryInit();
    ThreadsInit();
    PresenterInit();
    BuildFunctionIndex();
    uint32_t entry = LoadGame(gameDir);
    ProfilerInit();
    WatchInit();

    std::thread([] {
        while (true)
        {
            UpdateTimeBundle();
            Sleep(1);
        }
    }).detach();

    char* dumpAfter = nullptr;
    _dupenv_s(&dumpAfter, &length, "NIER_DUMP_AFTER");
    if (dumpAfter)
    {
        int seconds = atoi(dumpAfter);
        std::thread([seconds] {
            Sleep(seconds * 1000);
            DumpAllThreads();
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
    }

    Log("entering guest at 0x%08X\n", entry);
    RunMainThread(entry);
    fflush(stdout);
    ExitProcess(0);
}
