// Runs the game's audio engine and plays what it mixes through WASAPI.
//
// The engine pulls in 256-sample blocks. The output device's consumption decides when the next
// block is requested, which keeps movies in sync with the sound. Without a usable device the same
// schedule runs from a timer so the game still advances.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>
#include <thread>
#include "guest.h"
#include "settings.h"

namespace
{
    constexpr uint32_t SAMPLES_PER_FRAME = 256, SAMPLE_RATE = 48000, CHANNELS = 6;
    constexpr uint32_t PLANE_BYTES = SAMPLES_PER_FRAME * 4;
    constexpr uint32_t TARGET_QUEUED = SAMPLES_PER_FRAME * 8;  // about 43ms

    std::atomic<uint32_t> gCallback{ 0 }, gCallbackArgument{ 0 };
    std::atomic<uint64_t> gCallbacks{ 0 }, gFramesSubmitted{ 0 }, gUnderfed{ 0 };
    std::atomic<bool> gRunning{ false };

    IAudioClient* gClient = nullptr;
    IAudioRenderClient* gRender = nullptr;
    uint32_t gBufferFrames = 0;
    bool gDeviceOk = false;
    bool gSilent = false;   // NIER_MUTE=2: the device runs and paces the engine, but is given silence

    // How the output is doing, about every ten seconds (1875 blocks): blocks a second (187.5 when
    // the engine keeps up with the device), how often the device had nothing left, the longest
    // time between two blocks and the longest the engine took over one, and blocks that were
    // silent straight after one that was not (a stream that ran out for a moment). A line is
    // logged when any of it is off; NIER_AUDIO_HEALTH=1 logs every one.
    struct Health { int64_t since = 0, lastPump = 0, longestGap = 0, longestPump = 0; uint32_t pumps = 0, dry = 0, dropouts = 0; bool lastLoud = false; } gHealth;

    // The Windows timer resolution this process has right now, in ms (0 if it cannot be asked).
    // main.cpp asks for 1 ms; at 15.6 a Sleep(1) lasts a whole tick.
    double TimerResolutionMs()
    {
        using Query = LONG (NTAPI*)(ULONG*, ULONG*, ULONG*);
        static const Query query = (Query)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
        ULONG coarsest = 0, finest = 0, current = 0;
        return query && query(&coarsest, &finest, &current) >= 0 ? current / 10000.0 : 0.0;
    }

    // Waits about a millisecond whatever the timer resolution is: a high-resolution waitable timer
    // (Windows 10 1803 on), or Sleep(1) where there is none (older Windows, Wine).
    void ShortWait()
    {
        static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -10000;   // 1 ms from now, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 20);
        else Sleep(1);
    }

    void HealthAfterPump(int64_t began, int64_t ended, bool dry)
    {
        static const bool always = GetEnvironmentVariableA("NIER_AUDIO_HEALTH", nullptr, 0) > 0;
        static const int64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        Health& h = gHealth;
        if (!h.since) h.since = began;
        if (h.lastPump) h.longestGap = (std::max)(h.longestGap, began - h.lastPump);
        h.lastPump = began;
        h.longestPump = (std::max)(h.longestPump, ended - began);
        h.dry += dry;
        if (++h.pumps < 1875) return;
        const double seconds = double(ended - h.since) / double(frequency), rate = h.pumps / seconds, k = 1000.0 / double(frequency);
        static int logged = 0;
        const bool off = h.dry > 0 || h.dropouts > 0 || double(h.longestGap) * k > 40.0 || rate < 183.0;
        if (always || (off && logged++ < 200))
            Log("[audio] last %.1f s: %.1f blocks a second (187.5 keeps up), the device ran dry %u times, longest %.1f ms between blocks, slowest block %.1f ms, %u silent blocks in mid-sound, timer resolution %.1f ms (frame %llu)\n",
                seconds, rate, h.dry, double(h.longestGap) * k, double(h.longestPump) * k, h.dropouts, TimerResolutionMs(), (unsigned long long)GpuFrameCount());
        const bool loud = h.lastLoud;
        h = Health{};
        h.lastLoud = loud;
        h.lastPump = began;
        h.since = ended;
    }

    // Signal measurements for checking the output without listening to it.
    double gChannelPeak[CHANNELS], gSumSquares = 0, gSumDelta = 0;
    float gPreviousLeft = 0;
    uint64_t gMeasuredSamples = 0;

    bool OpenDevice()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (SUCCEEDED(result)) result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(result)) result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&gClient);
        if (FAILED(result))
        {
            Log("[audio] no output device (0x%08lX); running silently on a timer\n", result);
            return false;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec = SAMPLE_RATE * 8;
        const REFERENCE_TIME bufferDuration = 2000000;  // 200ms, in 100ns units
        result = gClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, bufferDuration, 0, &format, nullptr);
        if (SUCCEEDED(result)) result = gClient->GetBufferSize(&gBufferFrames);
        if (SUCCEEDED(result)) result = gClient->GetService(__uuidof(IAudioRenderClient), (void**)&gRender);
        if (SUCCEEDED(result)) result = gClient->Start();
        if (FAILED(result))
        {
            Log("[audio] could not start output (0x%08lX); running silently on a timer\n", result);
            return false;
        }
        REFERENCE_TIME devicePeriod = 0, shortestPeriod = 0;
        gClient->GetDevicePeriod(&devicePeriod, &shortestPeriod);
        Log("[audio] output started: stereo float 48kHz, device buffer %u samples, device period %.1f ms\n", gBufferFrames, double(devicePeriod) / 10000.0);
        return true;
    }

    void Pump(PPCContext& ctx, uint64_t stack)
    {
        uint32_t callback = gCallback.load();
        if (!callback) return;
        ctx.r1.u64 = stack;
        ctx.r3.u64 = gCallbackArgument.load();
        ctx.lr = 0;
        CallGuestFunction(ctx, callback);
        gCallbacks++;
    }

    void AudioThread()
    {
        PPCContext ctx;
        AttachGuestThread(ctx);
        uint64_t stack = ctx.r1.u64;
        char mute[8];
        const bool muted = GetEnvironmentVariableA("NIER_MUTE", mute, sizeof(mute)) > 0;
        gSilent = muted && (mute[0] == '2' || GetEnvironmentVariableA("NIER_SILENT_DEVICE", nullptr, 0) > 0);   // (the test scripts set NIER_MUTE=1 themselves)
        if (muted && !gSilent) Log("[audio] muted: running on a timer\n");
        else gDeviceOk = OpenDevice();
        if (gSilent && gDeviceOk) Log("[audio] muted, with the device running (it is given silence)\n");

        LARGE_INTEGER frequency, next, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next);
        const int64_t period = frequency.QuadPart * SAMPLES_PER_FRAME / SAMPLE_RATE;
        int64_t lastPump = 0;

        while (true)
        {
            if (gDeviceOk)
            {
                UINT32 queued = 0;
                if (FAILED(gClient->GetCurrentPadding(&queued)))
                {
                    Log("[audio] output device lost; continuing silently\n");
                    gDeviceOk = false;
                    QueryPerformanceCounter(&next);
                    continue;
                }
                if (queued + SAMPLES_PER_FRAME > TARGET_QUEUED)
                {
                    ShortWait();
                    continue;
                }
                // Blocks are asked of the game at an even pace, never several at once. When this
                // thread wakes late (a Sleep(1) that lasts a 15.6 ms timer tick: seen in play
                // 2026-11-21, 21 ms between blocks) the queue has gone down by three blocks, and
                // filling it back up in one go asked the game for them within a millisecond: its
                // streams had only so much ready, and blocks came back silent (about 15 a second:
                // crackling, and music that ran slow). So no block sooner than three quarters of a
                // block's length after the last (half, while the queue is low), and the waits are
                // ShortWait's, which do not depend on the timer resolution.
                {
                    LARGE_INTEGER sinceLast;
                    QueryPerformanceCounter(&sinceLast);
                    const int64_t spacing = queued < SAMPLES_PER_FRAME * 3 ? period / 2 : period * 3 / 4;
                    const int64_t remaining = lastPump ? lastPump + spacing - sinceLast.QuadPart : 0;
                    if (remaining > 0)
                    {
                        if (remaining > frequency.QuadPart / 500) ShortWait();
                        else SwitchToThread();
                        continue;
                    }
                }
                const bool dry = queued == 0 && gCallbacks.load() > 16;
                if (dry) gUnderfed++;
                LARGE_INTEGER began, ended;
                QueryPerformanceCounter(&began);
                lastPump = began.QuadPart;
                Pump(ctx, stack);
                QueryPerformanceCounter(&ended);
                HealthAfterPump(began.QuadPart, ended.QuadPart, dry);
                continue;
            }

            next.QuadPart += period;
            for (;;)
            {
                QueryPerformanceCounter(&now);
                int64_t remaining = next.QuadPart - now.QuadPart;
                if (remaining <= 0) break;
                if (remaining > frequency.QuadPart / 500) Sleep(1);
                else SwitchToThread();
            }
            LARGE_INTEGER began, ended;
            QueryPerformanceCounter(&began);
            Pump(ctx, stack);
            QueryPerformanceCounter(&ended);
            HealthAfterPump(began.QuadPart, ended.QuadPart, false);
        }
    }

    float ReadFloat(uint32_t address)
    {
        uint32_t bits = R32(address);
        float value;
        memcpy(&value, &bits, 4);
        return std::isfinite(value) ? value : 0.0f;
    }

    float Limit(float value) { return value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value; }

    uint32_t FloatBits(float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        return bits;
    }
}

void AudioDumpStats()
{
    double rms = gMeasuredSamples ? std::sqrt(gSumSquares / gMeasuredSamples) : 0;
    double roughness = gSumSquares > 0 ? (gSumDelta / gMeasuredSamples) / rms : 0;
    Log("[audio] %llu callbacks, %llu frames submitted, %llu times the device ran dry\n", gCallbacks.load(), gFramesSubmitted.load(), gUnderfed.load());
    Log("[audio] left-channel rms %.4f, step/rms %.3f (white noise is about 1.1); peaks FL %.3f FR %.3f FC %.3f LFE %.3f BL %.3f BR %.3f\n", rms, roughness,
        gChannelPeak[0], gChannelPeak[1], gChannelPeak[2], gChannelPeak[3], gChannelPeak[4], gChannelPeak[5]);
}

GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t callbackStruct = ctx.r3.u32, driverOut = ctx.r4.u32;
    // The callback dereferences its parameter once, so it receives the address of the argument.
    uint32_t argumentSlot = GuestAlloc(4, 4, Region::Heap);
    W32(argumentSlot, R32(callbackStruct + 4));
    gCallbackArgument = argumentSlot;
    gCallback = R32(callbackStruct);
    W32(driverOut, 0x41550000);
    Log("[audio] render client callback 0x%08X argument 0x%08X\n", gCallback.load(), gCallbackArgument.load());
    if (!gRunning.exchange(true)) std::thread(AudioThread).detach();
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    gCallback = 0;
    ctx.r3.u64 = 0;
}

// NIER_AUDIO_DUMP=<frame>,<seconds>: records the game's six-channel mix to captures/audio.wav from that
// guest frame on (32-bit float, 48 kHz), to check the output without listening (debugging).
namespace
{
    FILE* gDumpFile = nullptr;
    uint64_t gDumpFrames = 0, gDumpLimit = 0;
    bool gDumpDone = false;

    void DumpAudio(uint32_t samples)
    {
        static const std::pair<uint64_t, double> setting = [] {
            char value[64];
            if (!GetEnvironmentVariableA("NIER_AUDIO_DUMP", value, sizeof(value))) return std::make_pair(~0ull, 0.0);
            const char* comma = strchr(value, ',');
            return std::make_pair(strtoull(value, nullptr, 10), comma ? atof(comma + 1) : 10.0);
        }();
        if (gDumpDone || GpuFrameCount() < setting.first) return;
        if (!gDumpFile)
        {
            CreateDirectoryA("..\\captures", nullptr);
            gDumpFile = fopen("..\\captures\\audio.wav", "wb");
            if (!gDumpFile) { gDumpDone = true; return; }
            gDumpLimit = uint64_t(setting.second * SAMPLE_RATE / SAMPLES_PER_FRAME);
            uint8_t header[44] = {};
            fwrite(header, 1, 44, gDumpFile);   // filled in when done
            Log("[audio] recording the mix from frame %llu\n", GpuFrameCount());
        }
        float block[SAMPLES_PER_FRAME * CHANNELS];
        for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++)
            for (uint32_t c = 0; c < CHANNELS; c++) block[i * CHANNELS + c] = ReadFloat(samples + c * PLANE_BYTES + i * 4);
        fwrite(block, sizeof(block), 1, gDumpFile);
        if (++gDumpFrames < gDumpLimit) return;
        const uint32_t dataBytes = uint32_t(gDumpFrames * sizeof(block));
        struct { char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmtSize; uint16_t format, channels; uint32_t rate, byteRate; uint16_t align, bits; char data[4]; uint32_t dataSize; } h{
            { 'R','I','F','F' }, 36 + dataBytes, { 'W','A','V','E' }, { 'f','m','t',' ' }, 16, 3, CHANNELS, SAMPLE_RATE, SAMPLE_RATE * CHANNELS * 4, CHANNELS * 4, 32, { 'd','a','t','a' }, dataBytes };
        fseek(gDumpFile, 0, SEEK_SET);
        fwrite(&h, sizeof(h), 1, gDumpFile);
        fclose(gDumpFile);
        gDumpDone = true;
        Log("[audio] recorded %.1f s to captures/audio.wav\n", double(gDumpFrames) * SAMPLES_PER_FRAME / SAMPLE_RATE);
    }
}

// Samples arrive as six consecutive planes of 256 big-endian floats: FL, FR, FC, LFE, BL, BR.
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    uint32_t samples = ctx.r4.u32;
    gFramesSubmitted++;
    DumpAudio(samples);

    float left[SAMPLES_PER_FRAME], right[SAMPLES_PER_FRAME];
    for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++)
    {
        float channel[CHANNELS];
        for (uint32_t c = 0; c < CHANNELS; c++)
        {
            channel[c] = ReadFloat(samples + c * PLANE_BYTES + i * 4);
            double magnitude = std::fabs(channel[c]);
            if (magnitude > gChannelPeak[c]) gChannelPeak[c] = magnitude;
        }
        const float side = 0.7071f;
        left[i] = Limit(channel[0] + side * channel[2] + side * channel[4]);
        right[i] = Limit(channel[1] + side * channel[2] + side * channel[5]);
        gSumSquares += double(left[i]) * left[i];
        gSumDelta += std::fabs(double(left[i]) - gPreviousLeft);
        gPreviousLeft = left[i];
        gMeasuredSamples++;
    }

    // PC Settings: master volume, and silence in the background. The gain moves a little per sample
    // so changes don't click.
    static float gain = 1.0f;
    SettingsPollPage();   // so Master Volume is heard while it's adjusted
    const PcSettings& settings = Settings();
    const bool focused = PresenterHasFocus();
    const float target = (settings.muteInBackground && !focused) ? 0.0f : settings.masterVolume / 10.0f;
    {
        // A block of pure silence straight after one with sound in it: counted (gHealth).
        bool loud = false;
        for (uint32_t i = 0; i < SAMPLES_PER_FRAME && !loud; i++) loud = left[i] != 0.0f || right[i] != 0.0f;
        if (!loud && gHealth.lastLoud) gHealth.dropouts++;
        gHealth.lastLoud = loud;
    }
    float out[SAMPLES_PER_FRAME * 2];
    double outSquares = 0, inSquares = 0;
    for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++)
    {
        gain += (target - gain) * 0.002f;
        out[i * 2] = left[i] * gain;
        out[i * 2 + 1] = right[i] * gain;
        outSquares += double(out[i * 2]) * out[i * 2];
        inSquares += double(left[i]) * left[i];
    }

    // NIER_AUDIO_LEVELS=1: the mix and output level about every 5 s, with what decides the gain.
    static const bool logLevels = GetEnvironmentVariableA("NIER_AUDIO_LEVELS", nullptr, 0) > 0;
    static double levelIn = 0, levelOut = 0;
    static uint32_t levelFrames = 0;
    if (logLevels)
    {
        levelIn += inSquares; levelOut += outSquares;
        if (++levelFrames == 5 * SAMPLE_RATE / SAMPLES_PER_FRAME)
        {
            const double n = double(levelFrames) * SAMPLES_PER_FRAME;
            Log("[audio] level: mix rms %.4f, output rms %.4f, gain %.2f (volume %d, mute in background %d, focused %d, device %d)\n",
                std::sqrt(levelIn / n), std::sqrt(levelOut / n), gain, settings.masterVolume.load(), settings.muteInBackground.load(), focused, gDeviceOk);
            levelIn = levelOut = 0;
            levelFrames = 0;
        }
    }

    if (gDeviceOk)
    {
        BYTE* destination = nullptr;
        if (SUCCEEDED(gRender->GetBuffer(SAMPLES_PER_FRAME, &destination)))
        {
            memcpy(destination, out, sizeof(out));
            gRender->ReleaseBuffer(SAMPLES_PER_FRAME, gSilent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        }
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    W32(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    W32(ctx.r4.u32, FloatBits(1.0f));
    ctx.r3.u64 = 0;
}
