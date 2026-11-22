// Game logic that counts frames, made to go by time so that it behaves at any frame rate as it
// did at the console's 30 frames a second. Mid-asm hooks (recomp/config/nier_hooks.toml).
#include <windows.h>
#include <cstring>
#include <cmath>
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

// Wind blowing hair and cloth too hard at high frame rates (Kaine's hair and clothes at 240 fps).
// A character's update (sub_82205760) asks for the wind where it stands (sub_82347368) and gives
// it to each of its chains (sub_82186448) before the chain is simulated (sub_82187030, which does
// go by time): every link's velocity gets a push along the wind, the same push every frame
// whatever the frame's length. So the push came eight times as often at 240 fps as at the
// console's 30, and each one counted in full.
// f26: the push's strength for this link (the chain's wind factor times the wind's speed);
// r27: the chain, r30: the link's offset in the chain's array of links (+20, 64 bytes a link,
// velocity at +16).
// NIER_LOG_WIND=1 (test runs) logs, for every two seconds of game time, the pushes given and how
// fast the links were moving: the figures should not depend on the frame rate.
void ChainWindPush(PPCRegister& f26, PPCRegister& r27, PPCRegister& r30)
{
    const float step = GameStep();
    if (!KeepFrameCounts() && step > 0.0f) f26.f64 *= double(step) * 30.0;

    static const bool log = GetEnvironmentVariableA("NIER_LOG_WIND", nullptr, 0) > 0;
    if (!log) return;
    static double pushes = 0.0, speeds = 0.0, time = 0.0;
    static uint32_t links = 0, firstChain = 0, firstLink = 0;
    const uint32_t velocity = R32(r27.u32 + 20) + r30.u32 + 16;
    float v[3];
    for (int i = 0; i < 3; i++) { const uint32_t bits = R32(velocity + i * 4); memcpy(&v[i], &bits, 4); }
    pushes += f26.f64;
    speeds += sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    links++;
    // time goes by the first link seen: it comes round once a frame
    if (!firstChain) { firstChain = r27.u32; firstLink = r30.u32; }
    if (r27.u32 != firstChain || r30.u32 != firstLink) return;
    time += step;
    if (time < 2.0) return;
    Log("[wind] %.2f s at %.0f fps: push %.5f a second for each link, links moving at %.5f on average (frame %llu)\n",
        time, step > 0.0f ? 1.0 / step : 0.0, pushes / time / (links / (time / step)), speeds / links, (unsigned long long)GpuFrameCount());
    pushes = speeds = time = 0.0;
    links = 0; firstChain = 0;
}
