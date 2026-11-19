// Game logic that counts frames, made to go by time so that it behaves at any frame rate as it
// did at the console's 30 frames a second. Mid-asm hooks (recomp/config/nier_hooks.toml).
#include <windows.h>
#include <cstring>
#include <unordered_map>
#include "guest.h"

namespace
{
    // The game's time step for this frame, in seconds (stored by sub_82102C48 each frame).
    float GameStep()
    {
        const uint32_t bits = R32(0x827B8B34);
        float step;
        memcpy(&step, &bits, 4);
        return step > 0.0f && step < 0.25f ? step : 0.0f;
    }
}

namespace
{
    // In place of "the frame counter is a multiple of <frames>": true once every <frames>/30 s of
    // game time, counted separately for each object and place it is asked from. The game thread only.
    bool EveryFramesAt30(uint32_t object, uint32_t place, int frames)
    {
        static std::unordered_map<uint64_t, float> since;
        const float interval = float(frames) / 30.0f;
        float& elapsed = since[(uint64_t(place) << 32) | object];
        elapsed += GameStep();
        if (elapsed < interval) return false;
        elapsed -= interval;
        if (elapsed >= interval) elapsed = 0.0f;   // after a long frame: no burst
        return true;
    }

    bool KeepFrameCounts()   // NIER_FRAME_COUNTS=1: the game's own tests, for comparison
    {
        static const bool keep = GetEnvironmentVariableA("NIER_FRAME_COUNTS", nullptr, 0) > 0;
        return keep;
    }
}

// The trail a partner (Kaine, Emil) follows: sub_821F02C0 adds the player's position to a ring of
// 64 points when the game's frame counter is a multiple of 8, which at 30 fps is every 0.267 s. At
// 240 fps the points came eight times as often and the ring covered an eighth of the distance.
// r11: the frame counter about to be tested (its low three bits), r31: the partner.
void PartnerTrailCadence(PPCRegister& r11, PPCRegister& r31)
{
    if (!KeepFrameCounts()) r11.u64 = EveryFramesAt30(r31.u32, 0x821F06FC, 8) ? 0 : 1;
}

// Kaine warping to the player again and again at high frame rates (4 times in 20 seconds of a
// recorded run in the desert at 240 fps; none with this). While she follows (states 13 and 18 at +6556), her update
// (sub_82213DC0) puts her own position into a ring (+6912) when the frame counter is a multiple
// of 4; once the ring is full, if its oldest and newest points are closer than a threshold she
// counts as stuck and is warped to the player (vtable +944, sub_821E7C70). The ring spans about
// six seconds at 30 fps and under one at 240, where ordinary running no longer covered the
// distance. The test now passes once every 4/30 s of game time.
// r11: the frame counter about to be tested (its low two bits), r31: Kaine.
void PartnerStuckCadence(PPCRegister& r11, PPCRegister& r31)
{
    if (!KeepFrameCounts()) r11.u64 = EveryFramesAt30(r31.u32, 0x82214118, 4) ? 0 : 1;
}

// The fishing camera turning far too fast at high frame rates. Its update (sub_821A5830) turns
// the camera's angles into degrees, adds the right stick times the sensitivity setting, and
// stores them back: a fixed number of degrees a frame, where the ordinary camera goes by time.
// f29, f30: the stick's turn for this frame, across and up/down, in degrees at 30 fps.
void FishingCameraTurn(PPCRegister& f29, PPCRegister& f30)
{
    if (KeepFrameCounts()) return;
    const double share = double(GameStep()) * 30.0;
    f29.f64 *= share;
    f30.f64 *= share;
}
