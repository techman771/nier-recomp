// NVIDIA DLSS through Streamline: see dlss.h.
#include "dlss.h"
#include "guest.h"

#if __has_include(<sl.h>) && !defined(NIER_NO_DLSS)

#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>

// sl.interposer.dll is delay-loaded (compile_ppc.ps1): nothing of it is touched unless DlssStart
// found it and loaded it by its full path.

namespace
{
    int gState = 0;            // 0 not started, 1 Streamline is running, -1 unavailable
    bool gAvailable = false;   // the adapter can run DLSS
    char gName[48] = "DLSS";
    sl::ViewportHandle gViewport{ uint32_t(0) };
    sl::FrameToken* gToken = nullptr;
    uint64_t gTokenFrame = ~0ull;
    uint32_t gOptionsWidth = 0, gOptionsHeight = 0;
    int gOptionsQuality = -1;

    void LogMessage(sl::LogType type, const char* message)
    {
        // Warnings and errors only, and not without end; NIER_DLSS_LOG=1 passes everything.
        static const bool all = GetEnvironmentVariableA("NIER_DLSS_LOG", nullptr, 0) > 0;
        static int logged = 0;
        if (!all && (type == sl::LogType::eInfo || logged++ >= 40)) return;
        std::string text = message ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        Log("[dlss] %s: %s\n", type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warning" : "info", text.c_str());
    }

    bool Failed(sl::Result result, const char* what)
    {
        if (result == sl::Result::eOk) return false;
        static int logged = 0;
        if (logged++ < 12) Log("[dlss] %s failed: %s\n", what, sl::getResultAsStr(result));
        return true;
    }

    sl::DLSSMode ModeFor(int quality)
    {
        switch (quality)
        {
        case 2: return sl::DLSSMode::eMaxQuality;   // DLSS has no Ultra Quality (creating it fails): Quality, given the 1.3 picture
        case 3: return sl::DLSSMode::eMaxQuality;
        case 4: return sl::DLSSMode::eBalanced;
        case 5: return sl::DLSSMode::eMaxPerformance;
        case 6: return sl::DLSSMode::eUltraPerformance;
        }
        return sl::DLSSMode::eDLAA;
    }
}

void DlssStart()
{
    if (gState) return;
    gState = -1;
    // NIER_DLSS=0: never. Under Wine (Proton) only with NIER_DLSS=1: nothing there has been tried.
    char wanted[8] = "";
    const bool asked = GetEnvironmentVariableA("NIER_DLSS", wanted, sizeof(wanted)) > 0;
    if (asked && wanted[0] == '0') return;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version") && !asked) return;

    // The files, beside the executable: Streamline asks for full paths.
    wchar_t folder[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, folder, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(folder, L'\\');
    if (!slash) return;
    *slash = 0;
    for (const wchar_t* name : { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"nvngx_dlss.dll" })
    {
        const std::wstring path = std::wstring(folder) + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Log("[dlss] %ls is not beside the executable (tools/get_streamline.ps1): no DLSS\n", name);
            return;
        }
    }
    const std::wstring interposer = std::wstring(folder) + L"\\sl.interposer.dll";
    if (!LoadLibraryW(interposer.c_str()))
    {
        Log("[dlss] sl.interposer.dll could not be loaded (error %lu): no DLSS\n", GetLastError());
        return;
    }

    static const sl::Feature features[] = { sl::kFeatureDLSS };
    static const wchar_t* paths[1];
    static std::wstring pluginFolder;
    pluginFolder = folder;
    paths[0] = pluginFolder.c_str();
    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogMessage;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    // The renderer creates its own device, queue and swap chain and hands Streamline what it needs
    // (manual hooking), restores the command list itself after an evaluate, and tags the frame's
    // buffers by frame token. No over-the-air updates: nothing here goes to the network.
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = "6f0a3c52-9d1e-4b7a-8c35-2e4f7a91d0b6";   // this project's own (any GUID serves an unregistered application)
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    if (Failed(slInit(preferences, sl::kSDKVersion), "starting Streamline")) return;
    gState = 1;
}

void DlssSetDevice(ID3D12Device* device, const void* adapterLuid, uint32_t luidBytes)
{
    if (gState != 1) return;
    if (Failed(slSetD3DDevice(device), "giving Streamline the device")) { gState = -1; return; }
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = const_cast<uint8_t*>(static_cast<const uint8_t*>(adapterLuid));
    adapter.deviceLUIDSizeInBytes = luidBytes;
    const sl::Result supported = slIsFeatureSupported(sl::kFeatureDLSS, adapter);
    if (supported != sl::Result::eOk)
    {
        Log("[dlss] not available on this adapter: %s\n", sl::getResultAsStr(supported));
        return;
    }
    sl::FeatureVersion version{};
    if (slGetFeatureVersion(sl::kFeatureDLSS, version) == sl::Result::eOk && version.versionNGX)
        snprintf(gName, sizeof(gName), "DLSS %u.%u.%u", version.versionNGX.major, version.versionNGX.minor, version.versionNGX.build);
    gAvailable = true;
    Log("[dlss] available: %s (Streamline %u.%u.%u)\n", gName, version.versionSL.major, version.versionSL.minor, version.versionSL.build);
}

bool DlssAvailable() { return gAvailable; }
const char* DlssName() { return gName; }

void DlssUpgradeInterface(void** dxgiInterface)
{
    if (gState != 1 || !gAvailable) return;
    Failed(slUpgradeInterface(dxgiInterface), "wrapping the swap chain");
}

void DlssRelease()
{
    if (!gAvailable || gOptionsQuality < 0) return;
    slFreeResources(sl::kFeatureDLSS, gViewport);
    gOptionsQuality = -1;
}

bool DlssEvaluate(const DlssFrame& f)
{
    if (!gAvailable) return false;
    if (f.frame != gTokenFrame || !gToken)
    {
        const uint32_t index = uint32_t(f.frame);
        if (Failed(slGetNewFrameToken(gToken, &index), "a frame token") || !gToken) return false;
        gTokenFrame = f.frame;
    }
    if (f.quality != gOptionsQuality || f.outWidth != gOptionsWidth || f.outHeight != gOptionsHeight)
    {
        sl::DLSSOptions options{};
        options.mode = ModeFor(f.quality);
        options.outputWidth = f.outWidth;
        options.outputHeight = f.outHeight;
        options.colorBuffersHDR = sl::Boolean::eFalse;   // the game's picture: 8 bits, sRGB-encoded
        options.useAutoExposure = sl::Boolean::eFalse;
        if (Failed(slDLSSSetOptions(gViewport, options), "setting the DLSS options")) return false;
        gOptionsQuality = f.quality;
        gOptionsWidth = f.outWidth;
        gOptionsHeight = f.outHeight;
    }

    // Streamline's matrices are row-major for row vectors (v * M); the renderer's are for column
    // vectors, hence the transposes. DLSS itself reads the jitter, the motion vector scale, the
    // depth convention and the reset; the camera is filled in as far as it is known.
    sl::Constants constants{};
    const float aspect = float(f.renderWidth) / float(f.renderHeight);
    const float ys = 1.0f / std::tan(f.fovY * 0.5f), xs = ys / aspect;
    constants.cameraViewToClip[0] = { xs, 0, 0, 0 };
    constants.cameraViewToClip[1] = { 0, ys, 0, 0 };
    constants.cameraViewToClip[2] = { 0, 0, 1, 1 };
    constants.cameraViewToClip[3] = { 0, 0, -f.nearPlane, 0 };
    constants.clipToCameraView[0] = { 1.0f / xs, 0, 0, 0 };
    constants.clipToCameraView[1] = { 0, 1.0f / ys, 0, 0 };
    constants.clipToCameraView[2] = { 0, 0, 0, -1.0f / f.nearPlane };
    constants.clipToCameraView[3] = { 0, 0, 1, 1.0f / f.nearPlane };
    for (int r = 0; r < 4; r++)
    {
        constants.clipToLensClip[r] = { r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f, r == 2 ? 1.0f : 0.0f, r == 3 ? 1.0f : 0.0f };
        constants.clipToPrevClip[r] = constants.clipToLensClip[r];
        constants.prevClipToClip[r] = constants.clipToLensClip[r];
    }
    if (f.clipToPrevClip)
        for (int r = 0; r < 4; r++)
            constants.clipToPrevClip[r] = { float(f.clipToPrevClip[0 * 4 + r]), float(f.clipToPrevClip[1 * 4 + r]), float(f.clipToPrevClip[2 * 4 + r]), float(f.clipToPrevClip[3 * 4 + r]) };
    constants.jitterOffset = { f.jitterX, f.jitterY };
    constants.mvecScale = { 1.0f, 1.0f };   // already in screen fractions
    constants.cameraPinholeOffset = { 0.0f, 0.0f };
    constants.cameraPos = { 0.0f, 0.0f, 0.0f };
    constants.cameraUp = { 0.0f, 1.0f, 0.0f };
    constants.cameraRight = { 1.0f, 0.0f, 0.0f };
    constants.cameraFwd = { 0.0f, 0.0f, 1.0f };
    constants.cameraNear = f.nearPlane;
    constants.cameraFar = 100000.0f;
    constants.cameraFOV = f.fovY;
    constants.cameraAspectRatio = aspect;
    constants.motionVectorsInvalidValue = 0.0f;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    if (Failed(slSetConstants(constants, *gToken, gViewport), "setting the frame's constants")) return false;

    sl::Resource color(sl::ResourceType::eTex2d, f.color, nullptr, nullptr, f.colorState);
    sl::Resource depth(sl::ResourceType::eTex2d, f.depth, nullptr, nullptr, f.depthState);
    sl::Resource motion(sl::ResourceType::eTex2d, f.motion, nullptr, nullptr, f.motionState);
    sl::Resource output(sl::ResourceType::eTex2d, f.output, nullptr, nullptr, f.outputState);
    const sl::Extent in{ 0, 0, f.renderWidth, f.renderHeight }, out{ 0, 0, f.outWidth, f.outHeight };
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &in),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &in),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &in),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &out),
    };
    if (Failed(slSetTagForFrame(*gToken, gViewport, tags, uint32_t(std::size(tags)), f.list), "tagging the frame's buffers")) return false;
    const sl::BaseStructure* inputs[] = { &gViewport };
    return !Failed(slEvaluateFeature(sl::kFeatureDLSS, *gToken, inputs, 1, f.list), "the DLSS evaluate");
}

#else   // built without the Streamline SDK

void DlssStart() {}
void DlssSetDevice(ID3D12Device*, const void*, uint32_t) {}
void DlssUpgradeInterface(void**) {}
bool DlssAvailable() { return false; }
const char* DlssName() { return "DLSS"; }
bool DlssEvaluate(const DlssFrame&) { return false; }
void DlssRelease() {}

#endif
