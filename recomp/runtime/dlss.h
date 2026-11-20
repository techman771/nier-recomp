#pragma once
// NVIDIA DLSS through Streamline (dlss.cpp). Streamline's own code is MIT licensed and is loaded
// at run time from beside the executable (sl.interposer.dll, sl.common.dll, sl.dlss.dll) together
// with NVIDIA's DLSS model (nvngx_dlss.dll); tools/get_streamline.ps1 fetches the SDK and
// compile_ppc.ps1 builds with it when it is there. Without the SDK at build time, or the files at
// run time, or an RTX card, everything here answers "not available" and FSR does the work.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

// Before the D3D12 device is created: loads and starts Streamline if its files are there.
void DlssStart();
// Once the device exists: hands it to Streamline and asks whether the adapter can run DLSS.
void DlssSetDevice(ID3D12Device* device, const void* adapterLuid, uint32_t luidBytes);
bool DlssAvailable();
// The swap chain, right after it is created: Streamline swaps the pointer for its own stand-in,
// through which it sees each Present (its bookkeeping runs there). Unchanged without Streamline.
void DlssUpgradeInterface(void** dxgiInterface);
// "DLSS 310.2.1" once known, else "DLSS".
const char* DlssName();

struct DlssFrame
{
    ID3D12GraphicsCommandList* list = nullptr;
    // All in the D3D12 state given (Streamline puts them back as it found them).
    ID3D12Resource* color = nullptr; uint32_t colorState = 0;     // the jittered scene, render size, sRGB-encoded
    ID3D12Resource* depth = nullptr; uint32_t depthState = 0;     // 0 near to 1 far
    ID3D12Resource* motion = nullptr; uint32_t motionState = 0;   // to the previous position, in screen fractions
    ID3D12Resource* output = nullptr; uint32_t outputState = 0;
    uint32_t renderWidth = 0, renderHeight = 0, outWidth = 0, outHeight = 0;
    float jitterX = 0, jitterY = 0;      // pixels
    bool reset = false;
    int quality = 1;                     // the Upscaling Quality setting: 1 Native (DLAA) to 6 Ultra Performance
    const double* clipToPrevClip = nullptr;   // 16, row-major, column vectors (prev = M * now); null: none
    float nearPlane = 0.1f, fovY = 1.0f;
    uint64_t frame = 0;
};
// Records the upscale into the command list. False if it could not (the caller falls back).
// The command list's descriptor heaps, root signature and pipeline are not kept.
bool DlssEvaluate(const DlssFrame& frame);
// The sizes or the mode are about to change: lets go of DLSS's own resources. The GPU must be idle.
void DlssRelease();
