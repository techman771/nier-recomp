// Runs the game's audio engine and plays what it mixes through WASAPI.
//
// The engine pulls in 256-sample blocks. The output device's consumption decides when the next
// block is requested, which keeps movies in sync with the sound. Without a usable device the same
// schedule runs from a timer so the game still advances.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
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
        Log("[audio] output started: stereo float 48kHz, device buffer %u samples\n", gBufferFrames);
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
        if (GetEnvironmentVariableA("NIER_MUTE", mute, sizeof(mute))) Log("[audio] muted: running on a timer\n");
        else gDeviceOk = OpenDevice();

        LARGE_INTEGER frequency, next, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&next);
        const int64_t period = frequency.QuadPart * SAMPLES_PER_FRAME / SAMPLE_RATE;

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
                    Sleep(1);
                    continue;
                }
                if (queued == 0 && gCallbacks.load() > 16) gUnderfed++;
                Pump(ctx, stack);
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
            Pump(ctx, stack);
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
            gRender->ReleaseBuffer(SAMPLES_PER_FRAME, 0);
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
