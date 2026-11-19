// The game's main loop counted in ticks, and its time steps recorded and replayed with recorded
// input (NIER_RECORD_INPUT / NIER_REPLAY_INPUT, input.cpp), so that a replay simulates what was
// recorded tick for tick.
//
// The game runs on real time: each turn of the main loop (sub_82496D00) is simulated with the
// time the last one took. A replay that only gave the recorded pad states back by frame number
// or by time went off course, because the two runs' frame times differ (a dip to 200 fps in one
// and not the other) and the camera and the player's movement are not the same after different
// steps. So from a recording's last "route" marker on (gameplay beginning after a loading
// screen), every step is written to <recording>.steps, one float a tick, and a replay hands the
// game those steps in place of its own and looks the pad states up by tick. The game then moves
// slightly faster or slower than real time on screen, which a test run doesn't mind.
// Mid-asm hook GameStepHook (recomp/config/nier_hooks.toml).
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"

namespace
{
    std::atomic<uint64_t> gTick{ 0 };          // turns of the main loop so far
    std::atomic<uint64_t> gMarkTick{ ~0ull };  // the tick the route began at in this run
    std::mutex gMutex;
    FILE* gRecord = nullptr;                   // <recording>.steps being written
    std::string gRecordPath;
    std::vector<float> gSteps;                 // <recording>.steps being replayed
    bool gReplaying = false;

    // NIER_LOG_STALLS=1 (set for playtests by run_boot.ps1): when the main loop has not turned for
    // 100 ms, the log gets every guest thread's stack, so a hitch that can't be reproduced on
    // demand still shows what the game was doing. The first 25 stalls of a run; loading screens
    // count, and are told apart by where they are in the log.
    void WatchForStalls()
    {
        uint64_t last = gTick.load(), dumpedAt = ~0ull;
        ULONGLONG since = GetTickCount64();
        for (int dumps = 0;;)
        {
            Sleep(10);
            const uint64_t tick = gTick.load();
            const ULONGLONG now = GetTickCount64();
            if (tick != last)
            {
                if (dumpedAt == last) Log("[stall] the main loop went on after %llu ms\n", now - since);
                last = tick;
                since = now;
                continue;
            }
            if (now - since < 100 || dumpedAt == tick || dumps >= 25) continue;
            dumpedAt = tick;
            dumps++;
            Log("[stall] the main loop has not turned for %llu ms (tick %llu, frame %llu)\n", now - since, tick, GpuFrameCount());
            DumpGuestThreads();
        }
    }

    void Init()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            if (GetEnvironmentVariableA("NIER_LOG_STALLS", nullptr, 0) > 0) std::thread(WatchForStalls).detach();
            char path[MAX_PATH];
            if (GetEnvironmentVariableA("NIER_RECORD_INPUT", path, sizeof(path))) gRecordPath = std::string(path) + ".steps";
            if (GetEnvironmentVariableA("NIER_REPLAY_INPUT", path, sizeof(path)) && GetEnvironmentVariableA("NIER_REPLAY_BY_FRAME", nullptr, 0) == 0)
                if (FILE* file = fopen((std::string(path) + ".steps").c_str(), "rb"))
                {
                    float step;
                    while (fread(&step, sizeof(step), 1, file) == 1) gSteps.push_back(step);
                    fclose(file);
                    gReplaying = !gSteps.empty();
                    Log("[input] replaying %zu recorded time steps\n", gSteps.size());
                }
        });
    }
}

uint64_t GameTick() { return gTick.load(); }

// The route begins now: a marker being recorded, or the replay reaching the recording's last one.
void GameTickMarker()
{
    Init();
    std::lock_guard lock(gMutex);
    gMarkTick = gTick.load();
    if (!gRecordPath.empty())
    {
        if (gRecord) fclose(gRecord);
        gRecord = fopen(gRecordPath.c_str(), "wb");   // the steps of the last route only
    }
}

// f31: the time step, in seconds, the main loop is about to simulate.
void GameStepHook(PPCRegister& f31)
{
    Init();
    const uint64_t tick = ++gTick;
    const uint64_t mark = gMarkTick.load();
    if (mark == ~0ull || tick <= mark) return;
    const uint64_t index = tick - mark - 1;
    if (gReplaying)
    {
        if (index < gSteps.size()) f31.f64 = double(gSteps[index]);
        return;
    }
    std::lock_guard lock(gMutex);
    if (gRecord)
    {
        const float step = float(f31.f64);
        fwrite(&step, sizeof(step), 1, gRecord);
        if ((index & 63) == 63) fflush(gRecord);
    }
}
