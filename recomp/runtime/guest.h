// Shared runtime interface: guest memory access, allocation, handles and import definitions.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "ppc_config.h"
#include "ppc_context.h"

extern uint8_t* g_base;
extern std::string g_gameDir;

inline uint8_t R8(uint32_t a) { return *(volatile uint8_t*)(g_base + a); }
inline uint16_t R16(uint32_t a) { return __builtin_bswap16(*(volatile uint16_t*)(g_base + a)); }
inline uint32_t R32(uint32_t a) { return __builtin_bswap32(*(volatile uint32_t*)(g_base + a)); }
inline uint64_t R64(uint32_t a) { return __builtin_bswap64(*(volatile uint64_t*)(g_base + a)); }
inline void W8(uint32_t a, uint8_t v) { *(volatile uint8_t*)(g_base + a) = v; }
inline void W16(uint32_t a, uint16_t v) { *(volatile uint16_t*)(g_base + a) = __builtin_bswap16(v); }
inline void W32(uint32_t a, uint32_t v) { *(volatile uint32_t*)(g_base + a) = __builtin_bswap32(v); }
inline void W64(uint32_t a, uint64_t v) { *(volatile uint64_t*)(g_base + a) = __builtin_bswap64(v); }
inline const char* GuestStr(uint32_t a) { return (const char*)(g_base + a); }

// Arguments past r10 live on the guest stack in 8-byte slots; the 32-bit value is the low half.
inline uint32_t StackArg(const PPCContext& ctx, int index) { return R32(ctx.r1.u32 + 0x54 + (index - 8) * 8); }

enum class Region { Heap, Virtual, Physical };
void MemoryInit();
uint32_t GuestAlloc(uint32_t size, uint32_t align, Region region);
void GuestFree(uint32_t address);
uint32_t PhysicalToGuest(uint32_t physical);
void GpuDumpStats();
void AudioDumpStats();
uint64_t GpuFrameCount();
void GpuRequestCapture();   // F12: log every draw of the next frame and save its image
void ProfilerInit();        // NIER_PROFILE=<frame>,<seconds>: sampling profile into the log
void GpuResetFrameTiming(); // restart the frame time statistics (the profiler does at its start)
void GpuInstallHooks();    // replaces indirect-call table entries (NIER_60FPS)
void NoteGuestSwap();      // the game submitted a frame (for NIER_FIXED_TIME)
uint64_t FixedElapsed100ns();   // NIER_FIXED_TIME's elapsed time
void DumpGuestThreads();   // logs every guest thread's stack and last imports
void WatchInit();
void BreakInit();           // NIER_BREAK=<guest function>,<frame>: log registers when it runs           // NIER_WATCH_WRITE=<guest address>,<bytes>: log the guest code writing there
void LogSetCaptureFile(FILE* file);   // also copy log output to this file (null to stop)
bool PresenterHasFocus();
void PresenterInit();
void PresenterDumpStats();

uint32_t LoadGame(const std::string& gameDir);     // returns the entry point
void UpdateTimeBundle();
uint32_t XexHeaderGuest();

void ThreadsInit();
void RunMainThread(uint32_t entry);
uint32_t AttachGuestThread(PPCContext& ctx);       // gives a host thread a guest identity; returns its PCR
void CallGuestFunction(PPCContext& ctx, uint32_t address);
uint32_t CurrentKThread();
PPCContext* CurrentContext();

constexpr uint32_t X_STATUS_SUCCESS = 0;
constexpr uint32_t X_STATUS_TIMEOUT = 0x102;
constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t X_STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;

enum class HandleKind { Event, Semaphore, Thread, File };
struct HandleEntry
{
    HandleKind kind;
    uint32_t object = 0;     // guest dispatcher object, for waitable kinds
    void* host = nullptr;    // host HANDLE, for files and threads
    std::string path;        // host path, for files
};
uint32_t HandleCreate(const HandleEntry& entry);
bool HandleGet(uint32_t handle, HandleEntry& out);
bool HandleClose(uint32_t handle);
void SetEventObject(uint32_t object);
void SaveRootRegister(const std::string& name, const std::string& directory);
void SaveRootRemove(const std::string& name);
bool SaveRootFind(const std::string& name, std::string& directory);

void Log(const char* format, ...);
void RegisterGuestThread(PPCContext* ctx);
void TraceImport(const char* name, PPCContext& ctx);
void StubHit(const char* name, PPCContext& ctx);
[[noreturn]] void FatalGuest(const char* reason, PPCContext& ctx);
void InstallCrashHandler();

#define GUEST_IMPORT(name)                                       \
    static void impl_##name(PPCContext& ctx, uint8_t* base);     \
    PPC_FUNC(__imp__##name)                                      \
    {                                                            \
        TraceImport(#name, ctx);                                 \
        impl_##name(ctx, base);                                  \
    }                                                            \
    static void impl_##name(PPCContext& ctx, uint8_t* base)
