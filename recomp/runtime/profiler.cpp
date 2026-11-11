// Sampling profiler. NIER_PROFILE=<first guest frame>,<seconds>: once the game reaches that frame, every
// thread of the process is sampled about 1000 times a second for the given time, then the log gets each
// busy thread's hot functions (own time and inclusive time) and what its idle samples were waiting in.
// A third field "exit" (NIER_PROFILE=11000,20,exit) ends the process after the report.
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "guest.h"

namespace
{
    constexpr int kMaxFrames = 48;

    struct Sample
    {
        DWORD thread;
        uint8_t count;
        uintptr_t frames[kMaxFrames];
    };

    struct Symbol
    {
        std::string name;
        bool wait = false;      // a system call that blocks (the thread is idle)
        bool system = false;    // ntdll / kernelbase / win32u frame
    };

    std::unordered_map<uintptr_t, Symbol> gSymbols;   // by function start
    std::unordered_map<uintptr_t, uintptr_t> gStarts;  // address -> function start

    const Symbol& Describe(uintptr_t address, uintptr_t& start)
    {
        auto known = gStarts.find(address);
        if (known != gStarts.end())
        {
            start = known->second;
            return gSymbols[start];
        }
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
        auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 255;
        DWORD64 displacement = 0;
        Symbol symbol;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, info))
        {
            start = uintptr_t(info->Address);
            symbol.name = info->Name;
        }
        else
        {
            start = address;
            char text[32];
            snprintf(text, sizeof(text), "%p", (void*)address);
            symbol.name = text;
        }
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(GetCurrentProcess(), address, &module))
        {
            std::string name = module.ModuleName;
            for (char& c : name) c = char(tolower(c));
            symbol.system = name == "ntdll" || name == "kernelbase" || name == "win32u" || name == "kernel32";
            const std::string& n = symbol.name;
            symbol.wait = (name == "ntdll" || name == "win32u") && (n.rfind("Nt", 0) == 0 || n.rfind("Zw", 0) == 0);
            if (symbol.wait && n.find("Wait") == std::string::npos && n.find("Delay") == std::string::npos && n.find("Remove") == std::string::npos && n.find("Signal") == std::string::npos && n.find("Yield") == std::string::npos && n.rfind("NtUser", 0) != 0)
                symbol.wait = false;
        }
        gStarts[address] = start;
        auto& stored = gSymbols[start];
        if (stored.name.empty()) stored = symbol;
        return stored;
    }

    void Report(const std::vector<Sample>& samples, double seconds, uint64_t frames)
    {
        Log("\n=== profile: %.1f s, %llu guest frames (%.1f fps), %zu samples ===\n", seconds, (unsigned long long)frames, frames / seconds, samples.size());
        std::map<DWORD, std::vector<const Sample*>> byThread;
        for (const Sample& sample : samples) byThread[sample.thread].push_back(&sample);

        struct ThreadSummary { DWORD id; size_t total, busy; std::string label; std::vector<const Sample*> list; };
        std::vector<ThreadSummary> threads;
        for (auto& [id, list] : byThread)
        {
            size_t busy = 0;
            for (const Sample* sample : list)
            {
                uintptr_t start;
                if (sample->count && !Describe(sample->frames[0], start).wait) busy++;
            }
            // Label: the outermost non-system function of the first sample.
            std::string label;
            const Sample* first = list.front();
            for (int i = first->count - 1; i >= 0; i--)
            {
                uintptr_t start;
                const Symbol& symbol = Describe(first->frames[i], start);
                if (!symbol.system) { label = symbol.name; break; }
            }
            threads.push_back({ id, list.size(), busy, label, list });
        }
        std::sort(threads.begin(), threads.end(), [](auto& a, auto& b) { return a.busy > b.busy; });

        for (const ThreadSummary& thread : threads)
        {
            const double busyShare = 100.0 * thread.busy / std::max<size_t>(thread.total, 1);
            Log("\nthread %lu (%s): busy %.1f%% of %zu samples\n", thread.id, thread.label.c_str(), busyShare, thread.total);
            if (thread.busy * 20 < thread.total) continue;   // idle threads: just the line above

            std::unordered_map<uintptr_t, size_t> self, inclusive, waits;
            for (const Sample* sample : thread.list)
            {
                if (!sample->count) continue;
                uintptr_t leafStart;
                const Symbol& leaf = Describe(sample->frames[0], leafStart);
                if (leaf.wait)
                {
                    for (int i = 1; i < sample->count; i++)
                    {
                        uintptr_t start;
                        if (!Describe(sample->frames[i], start).system) { waits[start]++; break; }
                    }
                    continue;
                }
                self[leafStart]++;
                std::unordered_set<uintptr_t> seen;
                for (int i = 0; i < sample->count; i++)
                {
                    uintptr_t start;
                    Describe(sample->frames[i], start);
                    if (seen.insert(start).second) inclusive[start]++;
                }
            }
            auto print = [&](const char* title, std::unordered_map<uintptr_t, size_t>& counts, size_t limit) {
                std::vector<std::pair<size_t, uintptr_t>> sorted;
                for (auto& [start, count] : counts) sorted.push_back({ count, start });
                std::sort(sorted.rbegin(), sorted.rend());
                Log("  %s:\n", title);
                for (size_t i = 0; i < std::min(limit, sorted.size()); i++)
                    Log("    %5.1f%%  %s\n", 100.0 * sorted[i].first / thread.total, gSymbols[sorted[i].second].name.c_str());
            };
            print("own time", self, 25);
            print("inclusive", inclusive, 25);

            // Callers of the hottest functions (first caller frame with a different function).
            std::vector<std::pair<size_t, uintptr_t>> hottest;
            for (auto& [start, count] : self) hottest.push_back({ count, start });
            std::sort(hottest.rbegin(), hottest.rend());
            if (hottest.size() > 6) hottest.resize(6);
            Log("  callers of the hottest functions:\n");
            for (auto& [count, function] : hottest)
            {
                std::unordered_map<uintptr_t, size_t> callers;
                for (const Sample* sample : thread.list)
                {
                    uintptr_t start;
                    Describe(sample->frames[0], start);
                    if (start != function) continue;
                    for (int i = 1; i < sample->count; i++)
                    {
                        uintptr_t caller;
                        const Symbol& symbol = Describe(sample->frames[i], caller);
                        if (caller != function && symbol.name.rfind("std::", 0) != 0) { callers[caller]++; break; }
                    }
                }
                std::vector<std::pair<size_t, uintptr_t>> sorted;
                for (auto& [caller, n] : callers) sorted.push_back({ n, caller });
                std::sort(sorted.rbegin(), sorted.rend());
                Log("    %s:", gSymbols[function].name.c_str());
                for (size_t i = 0; i < std::min<size_t>(3, sorted.size()); i++)
                    Log(" %s %.1f%%", gSymbols[sorted[i].second].name.c_str(), 100.0 * sorted[i].first / thread.total);
                Log("\n");
            }
            if (!waits.empty()) print("idle, waiting in", waits, 6);
        }
        Log("=== end of profile ===\n");
    }

    void ProfilerThread(uint64_t firstFrame, double seconds, bool exitAfter)
    {
        while (GpuFrameCount() < firstFrame) Sleep(50);
        Log("[profile] sampling from frame %llu for %.0f s\n", (unsigned long long)GpuFrameCount(), seconds);
        GpuResetFrameTiming();

        const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
        std::unordered_map<DWORD, HANDLE> handles;
        std::vector<Sample> samples;
        samples.reserve(size_t(seconds * 1000 * 24));
        LARGE_INTEGER frequency, begin, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        const uint64_t startFrame = GpuFrameCount();
        int tick = 0;
        do
        {
            if (tick++ % 200 == 0)
            {
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self && !handles.count(entry.th32ThreadID))
                        if (HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID))
                            handles[entry.th32ThreadID] = handle;
                CloseHandle(snapshot);
            }
            for (auto& [id, handle] : handles)
            {
                Sample sample{ id, 0, {} };
                if (SuspendThread(handle) == DWORD(-1)) continue;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(handle, &context))
                {
                    while (sample.count < kMaxFrames && context.Rip)
                    {
                        sample.frames[sample.count++] = context.Rip;
                        DWORD64 imageBase = 0;
                        auto* function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                        if (!function)
                        {
                            // Leaf function without unwind data (innermost frame only): return address
                            // is at the stack pointer.
                            if (sample.count > 1) break;
                            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                            context.Rsp += 8;
                            continue;
                        }
                        void* handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                    }
                }
                ResumeThread(handle);
                if (sample.count) samples.push_back(sample);
            }
            Sleep(1);
            QueryPerformanceCounter(&now);
        } while (double(now.QuadPart - begin.QuadPart) / frequency.QuadPart < seconds);
        const double elapsed = double(now.QuadPart - begin.QuadPart) / frequency.QuadPart;
        Report(samples, elapsed, GpuFrameCount() - startFrame);
        for (auto& [id, handle] : handles) CloseHandle(handle);
        // NIER_SCAN=<f1>,<f2>,<f3>: logs guest addresses holding those three big-endian floats in a row
        // (within 0.01), e.g. to find every copy of a matrix (debugging).
        char scan[128];
        char* cursor = GetEnvironmentVariableA("NIER_SCAN", scan, sizeof(scan)) ? scan : nullptr;
        while (cursor && *cursor)   // several searches separated by ';'
        {
            float wanted[3] = {};
            for (float& value : wanted) { value = strtof(cursor, &cursor); if (*cursor == ',') cursor++; }
            if (*cursor == ';') cursor++;
            int found = 0;
            MEMORY_BASIC_INFORMATION info{};
            for (uint64_t address = 0; address < 0x100000000ull && found < 40;)
            {
                if (!VirtualQuery(g_base + address, &info, sizeof(info)) || !info.RegionSize) break;
                const uint64_t start = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - g_base);
                address = start + info.RegionSize;
                const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
                if (info.State != MEM_COMMIT || !(info.Protect & readable) || (info.Protect & PAGE_GUARD)) continue;
                for (uint64_t at = start; at + 12 <= start + info.RegionSize && at + 12 <= 0x100000000ull && found < 40; at += 4)
                {
                    bool match = true;
                    for (int i = 0; i < 3 && match; i++)
                    {
                        const uint32_t bits = R32(uint32_t(at + i * 4));
                        float value;
                        memcpy(&value, &bits, 4);
                        match = fabsf(value - wanted[i]) < 0.01f;
                    }
                    if (match) { Log("[scan] %08llX\n", (unsigned long long)at); found++; }
                }
            }
            Log("[scan] %d matches for %f %f %f\n", found, wanted[0], wanted[1], wanted[2]);
        }
        // NIER_SCAN_IMLOAD=1 (experiment): finds IM_LOAD packets (type 3, opcode 0x27) in physical memory,
        // e.g. in command buffers recorded at load time, and logs the shaders they reference (hash as in
        // gpu.cpp StoreShader).
        if (GetEnvironmentVariableA("NIER_SCAN_IMLOAD", nullptr, 0) > 0)
        {
            std::map<uint64_t, std::pair<uint32_t, uint32_t>> shaders;   // hash -> stage, address
            uint64_t packets = 0;
            for (uint32_t at = 0xA0000000; at < 0xBFFFFFF0; at += 4)
            {
                if (R32(at) != 0xC0012700) continue;
                const uint32_t addressType = R32(at + 4), startSize = R32(at + 8);
                const uint32_t stage = addressType & 3, dwords = startSize & 0xFFFF;
                const uint32_t physical = addressType & ~3u;
                if (stage > 1 || dwords < 6 || dwords > 4096 || physical >= 0x20000000 || (startSize >> 16) != 0) continue;
                packets++;
                uint64_t hash = 1469598103934665603ull;
                for (uint32_t i = 0; i < dwords; i++) hash = (hash ^ R32(0xA0000000 + physical + i * 4)) * 1099511628211ull;
                shaders.emplace(hash, std::make_pair(stage, physical));
            }
            Log("[imload] %llu IM_LOAD packets in physical memory, %zu distinct shaders\n", (unsigned long long)packets, shaders.size());
            for (const auto& [hash, info] : shaders) Log("[imload] %s %016llX at %08X\n", info.first ? "pixel" : "vertex", (unsigned long long)hash, info.second);
        }
        // NIER_DUMP_RANGE=<guest address>,<bytes>: writes that memory to captures/range_<address>.bin.
        char range[64];
        if (GetEnvironmentVariableA("NIER_DUMP_RANGE", range, sizeof(range)))
        {
            const uint32_t address = strtoul(range, nullptr, 16);
            const char* comma = strchr(range, ',');
            const uint32_t bytes = comma ? strtoul(comma + 1, nullptr, 0) : 4096;
            CreateDirectoryA("..\\captures", nullptr);
            char path[96];
            snprintf(path, sizeof(path), "..\\captures\\range_%08X.bin", address);
            if (FILE* file = fopen(path, "wb"))
            {
                fwrite(g_base + address, 1, bytes, file);
                fclose(file);
                Log("[profile] dumped %u bytes at %08X\n", bytes, address);
            }
        }
        // NIER_SCAN_U32=<hex>[,<hex>...]: logs guest addresses holding those big-endian 32-bit values.
        char words[256];
        if (GetEnvironmentVariableA("NIER_SCAN_U32", words, sizeof(words)))
        {
            std::vector<uint32_t> wanted;
            for (char* item = strtok(words, ","); item; item = strtok(nullptr, ",")) wanted.push_back(strtoul(item, nullptr, 16));
            MEMORY_BASIC_INFORMATION info{};
            int found = 0;
            for (uint64_t address = 0; address < 0x100000000ull && found < 60;)
            {
                if (!VirtualQuery(g_base + address, &info, sizeof(info)) || !info.RegionSize) break;
                const uint64_t start = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - g_base);
                address = start + info.RegionSize;
                const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
                if (info.State != MEM_COMMIT || !(info.Protect & readable) || (info.Protect & PAGE_GUARD)) continue;
                for (uint64_t at = start; at + 4 <= start + info.RegionSize && found < 60; at += 4)
                {
                    const uint32_t value = R32(uint32_t(at));
                    for (uint32_t w : wanted)
                        if (value == w) { Log("[scan] %08X holds %08X\n", uint32_t(at), w); found++; }
                }
            }
        }
        if (exitAfter)
        {
            DumpGuestThreads();
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

void ProfilerInit()
{
    char* setting = nullptr;
    size_t length = 0;
    _dupenv_s(&setting, &length, "NIER_PROFILE");
    if (!setting) return;
    uint64_t firstFrame = strtoull(setting, nullptr, 10);
    const char* comma = strchr(setting, ',');
    double seconds = comma ? atof(comma + 1) : 10.0;
    const bool exitAfter = strstr(setting, ",exit") != nullptr;
    free(setting);
    std::thread(ProfilerThread, firstFrame, seconds, exitAfter).detach();
}
