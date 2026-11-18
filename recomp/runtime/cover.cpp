// Function coverage (debugging; only a coverage build calls CoverHit, see cover_prologue.h).
// NIER_COVER=<first frame>,<last frame>: over those frames, notes for every guest function that
// runs the first and last frame it ran in and how many frames it ran in; the list is logged after
// the last frame. A function that ran in one or two frames out of hundreds is what happened then.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "guest.h"

extern "C" unsigned char gCoverOn = 0;

namespace
{
    struct Seen { uint64_t first, last; uint32_t frames; uint64_t list[10]; };   // list: the first frames it ran in
    std::mutex gCoverMutex;
    std::unordered_map<const char*, Seen> gCovered;
    uint64_t gCoverFirst = ~0ull, gCoverLast = 0;
}

extern "C" void CoverHit(const char* name)
{
    const uint64_t frame = GpuFrameCount();
    std::lock_guard lock(gCoverMutex);
    auto [entry, added] = gCovered.try_emplace(name, Seen{ frame, frame, 1, { frame } });
    if (!added && entry->second.last != frame)
    {
        Seen& seen = entry->second;
        if (seen.frames < 10) seen.list[seen.frames] = frame;
        seen.last = frame;
        seen.frames++;
    }
}

// Called once a frame (from the frame counter's owner).
void CoverTick(uint64_t frame)
{
    static const bool wanted = [] {
        char value[64];
        if (!GetEnvironmentVariableA("NIER_COVER", value, sizeof(value))) return false;
        return sscanf(value, "%llu,%llu", &gCoverFirst, &gCoverLast) == 2;
    }();
    if (!wanted) return;
    if (frame >= gCoverFirst && frame <= gCoverLast) { gCoverOn = 1; return; }
    if (!gCoverOn) return;
    gCoverOn = 0;
    std::lock_guard lock(gCoverMutex);
    Log("[cover] %zu functions ran in frames %llu-%llu\n", gCovered.size(), gCoverFirst, gCoverLast);
    for (const auto& [name, seen] : gCovered)
    {
        char frames[160] = "";
        if (seen.frames <= 10)
            for (uint32_t i = 0; i < seen.frames; i++) snprintf(frames + strlen(frames), sizeof(frames) - strlen(frames), " %llu", seen.list[i]);
        Log("[cover] %s first %llu last %llu frames %u%s%s\n", name, seen.first, seen.last, seen.frames, *frames ? ":" : "", frames);
    }
}
