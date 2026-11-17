// D3D12 backend. A dedicated thread takes the newest submitted frame and presents it with vsync,
// so waiting for the display never holds up the emulated GPU.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <pdh.h>     // GPU use, for the load readout (SampleLoad)
#include <pdhmsg.h>
#include <psapi.h>
#pragma comment(lib, "pdh.lib")
#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"
#include "renderer.h"
#include "settings.h"
#include <xxHash/xxhash.h>   // shader cache keys
#include <cfloat>
#include <api/include/dx12/ffx_api_dx12.h>   // AMD FSR SDK (tools/get_fsr_sdk.ps1)
#include <upscalers/include/ffx_upscale.h>

using Microsoft::WRL::ComPtr;

namespace
{
    // Textures are read by vertex shaders too (bone matrices for skinning), so "readable" covers
    // every shader stage; with the pixel-shader state alone, a vertex shader could read a texture
    // before an upload into it had finished.
    constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr UINT kBackBuffers = 3;
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    const char kShader[] = R"(
Texture2D<float> yPlane : register(t0);
Texture2D<float> uPlane : register(t1);
Texture2D<float> vPlane : register(t2);
SamplerState linearClamp : register(s0);

struct Interpolated { float4 position : SV_Position; float2 uv : TEXCOORD0; };

Interpolated VSMain(uint id : SV_VertexID)
{
    Interpolated output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}

// BT.601 limited-range conversion.
float4 PSMain(Interpolated input) : SV_Target
{
    float y = (yPlane.Sample(linearClamp, input.uv) - 16.0 / 255.0) * 1.164;
    float u = uPlane.Sample(linearClamp, input.uv) - 128.0 / 255.0;
    float v = vPlane.Sample(linearClamp, input.uv) - 128.0 / 255.0;
    return float4(y + 1.596 * v, y - 0.392 * u - 0.813 * v, y + 2.017 * u, 1.0);
}
)";

    struct MailboxPlane
    {
        std::vector<uint8_t> data;
        uint32_t width = 0, height = 0;
    };

    // Newest frame wins: an undisplayed frame is simply overwritten.
    std::mutex gMailboxMutex;
    std::condition_variable gMailboxChanged, gQueueSpace;
    std::deque<RendererFrame> gFrameQueue;
    std::mutex gCommandListMutex;
    std::vector<std::vector<RendererCommand>> gCommandLists;   // emptied lists, for RendererTakeCommandList
    MailboxPlane gPlanes[3];
    bool gMailboxMovie = false;
    uint64_t gMailboxFrame = 0, gMailboxSequence = 0;
    uint32_t gResizeWidth = 0, gResizeHeight = 0;
    bool gResizePending = false;

    HWND gWindow = nullptr;
    uint32_t gWidth = 1280, gHeight = 720;
    std::atomic<bool> gReady{ false };   // also read by the GPU emulation thread (RendererPreparePipeline)

    ComPtr<ID3D12Device> gDevice;
    ComPtr<ID3D12CommandQueue> gQueue;
    ComPtr<IDXGISwapChain3> gSwapChain;
    UINT gSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;   // + ALLOW_TEARING when supported
    ComPtr<ID3D12DescriptorHeap> gRtvHeap, gSrvHeap;
    ComPtr<ID3D12CommandAllocator> gAllocator;
    ComPtr<ID3D12GraphicsCommandList> gList;
    ComPtr<ID3D12RootSignature> gRootSignature;
    ComPtr<ID3D12PipelineState> gPipeline;
    ComPtr<ID3D12Fence> gFence;
    ComPtr<ID3D12Resource> gBackBuffers[kBackBuffers];
    ComPtr<ID3D12Resource> gTextures[3];
    ComPtr<ID3D12Resource> gUpload, gReadback;
    // Resolves written back to guest memory: copied into this buffer during the frame, handed to the
    // GPU emulation once the frame's work has finished.
    constexpr UINT64 kWriteBackBytes = 8u << 20;
    ComPtr<ID3D12Resource> gWriteBack;
    UINT64 gWriteBackUsed = 0;
    struct PendingWriteBack { RendererResolve resolve; UINT64 offset; UINT rowPitch; UINT hostWidth, hostHeight; };
    std::vector<PendingWriteBack> gPendingWriteBacks;
    UINT64 gUploadCapacity = 0, gReadbackCapacity = 0;
    UINT gRtvStep = 0;
    UINT64 gFenceValue = 0;
    HANDLE gFenceEvent = nullptr, gFrameWait = nullptr;

    std::set<uint64_t> gFramesToSave;
    std::atomic<uint64_t> gRequestedSave{ 0 };   // from other threads; picked up by the render thread
    std::atomic<uint64_t> gSubmitted{ 0 }, gPresented{ 0 }, gMovieImages{ 0 }, gLateImages{ 0 };
    uint64_t gGapBins[8];
    std::atomic<UINT> gDisplayHz{ 60 };   // the refresh rate of the display the window is on
    std::atomic<UINT> gVblankDivider{ 0 };   // display refreshes in a 60 Hz period when the display's rate is a multiple of 60 (and vsync is on); else 0
    uint64_t gLastHash = 0;
    LARGE_INTEGER gLastImage{};
    double gWorstGap = 0;

    bool Check(HRESULT result, const char* what)
    {
        if (SUCCEEDED(result)) return true;
        Log("[d3d12] %s failed: 0x%08lX\n", what, result);
        return false;
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    ComPtr<ID3D12Resource> MakeBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = heap;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        Check(gDevice->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)), "buffer creation");
        return buffer;
    }

    void WaitForGpu()
    {
        const UINT64 value = ++gFenceValue;
        gQueue->Signal(gFence.Get(), value);
        if (gFence->GetCompletedValue() < value)
        {
            gFence->SetEventOnCompletion(value, gFenceEvent);
            WaitForSingleObject(gFenceEvent, INFINITE);
        }
    }

    void AcquireBackBuffers()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; i++)
        {
            gSwapChain->GetBuffer(i, IID_PPV_ARGS(&gBackBuffers[i]));
            gDevice->CreateRenderTargetView(gBackBuffers[i].Get(), nullptr, rtv);
            rtv.ptr += gRtvStep;
        }
    }

    bool CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& out)
    {
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3DCompile(kShader, sizeof(kShader) - 1, "movie", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(result)) Log("[d3d12] shader %s failed: %s\n", entry, errors ? (const char*)errors->GetBufferPointer() : "unknown error");
        return SUCCEEDED(result);
    }

    bool CreatePipeline()
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 3;
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        signature.NumStaticSamplers = 1;
        signature.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> blob, errors;
        if (!Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root signature serialize")) return false;
        if (!Check(gDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gRootSignature)), "root signature")) return false;

        ComPtr<ID3DBlob> vs, ps;
        if (!CompileShader("VSMain", "vs_5_1", vs) || !CompileShader("PSMain", "ps_5_1", ps)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = gRootSignature.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kBackBufferFormat;
        desc.SampleDesc.Count = 1;
        return Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gPipeline)), "pipeline state");
    }

    // ---- Guest draw execution ----
    constexpr UINT64 kArenaBytes = 256ull * 1024 * 1024;
    constexpr UINT kDrawDescriptors = 262144, kTargetSlots = 256;
    constexpr UINT kTextureSlots = 32;   // descriptors per draw: one per fetch constant
    constexpr UINT kSamplerCount = 50;   // 2 filters x 5 x 5 address modes (CreateDrawObjects)
    constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    const char kRectGeometry[] = R"(
struct Interpolated { float4 position : SV_Position; float4 t[16] : TEXCOORD0; };
// A rect list gives three corners. The missing corner opposes the corner at the right angle.
[maxvertexcount(4)]
void main(triangle Interpolated v[3], inout TriangleStream<Interpolated> stream)
{
    float2 p0 = v[0].position.xy, p1 = v[1].position.xy, p2 = v[2].position.xy;
    float d12 = dot(p1 - p2, p1 - p2), d20 = dot(p2 - p0, p2 - p0), d01 = dot(p0 - p1, p0 - p1);
    int corner = d12 >= d20 && d12 >= d01 ? 0 : (d20 >= d01 ? 1 : 2);
    Interpolated a = v[corner], b = v[(corner + 1) % 3], c = v[(corner + 2) % 3], d;
    d.position = b.position + c.position - a.position;
    for (int i = 0; i < 16; i++) d.t[i] = b.t[i] + c.t[i] - a.t[i];
    stream.Append(b);
    stream.Append(a);
    stream.Append(d);
    stream.Append(c);
}
)";

    const char kBlitShader[] = R"(
Texture2D<float4> source : register(t0, space1);
SamplerState linearClamp : register(s0);
struct Interpolated { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Interpolated VSMain(uint id : SV_VertexID)
{
    Interpolated output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
float4 PSMain(Interpolated input) : SV_Target { return float4(source.Sample(linearClamp, input.uv).rgb, 1.0); }
float4 PSCopy(Interpolated input) : SV_Target { return source.Sample(linearClamp, input.uv); }
)";

    // Internal resolution scale: every render target, depth target and resolved surface is this many
    // times the guest's size along each axis, and viewports, scissors and copy rectangles are scaled
    // to match. The guest's own numbers (sizes, keys, rectangles) stay in guest pixels everywhere;
    // shaders address textures by normalized coordinates, so they need no change. Applied between frames
    // (PC Settings > Resolution, or NIER_RES_SCALE). It is kept in twelfths so that every size
    // offered is exact (854x480 is 8/12, 1600x900 is 15/12, 1920x1080 is 18/12); Scaled() converts
    // a guest length or coordinate, rounding to the nearest pixel and never to nothing.
    uint32_t gResTwelfths = 12;
    bool gResolutionLogged = false;
    // The display's own scale (the window against 1280x720) and the scale in force for the command
    // being executed: the render scale, or the display's for the frame's closing 2D pass (see
    // FindFlatPhase). Surfaces of the two scales are separate objects.
    uint32_t gDisplayTwelfths = 12, gActiveTwelfths = 12;
    // Shadow maps have their own scale (PC Settings > Shadow Quality): the game's are 1024x1024 over
    // a wide area, so a character covers a handful of texels and its shadow is a few squares. They
    // are square depth passes that end in a depth copy (see ShadowPasses); the scale applies to
    // their targets and to the resolved depth texture the scene reads.
    uint32_t gShadowTwelfths = 12;
    inline uint32_t Scaled(uint32_t guest) { return guest ? (std::max)((guest * gActiveTwelfths + 6) / 12, 1u) : 0; }
    inline int ScaledInt(int guest) { return guest >= 0 ? int((uint32_t(guest) * gActiveTwelfths + 6) / 12) : -int((uint32_t(-guest) * gActiveTwelfths + 6) / 12); }

    struct TargetResource
    {
        ComPtr<ID3D12Resource> texture;
        UINT rtvIndex = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        uint32_t width = 0, height = 0;
    };
    struct ResolvedResource
    {
        ComPtr<ID3D12Resource> texture;
        uint32_t width = 0, height = 0;
        bool swapRedBlue = false;
        bool depth = false;   // R32_FLOAT depth copy rather than a color copy
        uint64_t version = 0; // bumped by every resolve into it
        uint32_t twelfths = 0;   // the scale it was created at
    };
    // A resolved base plus resolved levels, combined into one mipmapped texture.
    struct ResolvedChain
    {
        ComPtr<ID3D12Resource> texture;
        uint32_t width = 0, height = 0, levels = 0;
        uint32_t hostWidth = 0, hostHeight = 0;
        std::vector<std::pair<ID3D12Resource*, uint64_t>> copied;   // per level: source and version last copied
    };
    std::unordered_map<uint32_t, ResolvedChain> gResolvedChains;
    struct SystemConstants
    {
        float ndcScale[2], ndcOffset[2];
        uint32_t bypassViewport, alphaFunc;
        float alphaRef;
        uint32_t paramGen;
        uint32_t vertexBase[96];
        uint32_t samplerPoint[32];   // per slot: sampler index (shader name samplerIndex)
        float texScale[32][4];
        uint32_t boolConstants[8];
        uint32_t texSign[32];
        float extra[4];   // x: guest pixels for each host pixel
    };
    struct ArenaBlock { uint8_t* cpu; D3D12_GPU_VIRTUAL_ADDRESS gpu; UINT64 offset; };

    ComPtr<ID3D12RootSignature> gDrawRoot;
    ComPtr<ID3D12PipelineState> gBlitPipeline, gTargetCopyPipeline, gOverlayPipeline;
    std::mutex gOverlayMutex;
    std::shared_ptr<DecodedTexture> gOverlay;   // RendererSetOverlay
    ComPtr<ID3D12DescriptorHeap> gDrawSrvHeap, gTargetRtvHeap, gSamplerHeap;

    // The draw root signature plus its sampler table (root parameter 4).
    void BindDrawRoot();
    ComPtr<ID3D12Resource> gArena;
    ComPtr<ID3DBlob> gRectBlob;
    uint8_t* gArenaCpu = nullptr;
    UINT64 gArenaOffset = 0;
    UINT gDrawSrvUsed = 0, gSrvStep = 0, gTargetRtvStep = 0, gTargetsCreated = 0;
    // EDRAM addresses are reused at different sizes within one frame, so a target is keyed by
    // address and size, and nothing is destroyed while the frame's commands may still use it.
    std::unordered_map<uint64_t, TargetResource> gTargets;
    std::vector<ComPtr<ID3D12Resource>> gRetired;

    struct DepthResource { ComPtr<ID3D12Resource> texture; UINT dsvIndex = 0; };
    constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;   // readable as R24_UNORM_X8
    constexpr DXGI_FORMAT kDepthCopyFormat = DXGI_FORMAT_R32_FLOAT;

    // Depth resolves: draw a triangle over the copied rectangle that loads the depth buffer.
    const char kDepthCopyShader[] = R"(
Texture2D<float> depthSource : register(t0, space1);
cbuffer Copy : register(b0) { int2 destOrigin; int2 sourceOrigin; int scale; int3 padding; };
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
float PSMain(float4 position : SV_Position) : SV_Target
{
    int2 p = (int2(position.xy) - destOrigin) * scale + sourceOrigin;
    return depthSource.Load(int3(p, 0));
}
)";
    ComPtr<ID3D12PipelineState> gDepthCopyPipeline;
    ComPtr<ID3D12DescriptorHeap> gDepthCopyRtvHeap;
    std::unordered_map<uint64_t, DepthResource> gDepths;
    ComPtr<ID3D12DescriptorHeap> gDepthDsvHeap;
    UINT gDepthDsvStep = 0, gDepthsCreated = 0;
    std::unordered_map<uint32_t, ResolvedResource> gResolved;
    std::unordered_map<uint32_t, ID3D12Resource*> gUpscaledNow;   // upscaled stand-ins for resolved surfaces, valid for the draw being set up
    // Temporal upscaling (FSR 3.1 / FSR 4, see TemporalUpscaled): state shared with ExecuteDraw.
    // While it is on, every depth-tested draw outside the shadow and 2D passes is shifted by a
    // sub-pixel offset that changes each frame (the jitter), and the first such draw carrying a
    // perspective view-projection matrix in vertex constants c5-c8 gives the frame's camera.
    bool gTemporalOn = false, gInShadowPass = false, gInFlatPass = false;
    float gJitterPixels[2] = {}, gJitterNdc[2] = {};
    double gCameraNow[16] = {}, gCameraBefore[16] = {};
    bool gCameraNowValid = false, gCameraBeforeValid = false;
    uint32_t gSceneDepthBase = 0;   // the frame's latest full-size depth copy (a resolved surface)
    // Scene draws whose vertex shader reads textures (skinning: the bone matrices), noted by
    // ExecuteDraw for the motion vector pass (ObjectMotion), with what it bound for them.
    struct SkinnedDraw
    {
        const RendererDraw* draw = nullptr;
        uint64_t key = 0;   // identifies the same draw in the next frame
        D3D12_GPU_VIRTUAL_ADDRESS system = 0, vertices = 0;
        D3D12_INDEX_BUFFER_VIEW indices{};
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
    };
    std::vector<SkinnedDraw> gSkinnedDraws;
    const std::vector<int>* VertexTextureSlots(const GuestShader& vs);
    struct DecodedResource { ComPtr<ID3D12Resource> texture; uint64_t contentHash = 0; };
    std::unordered_map<uint64_t, DecodedResource> gDecoded;
    std::unordered_map<uint64_t, ID3DBlob*> gShaderBlobs;   // owned by gShaderJobs.results (see ShaderJobs)
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> gPipelines;
    std::atomic<uint64_t> gDrawsExecuted{ 0 }, gDrawsFailed{ 0 };

    ArenaBlock ArenaAlloc(UINT64 size, UINT64 alignment)
    {
        UINT64 begin = (gArenaOffset + alignment - 1) & ~(alignment - 1);
        if (begin + size > kArenaBytes) begin = 0;   // only reached if one frame needs the whole arena
        gArenaOffset = begin + size;
        return { gArenaCpu + begin, gArena->GetGPUVirtualAddress() + begin, begin };
    }

    bool CreateDrawObjects()
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kTextureSlots;
        range.RegisterSpace = 1;   // space 0 t16 is the vertex buffer
        // Samplers: every filter and per-axis address mode combination, in a descriptor table
        // (s0-s49; an indexed sampler array can't use static samplers). A draw picks one per texture
        // slot: filter * 25 + addressU * 5 + addressV, modes in kSamplerAddressModes order, so index 0
        // is linear clamp (what the blit and depth copy shaders use as s0).
        D3D12_DESCRIPTOR_RANGE samplerRange{};
        samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRange.NumDescriptors = kSamplerCount;
        D3D12_ROOT_PARAMETER parameters[5]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2].Descriptor.ShaderRegister = 16;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 1;
        parameters[3].DescriptorTable.pDescriptorRanges = &range;
        parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[4].DescriptorTable.NumDescriptorRanges = 1;
        parameters[4].DescriptorTable.pDescriptorRanges = &samplerRange;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 5;
        signature.pParameters = parameters;
        ComPtr<ID3DBlob> blob, errors;
        if (!Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "draw root signature serialize")) return false;
        if (!Check(gDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gDrawRoot)), "draw root signature")) return false;

        static const D3D12_TEXTURE_ADDRESS_MODE kSamplerAddressModes[5] = { D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE, D3D12_TEXTURE_ADDRESS_MODE_BORDER };
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeap{};
        samplerHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeap.NumDescriptors = kSamplerCount;
        samplerHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(gDevice->CreateDescriptorHeap(&samplerHeap, IID_PPV_ARGS(&gSamplerHeap)), "sampler heap")) return false;
        const UINT samplerStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for (UINT i = 0; i < kSamplerCount; i++)
        {
            D3D12_SAMPLER_DESC sampler{};
            sampler.Filter = i < 25 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = kSamplerAddressModes[(i % 25) / 5];
            sampler.AddressV = kSamplerAddressModes[i % 5];
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE handle = gSamplerHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(i) * samplerStep;
            gDevice->CreateSampler(&sampler, handle);
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvHeap{};
        srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeap.NumDescriptors = kDrawDescriptors;
        srvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(gDevice->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&gDrawSrvHeap)), "draw SRV heap")) return false;
        gSrvStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
        rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeap.NumDescriptors = kTargetSlots;
        if (!Check(gDevice->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&gTargetRtvHeap)), "target RTV heap")) return false;
        gTargetRtvStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{};
        dsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeap.NumDescriptors = kTargetSlots;
        if (!Check(gDevice->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(&gDepthDsvHeap)), "depth heap")) return false;
        gDepthDsvStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        gArena = MakeBuffer(kArenaBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        gArena->Map(0, nullptr, (void**)&gArenaCpu);

        ComPtr<ID3DBlob> shaderErrors;
        if (FAILED(D3DCompile(kRectGeometry, sizeof(kRectGeometry) - 1, "rect", nullptr, nullptr, "main", "gs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &gRectBlob, &shaderErrors)))
        {
            Log("[d3d12] rect shader failed: %s\n", shaderErrors ? (const char*)shaderErrors->GetBufferPointer() : "unknown");
            return false;
        }

        ComPtr<ID3DBlob> vs, ps;
        if (FAILED(D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "blit", nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, &shaderErrors))) return false;
        if (FAILED(D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "blit", nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps, &shaderErrors))) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = gDrawRoot.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kBackBufferFormat;
        desc.SampleDesc.Count = 1;
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gBlitPipeline)), "blit pipeline")) return false;
        {
            // The same triangle into a render target, all four channels (the 2D pass's starting picture).
            ComPtr<ID3DBlob> copy;
            if (FAILED(D3DCompile(kBlitShader, sizeof(kBlitShader) - 1, "blit", nullptr, nullptr, "PSCopy", "ps_5_1", 0, 0, &copy, &shaderErrors))) return false;
            desc.PS = { copy->GetBufferPointer(), copy->GetBufferSize() };
            desc.RTVFormats[0] = kTargetFormat;
            if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gTargetCopyPipeline)), "target copy pipeline")) return false;
            // And onto the back buffer with alpha blending (the overlay).
            desc.RTVFormats[0] = kBackBufferFormat;
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gOverlayPipeline)), "overlay pipeline")) return false;
            blend.BlendEnable = FALSE;
        }

        ComPtr<ID3DBlob> copyVs, copyPs;
        if (FAILED(D3DCompile(kDepthCopyShader, sizeof(kDepthCopyShader) - 1, "depthcopy", nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &copyVs, &shaderErrors)) ||
            FAILED(D3DCompile(kDepthCopyShader, sizeof(kDepthCopyShader) - 1, "depthcopy", nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &copyPs, &shaderErrors)))
        {
            Log("[d3d12] depth copy shader failed: %s\n", shaderErrors ? (const char*)shaderErrors->GetBufferPointer() : "unknown");
            return false;
        }
        desc.VS = { copyVs->GetBufferPointer(), copyVs->GetBufferSize() };
        desc.PS = { copyPs->GetBufferPointer(), copyPs->GetBufferSize() };
        desc.RTVFormats[0] = kDepthCopyFormat;
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gDepthCopyPipeline)), "depth copy pipeline")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC copyRtvHeap{};
        copyRtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        copyRtvHeap.NumDescriptors = 1;
        return Check(gDevice->CreateDescriptorHeap(&copyRtvHeap, IID_PPV_ARGS(&gDepthCopyRtvHeap)), "depth copy RTV heap");
    }

    // Guest shaders compile on worker threads as soon as the GPU emulation first sees them
    // (RendererPrepareShader), usually frames before a draw needs them, and the DXBC is cached on disk
    // (recomp/shadercache, keyed by the HLSL's hash). Compiling on the render thread at first use made
    // hitches of 100-500 ms whenever a new effect or enemy appeared.
    ComPtr<ID3DBlob> CompileGuestShader(const GuestShader& shader)
    {
        const char* profile = shader.pixel ? "ps_5_1" : "vs_5_1";
        // Unoptimized: FXC's optimization passes made compiles ~2.5x slower, and the driver optimizes the
        // shader again when it builds the pipeline. The flags are part of the cache key.
        // NIER_SHADER_SALT=1 (testing) adds IEEE strictness: different bytecode, so neither this cache nor
        // the driver's has seen it, i.e. a first-run experience.
        static const UINT kFlags = (GetEnvironmentVariableA("NIER_SHADER_OPTIMIZE", nullptr, 0) > 0 ? D3DCOMPILE_OPTIMIZATION_LEVEL1 : D3DCOMPILE_SKIP_OPTIMIZATION) |
                                   (GetEnvironmentVariableA("NIER_SHADER_SALT", nullptr, 0) > 0 ? D3DCOMPILE_IEEE_STRICTNESS : 0);
        const uint64_t key = XXH3_64bits_withSeed(shader.hlsl.data(), shader.hlsl.size(), (shader.pixel ? 1 : 2) + uint64_t(kFlags) * 16);
        char path[128];
        snprintf(path, sizeof(path), "..\\shadercache\\%s_%016llX_%016llX.dxbc", shader.pixel ? "ps" : "vs", (unsigned long long)shader.hash, (unsigned long long)key);
        ComPtr<ID3DBlob> blob;
        if (FILE* file = fopen(path, "rb"))
        {
            fseek(file, 0, SEEK_END);
            const long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (size > 0 && SUCCEEDED(D3DCreateBlob(size_t(size), &blob)) && fread(blob->GetBufferPointer(), 1, size_t(size), file) == size_t(size))
            {
                fclose(file);
                return blob;
            }
            fclose(file);
            blob.Reset();
        }
        ComPtr<ID3DBlob> errors;
        HRESULT result = D3DCompile(shader.hlsl.data(), shader.hlsl.size(), "guest", nullptr, nullptr, "main", profile, kFlags, 0, &blob, &errors);
        if (FAILED(result))
        {
            Log("[shader] %s %016llX failed to compile: %s\n", shader.pixel ? "pixel" : "vertex", shader.hash, errors ? (const char*)errors->GetBufferPointer() : "unknown");
            return nullptr;
        }
        CreateDirectoryA("..\\shadercache", nullptr);
        if (FILE* file = fopen(path, "wb"))
        {
            fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), file);
            fclose(file);
        }
        return blob;
    }

    // A compiled shader's blob is held by its entry in `results` for the whole run, and everyone else
    // uses the plain pointer. A d3dcompiler blob's reference count is NOT safe to change from two
    // threads at once (measured: balanced AddRef/Release from 8 threads corrupts the heap), and the
    // pipeline workers each took and dropped a reference to the shaders they shared: now and then a
    // count came out one short, the blob was freed under them, and the game crashed while preparing
    // shaders at start-up (seen once, in a playtest).
    struct ShaderJobs
    {
        std::mutex mutex;
        std::condition_variable work;
        std::deque<std::shared_ptr<const GuestShader>> queue;
        std::unordered_map<uint64_t, std::shared_future<ComPtr<ID3DBlob>>> results;   // by guest hash
        std::unordered_map<uint64_t, std::shared_ptr<std::promise<ComPtr<ID3DBlob>>>> promises;
        bool started = false;
        bool backlog = false;   // ahead-of-time jobs were queued (urgent requests then look for theirs)
    } gShaderJobs;

    // While the game waits for the pipeline list at start-up (PrewarmWait) nothing else needs the
    // processor: extra workers (`temporary`) help until that is over (RendererPrewarmBoost).
    std::atomic<bool> gPrewarmBoost{ false };
    std::atomic<uint32_t> gPipelineJobsQueued{ 0 }, gPipelineJobsDone{ 0 }, gShaderJobsQueued{ 0 }, gShaderJobsDone{ 0 };

    void ShaderWorker(bool temporary)
    {
        while (true)
        {
            std::shared_ptr<const GuestShader> shader;
            std::shared_ptr<std::promise<ComPtr<ID3DBlob>>> promise;
            {
                std::unique_lock lock(gShaderJobs.mutex);
                gShaderJobs.work.wait(lock, [temporary] { return !gShaderJobs.queue.empty() || (temporary && !gPrewarmBoost); });
                if (gShaderJobs.queue.empty()) return;
                shader = gShaderJobs.queue.front();
                gShaderJobs.queue.pop_front();
                promise = gShaderJobs.promises[shader->hash];
                gShaderJobs.promises.erase(shader->hash);
            }
            promise->set_value(CompileGuestShader(*shader));
            gShaderJobsDone++;
        }
    }

    // Starts compiling a shader unless it is already compiled or queued; returns its result. What the
    // game needs now (`urgent`) goes ahead of the pipeline list's ahead-of-time work, and pulls its
    // own job forward if that work had queued it already. A job somebody is about to wait for
    // (`waiting`: a pipeline's worker, or for a pipeline the render thread) goes to the very front:
    // urgent jobs are taken newest first, so during a burst (a level's first frames) the one a
    // frame was held for could lie under dozens queued after it.
    std::shared_future<ComPtr<ID3DBlob>> QueueShader(const std::shared_ptr<const GuestShader>& shader, bool urgent = true, bool waiting = false)
    {
        std::lock_guard lock(gShaderJobs.mutex);
        if (!gShaderJobs.started)
        {
            gShaderJobs.started = true;
            const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 4, 2u, 6u);
            for (unsigned i = 0; i < workers; i++) std::thread(ShaderWorker, false).detach();
        }
        auto found = gShaderJobs.results.find(shader->hash);
        if (found != gShaderJobs.results.end())
        {
            if (waiting || (urgent && gShaderJobs.backlog))
                for (auto queued = gShaderJobs.queue.begin(); queued != gShaderJobs.queue.end(); ++queued)
                    if ((*queued)->hash == shader->hash)
                    {
                        auto job = *queued;
                        gShaderJobs.queue.erase(queued);
                        gShaderJobs.queue.push_front(job);
                        break;
                    }
            return found->second;
        }
        auto promise = std::make_shared<std::promise<ComPtr<ID3DBlob>>>();
        std::shared_future<ComPtr<ID3DBlob>> future = promise->get_future().share();
        gShaderJobs.results[shader->hash] = future;
        gShaderJobs.promises[shader->hash] = promise;
        gShaderJobsQueued++;
        if (urgent) gShaderJobs.queue.push_front(shader);
        else { gShaderJobs.queue.push_back(shader); gShaderJobs.backlog = true; }
        gShaderJobs.work.notify_one();
        return future;
    }

    // Render-thread time spent waiting for shader compiles and creating pipelines (RendererDumpStats).
    std::atomic<double> gShaderWaitMs{ 0 }, gShaderWaitWorstMs{ 0 }, gPipelineMs{ 0 }, gPipelineWorstMs{ 0 };
    std::atomic<uint64_t> gShaderWaits{ 0 }, gPipelineSlow{ 0 };

    ID3DBlob* ShaderBlob(const std::shared_ptr<GuestShader>& shader)
    {
        auto found = gShaderBlobs.find(shader->hash);
        if (found != gShaderBlobs.end()) return found->second;
        std::shared_future<ComPtr<ID3DBlob>> future = QueueShader(shader);
        LARGE_INTEGER before, after, frequency;
        QueryPerformanceCounter(&before);
        ID3DBlob* blob = future.get().Get();   // usually ready by now
        QueryPerformanceCounter(&after);
        QueryPerformanceFrequency(&frequency);
        const double waited = double(after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart;
        gShaderWaitMs += waited;
        gShaderWaitWorstMs = std::max(gShaderWaitWorstMs.load(), waited);
        if (waited > 1.0) gShaderWaits++;
        gShaderBlobs[shader->hash] = blob;
        return blob;
    }

    D3D12_BLEND BlendFactor(uint32_t value)
    {
        static const D3D12_BLEND kFactors[] = {
            D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA,
            D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC_ALPHA_SAT };
        return value <= 16 ? kFactors[value] : D3D12_BLEND_ONE;
    }

    D3D12_BLEND_OP BlendOp(uint32_t value)
    {
        static const D3D12_BLEND_OP kOps[] = { D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT };
        return value <= 4 ? kOps[value] : D3D12_BLEND_OP_ADD;
    }

    // What a pipeline is built from: the shaders and the draw's fixed-function state.
    struct PipelineState
    {
        std::shared_ptr<GuestShader> vs, ps;
        uint32_t blendControl = 0, writeMask = 0, depthControl = 0, stencilRefMask = 0;
        bool rectList = false, hasColor = false, depthBound = false;
        uint8_t cull = 0;
        bool frontCounterClockwise = true;
    };

    PipelineState PipelineStateOf(const RendererDraw& draw)
    {
        return { draw.vs, draw.ps, draw.blendControl, draw.writeMask, draw.depthControl, draw.stencilRefMask, draw.rectList, draw.hasColor, draw.depthBound,
                 draw.cull, draw.frontCounterClockwise };
    }

    uint64_t PipelineKey(const PipelineState& s)
    {
        uint64_t key = s.vs->hash * 31 + s.ps->hash;
        key = (key ^ s.blendControl) * 1099511628211ull;
        key = (key ^ (uint64_t(s.writeMask) << 1) ^ (s.rectList ? 1 : 0)) * 1099511628211ull;
        key = (key ^ s.depthControl ^ (uint64_t(s.stencilRefMask & 0xFFFF00) << 32) ^ (uint64_t(s.hasColor) << 60) ^ (uint64_t(s.depthBound) << 61)) * 1099511628211ull;
        key = (key ^ s.cull ^ (s.frontCounterClockwise ? 4 : 0)) * 1099511628211ull;
        return key;
    }

    ComPtr<ID3D12PipelineState> CreatePipeline(const PipelineState& draw);

    // Pipelines are created on their own worker threads (they wait for shader compiles, so sharing the
    // shader workers could deadlock), started by the GPU emulation when it first sees a combination
    // (RendererPreparePipeline), a frame or more before the render thread needs it. Creating them on
    // the render thread at first use made the driver's first compile of a new shader a visible hitch.
    struct PipelineJobs
    {
        std::mutex mutex;
        std::condition_variable work;
        std::deque<std::pair<uint64_t, PipelineState>> queue;
        std::unordered_map<uint64_t, std::shared_future<ComPtr<ID3D12PipelineState>>> results;
        std::unordered_map<uint64_t, std::shared_ptr<std::promise<ComPtr<ID3D12PipelineState>>>> promises;
        bool started = false;
        bool backlog = false;
    } gPipelineJobs;
    std::atomic<double> gPipelineWaitMs{ 0 };
    std::atomic<uint64_t> gPipelineWaits{ 0 };

    void PipelineWorker(bool temporary)
    {
        while (true)
        {
            std::pair<uint64_t, PipelineState> job;
            std::shared_ptr<std::promise<ComPtr<ID3D12PipelineState>>> promise;
            {
                std::unique_lock lock(gPipelineJobs.mutex);
                gPipelineJobs.work.wait(lock, [temporary] { return !gPipelineJobs.queue.empty() || (temporary && !gPrewarmBoost); });
                if (gPipelineJobs.queue.empty()) return;
                job = std::move(gPipelineJobs.queue.front());
                gPipelineJobs.queue.pop_front();
                promise = gPipelineJobs.promises[job.first];
                gPipelineJobs.promises.erase(job.first);
            }
            static const DWORD delay = [] { char value[16]; return GetEnvironmentVariableA("NIER_PIPELINE_DELAY", value, sizeof(value)) ? DWORD(atoi(value)) : 0; }();
            if (delay) Sleep(delay);
            promise->set_value(CreatePipeline(job.second));
            gPipelineJobsDone++;
        }
    }

    std::shared_future<ComPtr<ID3D12PipelineState>> QueuePipeline(const PipelineState& state, uint64_t key, bool urgent = true, bool waiting = false)
    {
        std::lock_guard lock(gPipelineJobs.mutex);
        if (!gPipelineJobs.started)
        {
            gPipelineJobs.started = true;
            const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 6, 2u, 4u);
            for (unsigned i = 0; i < workers; i++) std::thread(PipelineWorker, false).detach();
        }
        auto found = gPipelineJobs.results.find(key);
        if (found != gPipelineJobs.results.end())
        {
            if (waiting || (urgent && gPipelineJobs.backlog))
                for (auto queued = gPipelineJobs.queue.begin(); queued != gPipelineJobs.queue.end(); ++queued)
                    if (queued->first == key)
                    {
                        auto job = std::move(*queued);
                        gPipelineJobs.queue.erase(queued);
                        gPipelineJobs.queue.push_front(std::move(job));
                        break;
                    }
            return found->second;
        }
        auto promise = std::make_shared<std::promise<ComPtr<ID3D12PipelineState>>>();
        std::shared_future<ComPtr<ID3D12PipelineState>> future = promise->get_future().share();
        gPipelineJobs.results[key] = future;
        gPipelineJobs.promises[key] = promise;
        gPipelineJobsQueued++;
        if (urgent) gPipelineJobs.queue.emplace_front(key, state);
        else { gPipelineJobs.queue.emplace_back(key, state); gPipelineJobs.backlog = true; }
        gPipelineJobs.work.notify_one();
        return future;
    }

    // A pipeline that is not ready when the render thread reaches its draw (a combination met for
    // the first time, that neither the pipeline list nor the GPU emulation's head start covered)
    // used to hold the whole frame until the driver had built it. Now a draw of scene geometry is
    // left out until its pipeline is there: the object appears a frame or a few late and the
    // frames stay on time. Only draws that test against a depth buffer and have more vertices
    // than a quad are left out. Full-screen passes, the resolves' sources the game reads back
    // and the 2D layer still wait: without one of those the whole picture is wrong (black, or
    // an exposure measured from nothing), not one object. NIER_SYNC_PIPELINES=1: wait for every
    // pipeline, as before. Frame replays always wait (RendererSetSynchronousPipelines), so a
    // recorded frame is drawn whole. NIER_PIPELINE_DELAY=<ms> (testing) makes every pipeline
    // take that much longer on its worker.
    bool gSynchronousPipelines = false;
    struct DeferredPipeline { ULONGLONG since = 0; uint32_t draws = 0; };
    std::unordered_map<uint64_t, DeferredPipeline> gDeferredPipelines;   // render thread only
    std::atomic<uint64_t> gDrawsDeferred{ 0 }, gPipelinesDeferred{ 0 };

    bool MayDeferDraw(const RendererDraw& draw)
    {
        static const bool always = GetEnvironmentVariableA("NIER_SYNC_PIPELINES", nullptr, 0) > 0;
        if (always || gSynchronousPipelines) return false;
        const size_t vertices = (std::max)(size_t(draw.vertexCount), draw.indices.size());
        return !draw.rectList && draw.depthBound && ((draw.depthControl >> 1) & 1) && vertices > 6;
    }

    // `deferred` is set when the pipeline is still being built and the draw may be left out.
    ID3D12PipelineState* PipelineFor(const RendererDraw& draw, bool* deferred)
    {
        const PipelineState state = PipelineStateOf(draw);
        const uint64_t key = PipelineKey(state);
        auto found = gPipelines.find(key);
        if (found != gPipelines.end()) return found->second.Get();
        std::shared_future<ComPtr<ID3D12PipelineState>> future = QueuePipeline(state, key);
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready && MayDeferDraw(draw))
        {
            DeferredPipeline& d = gDeferredPipelines[key];
            if (!d.draws) { d.since = GetTickCount64(); gPipelinesDeferred++; }
            d.draws++;
            gDrawsDeferred++;
            *deferred = true;
            return nullptr;
        }
        if (auto was = gDeferredPipelines.find(key); was != gDeferredPipelines.end())
        {
            Log("[pipeline] vs %016llX ps %016llX: ready after %llu ms, %u draws left out meanwhile\n", (unsigned long long)state.vs->hash, (unsigned long long)state.ps->hash,
                GetTickCount64() - was->second.since, was->second.draws);
            gDeferredPipelines.erase(was);
        }
        LARGE_INTEGER before, after, frequency;
        QueryPerformanceCounter(&before);
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) QueuePipeline(state, key, true, true);   // the frame waits for this one: ahead of all others
        ComPtr<ID3D12PipelineState> pipeline = future.get();   // usually created in the background already
        QueryPerformanceCounter(&after);
        QueryPerformanceFrequency(&frequency);
        const double waited = double(after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart;
        gPipelineWaitMs += waited;
        if (waited > 1.0) gPipelineWaits++;
        // Which draws still hold a frame, and why they could not be left out.
        if (waited > 5.0)
            Log("[pipeline] vs %016llX ps %016llX: the frame waited %.0f ms (%s%s depth control %X, %zu vertices, target %ux%u)\n", (unsigned long long)state.vs->hash,
                (unsigned long long)state.ps->hash, waited, draw.rectList ? "rectangles, " : "", draw.depthBound ? "depth bound," : "no depth,", draw.depthControl,
                (std::max)(size_t(draw.vertexCount), draw.indices.size()), draw.hasColor ? draw.target.width : 0, draw.hasColor ? draw.target.height : 0);
        gPipelines[key] = pipeline;
        return pipeline.Get();
    }

    // Creates a pipeline (on a worker thread): waits for its shaders' compiles, then lets the driver build it.
    ComPtr<ID3D12PipelineState> CreatePipeline(const PipelineState& draw)
    {
        std::shared_future<ComPtr<ID3DBlob>> vsFuture = QueueShader(draw.vs, true, true), psFuture = QueueShader(draw.ps, true, true);   // both started before waiting
        ID3DBlob* vs = vsFuture.get().Get(), * ps = psFuture.get().Get();   // no reference taken: see ShaderJobs
        ComPtr<ID3D12PipelineState> pipeline;
        if (vs && ps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = gDrawRoot.Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            if (draw.rectList) desc.GS = { gRectBlob->GetBufferPointer(), gRectBlob->GetBufferSize() };
            auto& target = desc.BlendState.RenderTarget[0];
            const uint32_t b = draw.blendControl;
            target.SrcBlend = BlendFactor(b & 0x1F);
            target.BlendOp = BlendOp((b >> 5) & 7);
            target.DestBlend = BlendFactor((b >> 8) & 0x1F);
            target.SrcBlendAlpha = BlendFactor((b >> 16) & 0x1F);
            target.BlendOpAlpha = BlendOp((b >> 21) & 7);
            target.DestBlendAlpha = BlendFactor((b >> 24) & 0x1F);
            target.BlendEnable = !(target.SrcBlend == D3D12_BLEND_ONE && target.DestBlend == D3D12_BLEND_ZERO && target.BlendOp == D3D12_BLEND_OP_ADD &&
                target.SrcBlendAlpha == D3D12_BLEND_ONE && target.DestBlendAlpha == D3D12_BLEND_ZERO && target.BlendOpAlpha == D3D12_BLEND_OP_ADD);
            target.RenderTargetWriteMask = UINT8(draw.writeMask);
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = draw.rectList || draw.cull == 0 ? D3D12_CULL_MODE_NONE : draw.cull == 1 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
            desc.RasterizerState.FrontCounterClockwise = draw.frontCounterClockwise;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;   // strips are drawn as strips (RendererDraw::strip)
            desc.NumRenderTargets = draw.hasColor ? 1 : 0;
            desc.RTVFormats[0] = draw.hasColor ? kTargetFormat : DXGI_FORMAT_UNKNOWN;
            if (draw.depthBound)
            {
                // RB_DEPTHCONTROL: stencil 0, z 1, z write 2, z func 4-6, back-face stencil 7,
                // front stencil func/fail/pass/zfail 8-19, back-face 20-31. Xenos enums are D3D12's minus one.
                const uint32_t d = draw.depthControl;
                auto& ds = desc.DepthStencilState;
                desc.DSVFormat = kDepthFormat;
                ds.DepthEnable = (d >> 1) & 1;
                ds.DepthWriteMask = (((d >> 1) & 1) && ((d >> 2) & 1)) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                ds.DepthFunc = D3D12_COMPARISON_FUNC(((d >> 4) & 7) + 1);
                ds.StencilEnable = d & 1;
                ds.StencilReadMask = UINT8(draw.stencilRefMask >> 8);
                ds.StencilWriteMask = UINT8(draw.stencilRefMask >> 16);
                auto face = [](uint32_t func, uint32_t fail, uint32_t pass, uint32_t depthFail) {
                    D3D12_DEPTH_STENCILOP_DESC op;
                    op.StencilFunc = D3D12_COMPARISON_FUNC(func + 1);
                    op.StencilFailOp = D3D12_STENCIL_OP(fail + 1);
                    op.StencilPassOp = D3D12_STENCIL_OP(pass + 1);
                    op.StencilDepthFailOp = D3D12_STENCIL_OP(depthFail + 1);
                    return op;
                };
                ds.FrontFace = face((d >> 8) & 7, (d >> 11) & 7, (d >> 14) & 7, (d >> 17) & 7);
                ds.BackFace = ((d >> 7) & 1) ? face((d >> 20) & 7, (d >> 23) & 7, (d >> 26) & 7, (d >> 29) & 7) : ds.FrontFace;
            }
            desc.SampleDesc.Count = 1;
            LARGE_INTEGER before, after, frequency;
            QueryPerformanceCounter(&before);
            Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "guest pipeline");
            QueryPerformanceCounter(&after);
            QueryPerformanceFrequency(&frequency);
            const double ms = double(after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart;
            gPipelineMs += ms;
            gPipelineWorstMs = std::max(gPipelineWorstMs.load(), ms);
            if (ms > 5.0) gPipelineSlow++;
        }
        return pipeline;
    }

    TargetResource* GetTarget(const RenderTargetKey& key)
    {
        if (!key.width || !key.height) return nullptr;
        const uint64_t mapKey = uint64_t(key.edramBase) | (uint64_t(key.width) << 16) | (uint64_t(key.height) << 40) | (uint64_t(gActiveTwelfths) << 56);
        auto found = gTargets.find(mapKey);
        if (found != gTargets.end()) return &found->second;

        TargetResource created;
        created.rtvIndex = gTargetsCreated++;
        if (created.rtvIndex >= kTargetSlots) return nullptr;
        created.width = key.width;
        created.height = key.height;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = Scaled(key.width);
        desc.Height = Scaled(key.height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kTargetFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kTargetFormat;
        if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&created.texture)), "render target")) return nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gTargetRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(created.rtvIndex) * gTargetRtvStep;
        gDevice->CreateRenderTargetView(created.texture.Get(), nullptr, rtv);
        // A new texture holds whatever was in that memory. Passes that feed on their own last result
        // (the exposure adaptation) would take it for a picture: after a resolution change the scene
        // could stay nearly black. Start from zero instead.
        const FLOAT zero[4] = { 0, 0, 0, 0 };
        static const bool keepGarbage = GetEnvironmentVariableA("NIER_NO_TARGET_CLEAR", nullptr, 0) > 0;   // debugging: as before the fix
        if (!keepGarbage) gList->ClearRenderTargetView(rtv, zero, 0, nullptr);
        Log("[d3d12] render target at EDRAM 0x%X: %ux%u\n", key.edramBase, key.width, key.height);
        return &(gTargets[mapKey] = std::move(created));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DepthView(const DepthResource& depth)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = gDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(depth.dsvIndex) * gDepthDsvStep;
        return dsv;
    }

    // Depth targets follow the same address-and-size keying as color targets.
    DepthResource* GetDepth(const RenderTargetKey& key)
    {
        if (!key.width || !key.height) return nullptr;
        const uint64_t mapKey = uint64_t(key.edramBase) | (uint64_t(key.width) << 16) | (uint64_t(key.height) << 40) | (uint64_t(gActiveTwelfths) << 56);
        auto found = gDepths.find(mapKey);
        if (found != gDepths.end()) return &found->second;
        if (gDepthsCreated >= kTargetSlots) return nullptr;

        DepthResource created;
        created.dsvIndex = gDepthsCreated++;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = Scaled(key.width);
        desc.Height = Scaled(key.height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kDepthResourceFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&created.texture)), "depth target")) return nullptr;
        DepthResource& stored = gDepths[mapKey] = std::move(created);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = kDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        gDevice->CreateDepthStencilView(stored.texture.Get(), &dsvDesc, DepthView(stored));
        gList->ClearDepthStencilView(DepthView(stored), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        Log("[d3d12] depth target at EDRAM 0x%X: %ux%u\n", key.edramBase, key.width, key.height);
        return &stored;
    }

    void SetTargetState(TargetResource& target, D3D12_RESOURCE_STATES state)
    {
        if (target.state == state) return;
        auto barrier = Transition(target.texture.Get(), target.state, state);
        gList->ResourceBarrier(1, &barrier);
        target.state = state;
    }

    DXGI_FORMAT DecodedFormat(GuestTextureFormat format)
    {
        switch (format)
        {
        case GuestTextureFormat::BC1: return DXGI_FORMAT_BC1_UNORM;
        case GuestTextureFormat::BC2: return DXGI_FORMAT_BC2_UNORM;
        case GuestTextureFormat::BC3: return DXGI_FORMAT_BC3_UNORM;
        case GuestTextureFormat::R8: return DXGI_FORMAT_R8_UNORM;
        case GuestTextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case GuestTextureFormat::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case GuestTextureFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
        case GuestTextureFormat::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case GuestTextureFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    ID3D12Resource* GetDecoded(const DecodedTexture& decoded, std::unordered_map<uint64_t, DecodedResource>& cache = gDecoded)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = decoded.width;
        desc.Height = decoded.height;
        desc.DepthOrArraySize = UINT16(decoded.faces);
        desc.MipLevels = UINT16(decoded.levels);
        desc.Format = DecodedFormat(decoded.format);
        desc.SampleDesc.Count = 1;

        // One host texture per guest texture; new contents are uploaded into it, in command order,
        // so earlier draws in the frame still see the old contents.
        ComPtr<ID3D12Resource> texture;
        auto found = cache.find(decoded.key);
        if (found != cache.end())
        {
            if (found->second.contentHash == decoded.contentHash) return found->second.texture.Get();
            const D3D12_RESOURCE_DESC existing = found->second.texture->GetDesc();
            if (existing.Width == desc.Width && existing.Height == desc.Height && existing.DepthOrArraySize == desc.DepthOrArraySize && existing.Format == desc.Format && existing.MipLevels == desc.MipLevels)
            {
                texture = found->second.texture;
                auto toCopy = Transition(texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
                gList->ResourceBarrier(1, &toCopy);
            }
            else gRetired.push_back(std::move(found->second.texture));
        }
        if (!texture)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "guest texture")) return nullptr;
        }

        const UINT subresources = UINT(decoded.subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
        std::vector<UINT> rows(subresources);
        UINT64 total = 0;
        gDevice->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), nullptr, &total);
        ArenaBlock block = ArenaAlloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        for (UINT index = 0; index < subresources; index++)
        {
            const DecodedTexture::Subresource& sub = decoded.subresources[index];
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprints[index];
            const UINT copyRows = std::min(rows[index], sub.rows);
            const UINT copyBytes = std::min(UINT(footprint.Footprint.RowPitch), sub.rowPitch);
            for (UINT row = 0; row < copyRows; row++)
                memcpy(block.cpu + footprint.Offset + UINT64(row) * footprint.Footprint.RowPitch, decoded.data.data() + sub.offset + size_t(row) * sub.rowPitch, copyBytes);
            footprint.Offset += block.offset;
            D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            source.pResource = gArena.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            gList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        auto barrier = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
        gList->ResourceBarrier(1, &barrier);
        cache[decoded.key] = { texture, decoded.contentHash };
        return texture.Get();
    }

    // Builds (or refreshes) the mipmapped texture for a resolved base with resolved levels. Levels whose
    // resolve surface changed since the last copy are copied again, in command order.
    ID3D12Resource* ResolvedChainFor(const TextureBinding& binding, ResolvedResource& base, UINT& levelsOut)
    {
        std::vector<ResolvedResource*> sources{ &base };
        for (uint32_t i = 0; i < binding.resolvedLevelCount; i++)
        {
            auto level = gResolved.find(binding.resolvedLevels[i]);
            if (level == gResolved.end() || level->second.depth || !level->second.texture) break;
            sources.push_back(&level->second);
        }
        const UINT levels = UINT(sources.size());
        if (levels < 2) return nullptr;
        ResolvedChain& chain = gResolvedChains[binding.resolvedBase];
        const D3D12_RESOURCE_DESC baseDesc = base.texture->GetDesc();
        if (!chain.texture || chain.width != base.width || chain.height != base.height || chain.levels != levels || chain.hostWidth != baseDesc.Width || chain.hostHeight != baseDesc.Height)
        {
            if (chain.texture) gRetired.push_back(std::move(chain.texture));
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = baseDesc.Width;
            desc.Height = baseDesc.Height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = UINT16(levels);
            desc.Format = kTargetFormat;
            desc.SampleDesc.Count = 1;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kShaderRead, nullptr, IID_PPV_ARGS(&chain.texture)), "resolved mip chain")) return nullptr;
            chain.width = base.width;
            chain.height = base.height;
            chain.hostWidth = UINT(baseDesc.Width);
            chain.hostHeight = UINT(baseDesc.Height);
            chain.levels = levels;
            chain.copied.assign(levels, { nullptr, 0 });
        }
        bool started = false;
        for (UINT level = 0; level < levels; level++)
        {
            ResolvedResource& source = *sources[level];
            if (chain.copied[level].first == source.texture.Get() && chain.copied[level].second == source.version) continue;
            if (!started)
            {
                auto toCopy = Transition(chain.texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
                gList->ResourceBarrier(1, &toCopy);
                started = true;
            }
            const UINT levelWidth = std::max(chain.width >> level, 1u), levelHeight = std::max(chain.height >> level, 1u);
            // In host pixels: the chain's level and the resolved surface can differ by a pixel when the
            // scale isn't whole.
            const D3D12_RESOURCE_DESC sourceDesc = source.texture->GetDesc();
            const UINT hostLevelWidth = std::max(chain.hostWidth >> level, 1u), hostLevelHeight = std::max(chain.hostHeight >> level, 1u);
            D3D12_BOX box{ 0, 0, 0, std::min(hostLevelWidth, UINT(sourceDesc.Width)), std::min(hostLevelHeight, UINT(sourceDesc.Height)), 1 };
            auto toSource = Transition(source.texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gList->ResourceBarrier(1, &toSource);
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            to.pResource = chain.texture.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = level;
            from.pResource = source.texture.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            gList->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            auto back = Transition(source.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kShaderRead);
            gList->ResourceBarrier(1, &back);
            chain.copied[level] = { source.texture.Get(), source.version };
        }
        if (started)
        {
            auto toRead = Transition(chain.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
            gList->ResourceBarrier(1, &toRead);
        }
        levelsOut = levels;
        return chain.texture.Get();
    }

    void WriteTextureDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const TextureBinding& binding)
    {
        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool swap = false, depthCopy = false;
        UINT chainLevels = 1;
        if (binding.valid && binding.decoded)
        {
            resource = GetDecoded(*binding.decoded);
            format = DecodedFormat(binding.decoded->format);
        }
        else if (binding.valid)
        {
            auto found = gResolved.find(binding.resolvedBase);
            if (found != gResolved.end())
            {
                resource = found->second.texture.Get();
                if (!gUpscaledNow.empty())   // FSR 1: the display-sized, sharpened stand-in
                {
                    auto upscaled = gUpscaledNow.find(binding.resolvedBase);
                    if (upscaled != gUpscaledNow.end()) resource = upscaled->second;
                }
                swap = found->second.swapRedBlue;
                depthCopy = found->second.depth;
                if (depthCopy) format = kDepthCopyFormat;
                if (!depthCopy && binding.resolvedLevelCount)
                    if (ID3D12Resource* chain = ResolvedChainFor(binding, found->second, chainLevels)) resource = chain;
            }
        }
        // The view must match how the shader declares the slot; a texture of the other kind binds nothing.
        const bool isCube = binding.valid && binding.decoded && binding.decoded->faces == 6;
        if (resource && isCube != binding.cube) resource = nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        UINT mipLevels = (binding.valid && binding.decoded && !binding.baseOnly && resource) ? binding.decoded->levels : 1;
        if (binding.valid && !binding.decoded && !binding.baseOnly) mipLevels = chainLevels;
        if (binding.cube)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = mipLevels;
        }
        else
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = mipLevels;
        }
        // A resolve that swapped red and blue changes which channel each guest component reads.
        auto channel = [&](int i)
        {
            const uint32_t c = binding.swizzle[i];
            if (depthCopy) return c < 4 ? 0u : c;   // every channel reads the depth
            return (swap && (c == 0 || c == 2)) ? 2 - c : c;
        };
        view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(channel(0), channel(1), channel(2), channel(3));
        gDevice->CreateShaderResourceView(resource, &view, handle);
    }

    std::set<const DepthResource*> gFlatDepthsCleared;   // per frame

    void ExecuteDraw(const RendererDraw& draw)
    {
        bool deferred = false;
        ID3D12PipelineState* pipeline = PipelineFor(draw, &deferred);
        if (deferred) return;   // its pipeline is still being built: left out of this frame
        TargetResource* target = draw.hasColor ? GetTarget(draw.target) : nullptr;
        DepthResource* depth = draw.depthBound ? GetDepth(draw.depth) : nullptr;
        if (!pipeline || (draw.hasColor && !target) || (draw.depthBound && !depth) || !draw.vertexCount) { gDrawsFailed++; return; }
        if (target) SetTargetState(*target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // The 2D pass has its own depth and stencil surface (the display's size); it starts clean.
        if (depth && gActiveTwelfths != gResTwelfths && gFlatDepthsCleared.insert(depth).second)
            gList->ClearDepthStencilView(DepthView(*depth), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        ArenaBlock constants = ArenaAlloc(draw.constants.size() * 4, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        memcpy(constants.cpu, draw.constants.data(), draw.constants.size() * 4);
        if (gShadowTwelfths != 12)
        {
            // The shaders that read a shadow map filter it by hand: nine taps one texel apart, blended
            // by the position inside a texel. The game passes the map's size and the texel steps as
            // pixel shader constants 257.z (1024), 257.w (1/1024) and 258 (0, 1, 2, 3 texels); with a
            // larger map they are restated for its real size, or the taps skip texels and streak.
            float* c257 = reinterpret_cast<float*>(constants.cpu) + 257 * 4;
            float* c258 = c257 + 4;
            const float size = c257[2];
            const bool shadowConstants = size >= 256.0f && size <= 4096.0f && std::fabs(c257[3] * size - 1.0f) < 0.01f &&
                c258[0] == 0.0f && std::fabs(c258[1] * size - 1.0f) < 0.01f && std::fabs(c258[2] * size - 2.0f) < 0.01f;
            if (shadowConstants)
            {
                bool readsShadowMap = false;
                for (const TextureBinding& binding : draw.textures)
                    if (binding.valid && !binding.decoded)
                    {
                        auto found = gResolved.find(binding.resolvedBase);
                        readsShadowMap = readsShadowMap || (found != gResolved.end() && found->second.depth && found->second.twelfths == gShadowTwelfths && found->second.width == found->second.height);
                    }
                if (readsShadowMap)
                {
                    const float k = gShadowTwelfths / 12.0f;
                    c257[2] *= k; c257[3] /= k;
                    for (int i = 1; i < 4; i++) c258[i] /= k;
                }
            }
        }

        ArenaBlock system = ArenaAlloc(sizeof(SystemConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        auto* s = reinterpret_cast<SystemConstants*>(system.cpu);
        memset(s, 0, sizeof(*s));
        memcpy(s->ndcScale, draw.ndcScale, sizeof(s->ndcScale));
        memcpy(s->ndcOffset, draw.ndcOffset, sizeof(s->ndcOffset));
        s->bypassViewport = draw.bypassViewport ? 1 : 0;
        s->alphaFunc = draw.alphaFunc;
        s->alphaRef = draw.alphaRef;
        s->paramGen = draw.paramGen;
        s->extra[0] = 12.0f / float(gActiveTwelfths);
        if (gTemporalOn && !gInShadowPass && !gInFlatPass && draw.depthBound && (draw.depthControl & 2))
        {
            s->extra[1] = gJitterNdc[0];
            s->extra[2] = gJitterNdc[1];
        }
        memcpy(s->vertexBase, draw.vertexBase.data(), sizeof(s->vertexBase));
        memcpy(s->boolConstants, draw.boolConstants.data(), sizeof(s->boolConstants));

        UINT64 vertexBytes = std::max<UINT64>(draw.vertexData.size(), 16);
        ArenaBlock vertices = ArenaAlloc(vertexBytes, 16);
        if (!draw.vertexData.empty()) memcpy(vertices.cpu, draw.vertexData.data(), draw.vertexData.size());

        const UINT firstDescriptor = gDrawSrvUsed;
        gDrawSrvUsed += kTextureSlots;
        if (gDrawSrvUsed > kDrawDescriptors) { gDrawSrvUsed = kTextureSlots; gDrawsFailed++; return; }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(firstDescriptor) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(firstDescriptor) * gSrvStep;
        for (UINT i = 0; i < kTextureSlots; i++)
        {
            const TextureBinding& binding = draw.textures[i];
            s->samplerPoint[i] = (binding.point ? 25 : 0) + binding.addressU * 5 + binding.addressV;   // sampler index
            s->texScale[i][0] = binding.scale[0];
            s->texScale[i][1] = binding.scale[1];
            s->texScale[i][2] = binding.expScale;   // 1 for unbound slots (the default)
            s->texSign[i] = binding.valid ? binding.sign : 0;
            WriteTextureDescriptor(cpu, binding);
            cpu.ptr += gSrvStep;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
        if (target)
        {
            rtv = gTargetRtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += SIZE_T(target->rtvIndex) * gTargetRtvStep;
        }
        if (depth) dsv = DepthView(*depth);
        gList->OMSetRenderTargets(target ? 1 : 0, target ? &rtv : nullptr, FALSE, depth ? &dsv : nullptr);
        if (depth) gList->OMSetStencilRef(draw.stencilRefMask & 0xFF);
        // The constant blend factors. Left unset they are zero: a draw that scales its alpha by the
        // constant alpha (the Aerie boss's orbs) then wiped the target's alpha over its whole quad,
        // and the passes after it that read that alpha showed the quad as a square.
        gList->OMSetBlendFactor(draw.blendConstant);
        const float rs = gActiveTwelfths / 12.0f;
        D3D12_VIEWPORT viewport{ draw.viewport[0] * rs, draw.viewport[1] * rs, draw.viewport[2] * rs, draw.viewport[3] * rs, 0.0f, 1.0f };
        D3D12_RECT scissor{ ScaledInt(draw.scissor[0]), ScaledInt(draw.scissor[1]), ScaledInt(draw.scissor[2]), ScaledInt(draw.scissor[3]) };
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(pipeline);
        gList->SetGraphicsRootConstantBufferView(0, constants.gpu);
        gList->SetGraphicsRootConstantBufferView(1, system.gpu);
        gList->SetGraphicsRootShaderResourceView(2, vertices.gpu);
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(draw.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW lastIndexView{};
        if (draw.indices.empty())
        {
            gList->DrawInstanced(draw.vertexCount, 1, 0, 0);
        }
        else
        {
            ArenaBlock indices = ArenaAlloc(draw.indices.size() * 4, 4);
            memcpy(indices.cpu, draw.indices.data(), draw.indices.size() * 4);
            D3D12_INDEX_BUFFER_VIEW view{ indices.gpu, UINT(draw.indices.size() * 4), DXGI_FORMAT_R32_UINT };
            gList->IASetIndexBuffer(&view);
            gList->DrawIndexedInstanced(UINT(draw.indices.size()), 1, 0, 0, 0);
            lastIndexView = view;
        }
        gDrawsExecuted++;
        // For the per-object motion vectors: opaque scene geometry drawn with a skinning shader.
        if (gTemporalOn && !gInShadowPass && !gInFlatPass && draw.depthBound && (draw.depthControl & 2) && (draw.depthControl & 4) && draw.hasColor && !draw.rectList && !draw.indices.empty())
        {
            const std::vector<int>* slots = VertexTextureSlots(*draw.vs);
            if (slots && !slots->empty())
            {
                SkinnedDraw record;
                record.draw = &draw;
                const uint64_t identity[5] = { draw.vs->hash, draw.ps->hash, draw.vertexCount, draw.indices.size(), draw.vertexData.size() };
                record.key = XXH3_64bits(identity, sizeof(identity));
                record.system = system.gpu;
                record.vertices = vertices.gpu;
                record.indices = lastIndexView;
                record.viewport = viewport;
                record.scissor = scissor;
                gSkinnedDraws.push_back(record);
            }
        }
    }

    // Clears requested alongside a copy happen after it. RB_COLOR_CLEAR is read as 0xAARRGGBB and
    // RB_DEPTH_CLEAR as 24-bit depth over 8-bit stencil.
    void ApplyResolveClears(const RendererResolve& resolve)
    {
        if (resolve.clearColor)
        {
            if (TargetResource* target = GetTarget(resolve.source))
            {
                SetTargetState(*target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = gTargetRtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += SIZE_T(target->rtvIndex) * gTargetRtvStep;
                const uint32_t c = resolve.colorClear;
                const FLOAT color[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f };
                gList->ClearRenderTargetView(rtv, color, 0, nullptr);
            }
        }
        if (resolve.clearDepth)
        {
            if (DepthResource* depth = GetDepth(resolve.depth))
                gList->ClearDepthStencilView(DepthView(*depth), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    float(resolve.depthClear >> 8) / 16777215.0f, UINT8(resolve.depthClear & 0xFF), 0, nullptr);
        }
    }

    void ExecuteDepthResolve(const RendererResolve& resolve)
    {
        DepthResource* source = GetDepth(resolve.depth);
        if (!source || !resolve.width || !resolve.height) return;
        ResolvedResource& destination = gResolved[resolve.destBase];
        if (!gInShadowPass && gActiveTwelfths == gResTwelfths && resolve.width >= 1024 && resolve.width > resolve.height) gSceneDepthBase = resolve.destBase;
        bool created = false;
        if (!destination.texture || !destination.depth || destination.width != resolve.width || destination.height != resolve.height || destination.twelfths != gActiveTwelfths)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = Scaled(resolve.width);
            desc.Height = Scaled(resolve.height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kDepthCopyFormat;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = kDepthCopyFormat;
            clear.Color[0] = 1.0f;
            if (destination.texture) gRetired.push_back(std::move(destination.texture));
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&destination.texture)), "depth resolve texture")) return;
            destination.width = resolve.width;
            destination.height = resolve.height;
            destination.twelfths = gActiveTwelfths;
            destination.depth = true;
            destination.version++;
            destination.swapRedBlue = false;
            created = true;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gDepthCopyRtvHeap->GetCPUDescriptorHandleForHeapStart();
        gDevice->CreateRenderTargetView(destination.texture.Get(), nullptr, rtv);
        if (created)
        {
            const FLOAT farDepth[4] = { 1.0f, 0, 0, 0 };
            gList->ClearRenderTargetView(rtv, farDepth, 0, nullptr);
        }
        else
        {
            auto toTarget = Transition(destination.texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
            gList->ResourceBarrier(1, &toTarget);
        }
        auto toRead = Transition(source->texture.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kShaderRead);
        gList->ResourceBarrier(1, &toRead);

        // Clip the copied rectangle to both surfaces.
        int sourceX = resolve.sourceX, sourceY = resolve.sourceY, destX = resolve.destX, destY = resolve.destY;
        int copyWidth = int(resolve.copyWidth), copyHeight = int(resolve.copyHeight);
        const int scale = int(std::max(1u, resolve.scale));
        if (sourceX < 0) { destX -= sourceX / scale; copyWidth += sourceX / scale; sourceX = 0; }
        if (sourceY < 0) { destY -= sourceY / scale; copyHeight += sourceY / scale; sourceY = 0; }
        D3D12_RESOURCE_DESC sourceDesc = source->texture->GetDesc();
        // The rectangles are in guest pixels; the surfaces are larger by the resolution scale.
        const int guestSourceWidth = int(sourceDesc.Width) * 12 / int(gActiveTwelfths), guestSourceHeight = int(sourceDesc.Height) * 12 / int(gActiveTwelfths);
        copyWidth = std::min({ copyWidth, (guestSourceWidth - sourceX) / scale, int(resolve.width) - destX });
        copyHeight = std::min({ copyHeight, (guestSourceHeight - sourceY) / scale, int(resolve.height) - destY });
        if (copyWidth > 0 && copyHeight > 0)
        {
            const UINT slot = gDrawSrvUsed;
            gDrawSrvUsed += kTextureSlots;
            if (gDrawSrvUsed <= kDrawDescriptors)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += SIZE_T(slot) * gSrvStep;
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += UINT64(slot) * gSrvStep;
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                gDevice->CreateShaderResourceView(source->texture.Get(), &view, cpu);

                ArenaBlock constants = ArenaAlloc(256, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
                const int32_t values[8] = { ScaledInt(destX), ScaledInt(destY), ScaledInt(sourceX), ScaledInt(sourceY), scale, 0, 0, 0 };
                memcpy(constants.cpu, values, sizeof(values));
                D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(Scaled(resolve.width)), float(Scaled(resolve.height)), 0.0f, 1.0f };
                D3D12_RECT scissor{ ScaledInt(destX), ScaledInt(destY), ScaledInt(destX + copyWidth), ScaledInt(destY + copyHeight) };
                gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                gList->RSSetViewports(1, &viewport);
                gList->RSSetScissorRects(1, &scissor);
                BindDrawRoot();
                gList->SetPipelineState(gDepthCopyPipeline.Get());
                gList->SetGraphicsRootConstantBufferView(0, constants.gpu);
                gList->SetGraphicsRootDescriptorTable(3, gpu);
                gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                gList->DrawInstanced(3, 1, 0, 0);
            }
        }
        auto toWrite = Transition(source->texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto toSample = Transition(destination.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
        D3D12_RESOURCE_BARRIER after[2] = { toWrite, toSample };
        gList->ResourceBarrier(2, after);
        ApplyResolveClears(resolve);
    }

    void ExecuteResolve(const RendererResolve& resolve)
    {
        if (resolve.copyDepth) { ExecuteDepthResolve(resolve); return; }
        if (!resolve.copyColor) { ApplyResolveClears(resolve); return; }
        TargetResource* source = GetTarget(resolve.source);
        if (!source) return;
        const uint32_t width = resolve.width, height = resolve.height;
        if (!width || !height) return;

        ResolvedResource& destination = gResolved[resolve.destBase];
        bool created = false;
        if (!destination.texture || destination.depth || destination.width != width || destination.height != height || destination.twelfths != gActiveTwelfths)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = Scaled(width);
            desc.Height = Scaled(height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kTargetFormat;
            desc.SampleDesc.Count = 1;
            if (destination.texture) gRetired.push_back(std::move(destination.texture));
            // Created as a render target only so it can start cleared: a copy may cover part of it
            // (tiles, or an edge pixel at fractional scales), and the rest must not be leftover memory.
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE zeroClear{};
            zeroClear.Format = kTargetFormat;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &zeroClear, IID_PPV_ARGS(&destination.texture)), "resolve texture")) return;
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = gDepthCopyRtvHeap->GetCPUDescriptorHandleForHeapStart();
                gDevice->CreateRenderTargetView(destination.texture.Get(), nullptr, rtv);
                gList->ClearRenderTargetView(rtv, zeroClear.Color, 0, nullptr);
                auto toCopy = Transition(destination.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                gList->ResourceBarrier(1, &toCopy);
            }
            destination.width = width;
            destination.height = height;
            destination.twelfths = gActiveTwelfths;
            destination.depth = false;
            created = true;
        }
        destination.swapRedBlue = resolve.swapRedBlue;
        destination.version++;

        SetTargetState(*source, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!created)
        {
            auto toCopy = Transition(destination.texture.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
            gList->ResourceBarrier(1, &toCopy);
        }
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = destination.texture.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = source->texture.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        // Clip the rectangle to both surfaces; tiles of one frame land at different destination rows.
        int sourceX = resolve.sourceX, sourceY = resolve.sourceY, destX = resolve.destX, destY = resolve.destY;
        int copyWidth = int(resolve.copyWidth), copyHeight = int(resolve.copyHeight);
        if (sourceX < 0) { destX -= sourceX; copyWidth += sourceX; sourceX = 0; }
        if (sourceY < 0) { destY -= sourceY; copyHeight += sourceY; sourceY = 0; }
        copyWidth = std::min({ copyWidth, int(source->width) - sourceX, int(width) - destX });
        copyHeight = std::min({ copyHeight, int(source->height) - sourceY, int(height) - destY });
        if (copyWidth > 0 && copyHeight > 0)
        {
            // In host pixels, kept inside both textures (rounding can add a pixel when the scale isn't whole).
            const D3D12_RESOURCE_DESC fromDesc = source->texture->GetDesc(), toDesc = destination.texture->GetDesc();
            const UINT hostSourceX = Scaled(UINT(sourceX)), hostSourceY = Scaled(UINT(sourceY)), hostDestX = Scaled(UINT(destX)), hostDestY = Scaled(UINT(destY));
            const UINT hostWidth = std::min({ Scaled(UINT(sourceX + copyWidth)) - hostSourceX, UINT(fromDesc.Width) - hostSourceX, UINT(toDesc.Width) - hostDestX });
            const UINT hostHeight = std::min({ Scaled(UINT(sourceY + copyHeight)) - hostSourceY, UINT(fromDesc.Height) - hostSourceY, UINT(toDesc.Height) - hostDestY });
            D3D12_BOX box{ hostSourceX, hostSourceY, 0, hostSourceX + hostWidth, hostSourceY + hostHeight, 1 };
            if (hostWidth && hostHeight) gList->CopyTextureRegion(&to, hostDestX, hostDestY, 0, &from, &box);
        }
        D3D12_RESOURCE_STATES finalCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        // NIER_NO_SCALED_WRITEBACK=1 (debugging) skips the read-back unless the scale is 1, as an early
        // version did: the exposure then keeps a stale value and the scene can go dark.
        static const bool skipScaled = GetEnvironmentVariableA("NIER_NO_SCALED_WRITEBACK", nullptr, 0) > 0;
        if (resolve.writeBack && !(skipScaled && gActiveTwelfths != 12))
        {
            if (!gWriteBack) gWriteBack = MakeBuffer(kWriteBackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_RESOURCE_DESC desc = destination.texture->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 size = 0;
            gDevice->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
            const UINT64 offset = (gWriteBackUsed + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            if (gWriteBack && offset + size <= kWriteBackBytes)
            {
                auto toSource = Transition(destination.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                gList->ResourceBarrier(1, &toSource);
                footprint.Offset = offset;
                D3D12_TEXTURE_COPY_LOCATION readTo{}, readFrom{};
                readTo.pResource = gWriteBack.Get();
                readTo.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                readTo.PlacedFootprint = footprint;
                readFrom.pResource = destination.texture.Get();
                readFrom.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                gList->CopyTextureRegion(&readTo, 0, 0, 0, &readFrom, nullptr);
                gWriteBackUsed = offset + size;
                gPendingWriteBacks.push_back({ resolve, offset, footprint.Footprint.RowPitch, UINT(desc.Width), UINT(desc.Height) });
                finalCopyState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
        }
        auto toRead = Transition(destination.texture.Get(), finalCopyState, kShaderRead);
        gList->ResourceBarrier(1, &toRead);
        ApplyResolveClears(resolve);
    }

    // NIER_SHOW_SURFACE=<hex guest address>[@n] displays that resolved surface instead of the scanout;
    // with @n, the n-th copy to it in each frame. A comma-separated list shows each entry for 60 frames
    // in turn (not per frame: the window is on screen), so save frames 60 apart to capture each one.
    constexpr uint32_t kSnapshotBase = 0xFFFFF000;
    struct ShownSurface { uint32_t base = 0, occurrence = 0; };
    uint64_t gExecutingFrame = 0;
    ShownSurface Shown()
    {
        static const std::vector<ShownSurface> list = [] {
            std::vector<ShownSurface> entries;
            char value[512];
            if (GetEnvironmentVariableA("NIER_SHOW_SURFACE", value, sizeof(value)))
                for (char* item = strtok(value, ","); item; item = strtok(nullptr, ","))
                {
                    ShownSurface s;
                    char* end = nullptr;
                    s.base = uint32_t(strtoul(item, &end, 16));
                    if (end && *end == '@') s.occurrence = uint32_t(strtoul(end + 1, nullptr, 10));
                    entries.push_back(s);
                }
            return entries;
        }();
        return list.empty() ? ShownSurface{} : list[(gExecutingFrame / 60) % list.size()];
    }

    void BindDrawRoot()
    {
        gList->SetGraphicsRootSignature(gDrawRoot.Get());
        gList->SetGraphicsRootDescriptorTable(4, gSamplerHeap->GetGPUDescriptorHandleForHeapStart());
    }

    bool SameTarget(const RenderTargetKey& a, const RenderTargetKey& b) { return a.edramBase == b.edramBase && a.width == b.width && a.height == b.height; }

    // The frame's shadow map passes, as [first command, last command] ranges. A shadow map is a
    // square depth copy (1024x1024 in this game; the scene's own depth copies have the screen's
    // shape) that isn't the picture. Its pass is the commands before that copy back to the previous
    // copy from the same surface or the start of the frame: the clears and the casters' draws.
    std::vector<std::pair<size_t, size_t>> ShadowPasses(const RendererFrame& frame, bool always = false)
    {
        std::vector<std::pair<size_t, size_t>> passes;
        if (!always && gShadowTwelfths == gResTwelfths) return passes;
        for (size_t i = 0; i < frame.commands.size(); i++)
        {
            const RendererResolve* resolve = std::get_if<RendererResolve>(&frame.commands[i]);
            if (!resolve || !resolve->copyDepth || resolve->width != resolve->height || resolve->width < 256) continue;
            size_t start = i;
            while (start > 0)
            {
                const RendererCommand& before = frame.commands[start - 1];
                if (const RendererResolve* earlier = std::get_if<RendererResolve>(&before))
                {
                    if (earlier->copyColor || earlier->copyDepth) break;   // another pass's result
                }
                else
                {
                    const RendererDraw& draw = std::get<RendererDraw>(before);
                    // Its casters are drawn through a square viewport; clears are rectangles.
                    if (!draw.rectList && std::fabs(draw.viewport[2] - draw.viewport[3]) > 0.5f) break;
                }
                start--;
            }
            passes.emplace_back(start, i);
        }
        return passes;
    }

    // Passes that make data surfaces, as [first command, last command] ranges: a colour copy that
    // doesn't have the screen's shape (the blur and luminance reduction chain: 512x512 down to
    // 32x2, whose end the game reads back to set the exposure), with the commands before it back to
    // the previous copy. They run at a whole multiple of the game's size (DataTwelfths): at a
    // fractional scale a 32x2 surface is 40x3 at 1600x900 with a viewport 2.5 pixels high, its last
    // row is half covered, the average the game reads is too dark and so is the whole picture.
    // The whole chain goes together: its surfaces are levels of one texture and must agree.
    uint32_t DataTwelfths() { return std::max(1u, gResTwelfths / 12) * 12; }
    std::vector<std::pair<size_t, size_t>> DataPasses(const RendererFrame& frame, bool always = false)
    {
        std::vector<std::pair<size_t, size_t>> passes;
        if (!always && gResTwelfths == DataTwelfths()) return passes;
        for (size_t i = 0; i < frame.commands.size(); i++)
        {
            const RendererResolve* resolve = std::get_if<RendererResolve>(&frame.commands[i]);
            if (!resolve || !resolve->copyColor || !resolve->width || !resolve->height) continue;
            if (resolve->width * 9 == resolve->height * 16 || resolve->destBase == frame.scanoutBase) continue;
            size_t start = i;
            while (start > 0)
            {
                const RendererResolve* earlier = std::get_if<RendererResolve>(&frame.commands[start - 1]);
                if (earlier && (earlier->copyColor || earlier->copyDepth)) break;   // another pass's result
                start--;
            }
            passes.emplace_back(start, i);
            static std::set<uint64_t> logged;   // which surfaces these are (debugging)
            if (logged.size() < 24 && logged.insert(uint64_t(resolve->destBase) << 32 | resolve->width << 16 | resolve->height).second)
                Log("[d3d12] data pass: commands %zu-%zu -> %08X %ux%u%s\n", start, i, resolve->destBase, resolve->width, resolve->height, resolve->writeBack ? " (read back by the game)" : "");
        }
        return passes;
    }

    // The frame's closing 2D pass. A frame ends with the copy that produces the picture shown (the
    // scanout); before it the game draws the final full-screen composite of the 3D scene and then
    // the HUD, menus and text into the same target, none of it depth-tested. That run of draws is
    // rendered at the display's resolution instead of the render resolution, so the 2D stays sharp
    // when the scene is drawn smaller (or isn't oversized when it is drawn larger): the composite
    // scales the scene texture to the display as it draws, and everything after lands on display
    // pixels. Returns the index of the first command of the pass (commands.size() if there is none)
    // and the target it draws to. NIER_NATIVE_2D=0 turns it off.
    size_t FindFlatPhase(const RendererFrame& frame, RenderTargetKey& target, bool always = false)
    {
        const size_t none = frame.commands.size();
        static const bool enabled = [] {
            char value[8];
            return !(GetEnvironmentVariableA("NIER_NATIVE_2D", value, sizeof(value)) && value[0] == '0');
        }();
        if (!frame.scanoutBase || (!always && (!enabled || gDisplayTwelfths == gResTwelfths))) return none;
        size_t end = none;
        for (size_t i = frame.commands.size(); i-- > 0;)
            if (const RendererResolve* resolve = std::get_if<RendererResolve>(&frame.commands[i]))
                if (resolve->copyColor && resolve->destBase == frame.scanoutBase) { end = i; target = resolve->source; break; }
        if (end == none) return none;
        size_t start = end;
        for (size_t i = end; i-- > 0;)
        {
            if (const RendererDraw* draw = std::get_if<RendererDraw>(&frame.commands[i]))
            {
                if (!SameTarget(draw->target, target)) continue;   // a pass into another surface: left as it is
                const bool depthTested = (draw->depthControl & 2) && ((draw->depthControl >> 4) & 7) != 7;
                if (!draw->hasColor || depthTested) break;          // the 3D scene
                start = i;
            }
            else if (SameTarget(std::get<RendererResolve>(frame.commands[i]).source, target)) break;   // the scene is copied out here
        }
        return start == end ? none : start;
    }

    // Starts the 2D pass: the display-sized target takes over the picture the render-sized one holds.
    void BeginFlatPhase(const RenderTargetKey& key, bool drawnThisFrame)
    {
        gActiveTwelfths = gResTwelfths;
        TargetResource* from = GetTarget(key);
        gActiveTwelfths = gDisplayTwelfths;
        TargetResource* to = GetTarget(key);
        if (!from || !to || !drawnThisFrame) return;   // nothing new at render size: the display-sized picture carries on
        const UINT slot = gDrawSrvUsed;
        if (slot + kTextureSlots > kDrawDescriptors) return;
        gDrawSrvUsed += kTextureSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kTargetFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(from->texture.Get(), &view, cpu);
        SetTargetState(*from, kShaderRead);
        SetTargetState(*to, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gTargetRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(to->rtvIndex) * gTargetRtvStep;
        const D3D12_RESOURCE_DESC toDesc = to->texture->GetDesc();
        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(toDesc.Width), float(toDesc.Height), 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(toDesc.Width), LONG(toDesc.Height) };
        gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(gTargetCopyPipeline.Get());
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
    }

    // FSR 1 (PC Settings > Upscaling): AMD's spatial upscaler, for a render resolution below the
    // display's. The frame's closing 2D pass (FindFlatPhase) draws at display size, and its first
    // draws sample the finished scene from a render-sized resolved surface: plain bilinear filtering
    // there is what made a low render resolution look soft. With FSR 1 that surface is first enlarged
    // to display size by EASU (edge-adaptive upsampling) and sharpened by RCAS, and the 2D pass
    // samples the result one to one. The shader code is AMD's (ffx_a.h, ffx_fsr1.h, MIT licensed),
    // read from the fsr1 folder beside the executable (tools/get_fsr1.ps1 fetches it); without those
    // files the setting does nothing. NIER_UPSCALING=<0|1|2> overrides the setting (testing).
    struct UpscaledSurface
    {
        ComPtr<ID3D12Resource> easu, sharpened;
        uint32_t inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0;
        D3D12_RESOURCE_STATES easuState = D3D12_RESOURCE_STATE_RENDER_TARGET, sharpenedState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12Resource* source = nullptr;   // what it was made from
        uint64_t version = 0;
        UINT slot = 0;
    };
    std::unordered_map<uint32_t, UpscaledSurface> gUpscaled;        // by the resolved surface's guest address
    ComPtr<ID3D12PipelineState> gEasuPipeline, gRcasPipeline;
    ComPtr<ID3D12DescriptorHeap> gFsrRtvHeap;
    constexpr UINT kFsrSurfaces = 4;
    int gFsrState = 0;   // 0 not tried, 1 ready, -1 unavailable

    bool FsrReady()
    {
        if (gFsrState) return gFsrState > 0;
        gFsrState = -1;
        auto readFile = [](const char* path, std::string& out) {
            FILE* file = fopen(path, "rb");
            if (!file) return false;
            fseek(file, 0, SEEK_END);
            const long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            out.resize(size_t(std::max(size, 0L)));
            const bool ok = size > 0 && fread(out.data(), 1, out.size(), file) == out.size();
            fclose(file);
            return ok;
        };
        std::string a, fsr;
        if (!readFile("fsr1\\ffx_a.h", a) || !readFile("fsr1\\ffx_fsr1.h", fsr))
        {
            Log("[fsr1] fsr1\\ffx_a.h and ffx_fsr1.h not found beside the executable (tools/get_fsr1.ps1): Upscaling stays plain\n");
            return false;
        }
        const std::string source = std::string(R"(
cbuffer Fsr : register(b1) { uint4 con0, con1, con2, con3; float4 outputTexel; };
Texture2D<float4> source : register(t0, space1);
SamplerState linearClamp : register(s0);
#define A_GPU 1
#define A_HLSL 1
)") + a + R"(
#ifdef PASS_EASU
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return source.GatherRed(linearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return source.GatherGreen(linearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return source.GatherBlue(linearClamp, p); }
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return source.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
)" + fsr + R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target
{
#ifdef PASS_EASU
    AF3 color;
    FsrEasuF(color, AU2(position.xy), con0, con1, con2, con3);
    return float4(color, source.SampleLevel(linearClamp, position.xy * outputTexel.xy, 0).a);   // alpha: plainly filtered
#else
    AF1 r, g, b;
    FsrRcasF(r, g, b, AU2(position.xy), con0);
    return float4(r, g, b, source.Load(int3(position.xy, 0)).a);
#endif
}
)";
        ComPtr<ID3DBlob> vs, easu, rcas, errors;
        const D3D_SHADER_MACRO easuMacros[] = { { "PASS_EASU", "1" }, { nullptr, nullptr } };
        if (FAILED(D3DCompile(source.data(), source.size(), "fsr1", nullptr, nullptr, "VSMain", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
            FAILED(D3DCompile(source.data(), source.size(), "fsr1", easuMacros, nullptr, "PSMain", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &easu, &errors)) ||
            FAILED(D3DCompile(source.data(), source.size(), "fsr1", nullptr, nullptr, "PSMain", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &rcas, &errors)))
        {
            Log("[fsr1] shaders failed to compile: %s\n", errors ? (const char*)errors->GetBufferPointer() : "unknown");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = gDrawRoot.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { easu->GetBufferPointer(), easu->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = kTargetFormat;
        desc.SampleDesc.Count = 1;
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gEasuPipeline)), "FSR 1 EASU pipeline")) return false;
        desc.PS = { rcas->GetBufferPointer(), rcas->GetBufferSize() };
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gRcasPipeline)), "FSR 1 RCAS pipeline")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = kFsrSurfaces * 2;
        if (!Check(gDevice->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&gFsrRtvHeap)), "FSR 1 RTV heap")) return false;
        Log("[fsr1] ready (EASU + RCAS)\n");
        gFsrState = 1;
        return true;
    }

    // One full-screen FSR pass: `from` (in the shader-read state) into `to` (in the render-target state).
    void FsrPass(ID3D12PipelineState* pipeline, ID3D12Resource* from, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height, const uint32_t constants[20])
    {
        const UINT slot = gDrawSrvUsed;
        gDrawSrvUsed += kTextureSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kTargetFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(from, &view, cpu);
        ArenaBlock block = ArenaAlloc(80, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        memcpy(block.cpu, constants, 80);
        D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
        gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(pipeline);
        gList->SetGraphicsRootConstantBufferView(1, block.gpu);
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
    }

    // The display-sized, sharpened version of a render-sized resolved surface; made again only when
    // the surface has been copied to since. Null if it can't be made.
    ID3D12Resource* Upscaled(uint32_t base, const ResolvedResource& surface)
    {
        if (!FsrReady()) return nullptr;
        auto found = gUpscaled.find(base);
        if (found == gUpscaled.end())
        {
            if (gUpscaled.size() >= kFsrSurfaces) return nullptr;
            const UINT slot = UINT(gUpscaled.size());
            found = gUpscaled.emplace(base, UpscaledSurface{}).first;
            found->second.slot = slot;
        }
        UpscaledSurface& up = found->second;
        const D3D12_RESOURCE_DESC in = surface.texture->GetDesc();
        const uint32_t outWidth = (surface.width * gDisplayTwelfths + 6) / 12, outHeight = (surface.height * gDisplayTwelfths + 6) / 12;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gFsrRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(up.slot) * 2 * gTargetRtvStep;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvSharpened = rtv;
        rtvSharpened.ptr += gTargetRtvStep;
        if (!up.sharpened || up.outWidth != outWidth || up.outHeight != outHeight)
        {
            if (up.easu) gRetired.push_back(std::move(up.easu));
            if (up.sharpened) gRetired.push_back(std::move(up.sharpened));
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = outWidth;
            desc.Height = outHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kTargetFormat;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = kTargetFormat;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&up.easu)), "FSR 1 surface") ||
                !Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&up.sharpened)), "FSR 1 surface"))
            {
                up.easu.Reset();
                up.sharpened.Reset();
                return nullptr;
            }
            gDevice->CreateRenderTargetView(up.easu.Get(), nullptr, rtv);
            gDevice->CreateRenderTargetView(up.sharpened.Get(), nullptr, rtvSharpened);
            up.easuState = up.sharpenedState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            up.outWidth = outWidth;
            up.outHeight = outHeight;
            up.source = nullptr;
            Log("[fsr1] surface %08X: %ux%u -> %ux%u\n", base, unsigned(in.Width), unsigned(in.Height), outWidth, outHeight);
        }
        if (up.source == surface.texture.Get() && up.version == surface.version) return up.sharpened.Get();
        if (gDrawSrvUsed + 2 * kTextureSlots > kDrawDescriptors) return nullptr;
        up.source = surface.texture.Get();
        up.version = surface.version;
        up.inWidth = uint32_t(in.Width);
        up.inHeight = uint32_t(in.Height);

        auto bits = [](float value) { uint32_t b; memcpy(&b, &value, 4); return b; };
        const float inW = float(in.Width), inH = float(in.Height), outW = float(outWidth), outH = float(outHeight);
        uint32_t constants[20] = {
            bits(inW / outW), bits(inH / outH), bits(0.5f * inW / outW - 0.5f), bits(0.5f * inH / outH - 0.5f),   // as FsrEasuCon
            bits(1.0f / inW), bits(1.0f / inH), bits(1.0f / inW), bits(-1.0f / inH),
            bits(-1.0f / inW), bits(2.0f / inH), bits(1.0f / inW), bits(2.0f / inH),
            bits(0.0f), bits(4.0f / inH), 0, 0,
            bits(1.0f / outW), bits(1.0f / outH), 0, 0,
        };
        if (up.easuState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            auto barrier = Transition(up.easu.Get(), up.easuState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            gList->ResourceBarrier(1, &barrier);
        }
        FsrPass(gEasuPipeline.Get(), surface.texture.Get(), rtv, outWidth, outHeight, constants);
        D3D12_RESOURCE_BARRIER barriers[2] = { Transition(up.easu.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead), {} };
        UINT count = 1;
        if (up.sharpenedState != D3D12_RESOURCE_STATE_RENDER_TARGET) barriers[count++] = Transition(up.sharpened.Get(), up.sharpenedState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gList->ResourceBarrier(count, barriers);
        up.easuState = kShaderRead;
        // RCAS: sharpness in stops below the maximum (0.2 is AMD's suggested default), as FsrRcasCon.
        static const float stops = [] { char v[16]; return GetEnvironmentVariableA("NIER_FSR_SHARPNESS", v, sizeof(v)) ? float(atof(v)) : 0.2f; }();
        uint32_t rcas[20] = { bits(std::exp2(-stops)), 0, 0, 0 };
        FsrPass(gRcasPipeline.Get(), up.easu.Get(), rtvSharpened, outWidth, outHeight, rcas);
        auto toRead = Transition(up.sharpened.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead);
        gList->ResourceBarrier(1, &toRead);
        up.sharpenedState = kShaderRead;
        return up.sharpened.Get();
    }

    // Temporal upscaling: FSR 3.1 through AMD's FSR SDK (amd_fidelityfx_loader_dx12.dll and
    // amd_fidelityfx_upscaler_dx12.dll beside the executable; on Radeon cards that support it the
    // same DLL runs FSR 4 instead). It sits where FSR 1 does: the finished scene, render-sized, on
    // its way into the 2D pass. The game gives it none of what such an upscaler needs, so:
    //  - jitter: see gTemporalOn above (applied at the end of the translated vertex shaders);
    //  - depth: the game's own full-size depth copy of the frame (gSceneDepthBase);
    //  - motion vectors: computed here from that depth and the camera of this frame and the last,
    //    so they describe the camera's movement only. Things that move by themselves (characters,
    //    particles) have no vectors of their own yet and can leave trails.
    // The colour it gets is the tone-mapped picture after the game's post-processing (declared as
    // non-linear), not the scene before it as the SDK would prefer.
    // Per-object motion vectors for skinned geometry (characters, enemies, cloth). The camera-only
    // vectors treat everything as standing still in the world, so anything animated left trails in
    // the temporal upscaler. A skinning vertex shader takes its bone matrices from a texture and
    // its camera from constants: run with last frame's texture and constants it gives where the
    // same vertex was a frame ago. So each such shader gets a second form whose body exists twice,
    // once reading this frame's inputs and once last frame's (the constants as cP[], each texture
    // tN as tPN in slot (N + 16) % 32), and which hands both clip positions to a pixel shader
    // that writes their difference. Those draws are repeated into the motion vector texture after
    // the camera pass, kept only where they are the visible surface (compared with the frame's
    // depth copy, in slot 31). A draw is paired with last frame's by its shaders and sizes and
    // its order among equals. NIER_NO_OBJECT_MOTION=1 turns this off.
    struct MotionVariant
    {
        std::vector<int> slots;        // texture slots the vertex shader reads
        bool usable = false, tried = false;
        ComPtr<ID3DBlob> blob;
        std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> pipelines;   // by cull state
    };
    std::unordered_map<uint64_t, MotionVariant> gMotionVariants;
    ComPtr<ID3DBlob> gObjectMotionPixel;
    struct PreviousDraw
    {
        std::array<float, 2048> constants;
        std::vector<std::pair<int, std::shared_ptr<DecodedTexture>>> textures;
    };
    std::unordered_map<uint64_t, PreviousDraw> gPreviousDraws;   // by key and order among equals
    std::unordered_map<uint64_t, DecodedResource> gDecodedBefore;   // host textures holding last frame's contents

    MotionVariant& VariantFor(const GuestShader& vs)
    {
        MotionVariant& variant = gMotionVariants[vs.hash];
        if (variant.tried) return variant;
        variant.tried = true;
        const std::string& hlsl = vs.hlsl;
        if (hlsl.find("TextureCube") != std::string::npos) return variant;
        for (size_t at = 0; (at = hlsl.find("Texture2D<float4> t", at)) != std::string::npos; at++)
        {
            const int slot = atoi(hlsl.c_str() + at + 19);
            if (hlsl.compare(at + 19, 1, "0") >= 0 && hlsl.compare(at + 19, 1, "9") <= 0) variant.slots.push_back(slot);
        }
        return variant;
    }

    const std::vector<int>* VertexTextureSlots(const GuestShader& vs) { return &VariantFor(vs).slots; }

    // Compiles the two-bodied form of a skinning vertex shader (once; false if it can't be made).
    bool PrepareVariant(const GuestShader& vs, MotionVariant& variant)
    {
        if (variant.blob) return true;
        if (variant.usable || variant.slots.empty()) return false;   // usable is set only on the first attempt
        variant.usable = true;
        const std::string& hlsl = vs.hlsl;
        const std::string mainStart = "Interpolated main(uint vertexId : SV_VertexID)";
        const std::string constants = "cbuffer Constants : register(b0) { float4 c[512]; };";
        const size_t mainAt = hlsl.find(mainStart), constantsAt = hlsl.find(constants);
        if (mainAt == std::string::npos || constantsAt == std::string::npos) return false;
        for (int slot : variant.slots)
            for (int other : variant.slots)
                if ((slot + 16) % 32 == other || slot == 31 || (slot + 16) % 32 == 31) return false;   // no free slot for last frame's copy
        std::string head = hlsl.substr(0, mainAt);
        head.replace(constantsAt, constants.size(), "cbuffer Constants : register(b0) { float4 c[512]; float4 cP[512]; };");
        for (int slot : variant.slots)
            head += "Texture2D<float4> tP" + std::to_string(slot) + " : register(t" + std::to_string((slot + 16) % 32) + ", space1);\n";
        const std::string body = hlsl.substr(mainAt);
        std::string now = body, before = body;
        now.replace(0, mainStart.size(), "Interpolated BodyNow_(uint vertexId)");
        before.replace(0, mainStart.size(), "Interpolated BodyBefore_(uint vertexId)");
        auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
            for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
            {
                const bool wordStart = at == 0 || !(isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
                if (wordStart) text.replace(at, from.size(), to);
                else { at += from.size(); at -= to.size(); }
            }
        };
        replaceAll(before, "c[", "cP[");
        for (int slot : variant.slots) replaceAll(before, "t" + std::to_string(slot) + ".", "tP" + std::to_string(slot) + ".");
        const std::string source = head + now + before + R"(
struct MotionOut { float4 position : SV_Position; float4 now : TEXCOORD0; float4 before : TEXCOORD1; };
MotionOut main(uint vertexId : SV_VertexID)
{
    Interpolated a = BodyNow_(vertexId), b = BodyBefore_(vertexId);
    MotionOut output;
    output.position = a.position;
    output.now = a.position;
    output.before = b.position;
    return output;
}
)";
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(source.data(), source.size(), "motion variant", nullptr, nullptr, "main", "vs_5_1", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &variant.blob, &errors)))
        {
            Log("[fsr3] motion form of vertex shader %016llX failed to compile: %s\n", vs.hash, errors ? (const char*)errors->GetBufferPointer() : "unknown");
            variant.blob.Reset();
            return false;
        }
        return true;
    }

    ID3D12PipelineState* VariantPipeline(MotionVariant& variant, const RendererDraw& draw)
    {
        const uint32_t state = uint32_t(draw.cull) | (draw.frontCounterClockwise ? 4u : 0u);
        auto found = variant.pipelines.find(state);
        if (found != variant.pipelines.end()) return found->second.Get();
        if (!gObjectMotionPixel)
        {
            const char kPixel[] = R"(
Texture2D<float4> depthTexture : register(t31, space1);
struct MotionOut { float4 position : SV_Position; float4 now : TEXCOORD0; float4 before : TEXCOORD1; };
float2 main(MotionOut input) : SV_Target
{
    // Only the visible surface: the frame's depth copy holds it (depth runs 0 near to 1 far, and
    // 1 - depth shrinks with distance, so the allowance is a share of that).
    float depth = depthTexture.Load(int3(input.position.xy, 0)).r;
    if (input.position.z > depth + (1.0 - depth) * 0.02 + 1e-6) discard;
    float2 a = input.now.xy / input.now.w, b = input.before.xy / input.before.w;
    return float2(b.x - a.x, a.y - b.y) * 0.5;
}
)";
            ComPtr<ID3DBlob> errors;
            if (FAILED(D3DCompile(kPixel, sizeof(kPixel) - 1, "object motion", nullptr, nullptr, "main", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &gObjectMotionPixel, &errors)))
            {
                Log("[fsr3] object motion pixel shader failed to compile: %s\n", errors ? (const char*)errors->GetBufferPointer() : "unknown");
                return nullptr;
            }
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = gDrawRoot.Get();
        desc.VS = { variant.blob->GetBufferPointer(), variant.blob->GetBufferSize() };
        desc.PS = { gObjectMotionPixel->GetBufferPointer(), gObjectMotionPixel->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = draw.cull == 0 ? D3D12_CULL_MODE_NONE : draw.cull == 1 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
        desc.RasterizerState.FrontCounterClockwise = draw.frontCounterClockwise;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pipeline;
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "object motion pipeline")) return nullptr;
        return (variant.pipelines[state] = pipeline).Get();
    }

    // The frame's skinned draws again, into the motion vector texture (bound as the render target
    // by the caller, with `rtv`), each with last frame's inputs beside this frame's.
    void ObjectMotion(D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* depthCopy)
    {
        static const bool off = GetEnvironmentVariableA("NIER_NO_OBJECT_MOTION", nullptr, 0) > 0;
        if (off || gSkinnedDraws.empty()) return;
        std::unordered_map<uint64_t, uint32_t> order;
        uint32_t drawn = 0;
        for (const SkinnedDraw& record : gSkinnedDraws)
        {
            const uint64_t key = record.key + order[record.key]++ * 0x9E3779B97F4A7C15ull;
            auto before = gPreviousDraws.find(key);
            if (before == gPreviousDraws.end()) continue;
            const RendererDraw& draw = *record.draw;
            MotionVariant& variant = VariantFor(*draw.vs);
            if (!PrepareVariant(*draw.vs, variant)) continue;
            ID3D12PipelineState* pipeline = VariantPipeline(variant, draw);
            if (!pipeline || gDrawSrvUsed + kTextureSlots > kDrawDescriptors) continue;

            // Textures first (uploads change the command list's state): this frame's contents in the
            // usual host texture, last frame's in its twin.
            ID3D12Resource* textures[kTextureSlots] = {};
            DXGI_FORMAT formats[kTextureSlots] = {};
            bool complete = true;
            for (int slot : variant.slots)
            {
                const TextureBinding& binding = draw.textures[slot];
                std::shared_ptr<DecodedTexture> old;
                for (const auto& [oldSlot, texture] : before->second.textures) if (oldSlot == slot) old = texture;
                if (!binding.valid || !binding.decoded || !old || old->width != binding.decoded->width || old->height != binding.decoded->height || old->format != binding.decoded->format) { complete = false; break; }
                textures[slot] = GetDecoded(*binding.decoded);
                textures[(slot + 16) % 32] = GetDecoded(*old, gDecodedBefore);
                formats[slot] = formats[(slot + 16) % 32] = DecodedFormat(binding.decoded->format);
                if (!textures[slot] || !textures[(slot + 16) % 32]) { complete = false; break; }
            }
            if (!complete) continue;

            const UINT first = gDrawSrvUsed;
            gDrawSrvUsed += kTextureSlots;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(first) * gSrvStep;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(first) * gSrvStep;
            for (UINT slot = 0; slot < kTextureSlots; slot++)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                ID3D12Resource* resource = slot == 31 ? depthCopy : textures[slot];
                view.Format = slot == 31 ? kDepthCopyFormat : resource ? formats[slot] : DXGI_FORMAT_R8G8B8A8_UNORM;
                if (resource && slot != 31)
                {
                    // As the draw itself views it (component order).
                    const bool own = std::find(variant.slots.begin(), variant.slots.end(), int(slot)) != variant.slots.end();
                    const TextureBinding& binding = draw.textures[own ? slot : (slot + 16) % 32];   // last frame's copy sits 16 slots on
                    view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(binding.swizzle[0], binding.swizzle[1], binding.swizzle[2], binding.swizzle[3]);
                }
                gDevice->CreateShaderResourceView(resource, &view, cpu);
                cpu.ptr += gSrvStep;
            }
            ArenaBlock constants = ArenaAlloc(2 * 8192, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            memcpy(constants.cpu, draw.constants.data(), 8192);
            memcpy(constants.cpu + 8192, before->second.constants.data(), 8192);
            gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            gList->RSSetViewports(1, &record.viewport);
            gList->RSSetScissorRects(1, &record.scissor);
            BindDrawRoot();
            gList->SetPipelineState(pipeline);
            gList->SetGraphicsRootConstantBufferView(0, constants.gpu);
            gList->SetGraphicsRootConstantBufferView(1, record.system);
            gList->SetGraphicsRootShaderResourceView(2, record.vertices);
            gList->SetGraphicsRootDescriptorTable(3, gpu);
            gList->IASetPrimitiveTopology(draw.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            gList->IASetIndexBuffer(&record.indices);
            gList->DrawIndexedInstanced(record.indices.SizeInBytes / 4, 1, 0, 0, 0);
            drawn++;
        }
        static uint32_t frames = 0;
        if (frames++ % 600 == 0) Log("[fsr3] object motion: %u of %zu skinned draws paired with last frame's\n", drawn, gSkinnedDraws.size());
    }

    // End of a frame's commands: what the next frame's object motion needs of this one.
    void RememberSkinnedDraws()
    {
        gPreviousDraws.clear();
        std::unordered_map<uint64_t, uint32_t> order;
        for (const SkinnedDraw& record : gSkinnedDraws)
        {
            const uint64_t key = record.key + order[record.key]++ * 0x9E3779B97F4A7C15ull;
            PreviousDraw& kept = gPreviousDraws[key];
            kept.constants = record.draw->constants;
            for (int slot : *VertexTextureSlots(*record.draw->vs))
                if (record.draw->textures[slot].valid && record.draw->textures[slot].decoded) kept.textures.emplace_back(slot, record.draw->textures[slot].decoded);
        }
        gSkinnedDraws.clear();
    }

    struct TemporalState
    {
        HMODULE library = nullptr;
        PfnFfxCreateContext create = nullptr;
        PfnFfxDestroyContext destroy = nullptr;
        PfnFfxQuery query = nullptr;
        PfnFfxDispatch dispatch = nullptr;
        int state = 0;   // 0 not tried, 1 loaded, -1 unavailable
        ffxContext context = nullptr;
        bool newest = false;   // the context was made for the FSR 4 setting (else FSR 3.1)
        uint32_t renderWidth = 0, renderHeight = 0, outWidth = 0, outHeight = 0;
        ComPtr<ID3D12Resource> motion, output;
        D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ComPtr<ID3D12PipelineState> motionPipeline;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        uint32_t base = 0;
        ID3D12Resource* source = nullptr;
        uint64_t version = 0;
        bool history = false;   // the upscaler has seen the frame before this one
        uint32_t jitterIndex = 0;
        uint64_t frame = ~0ull;   // the frame it last ran in
    } gTemporal;

    std::string gTemporalName = "FSR 3.1";   // the temporal upscaler in use, for the timing log ("FSR 4.1.2")
    std::string gAdapterName;
    ComPtr<IDXGIAdapter3> gAdapter3;   // for the video memory in use (SampleLoad)
    std::mutex gPerfMutex;
    RendererPerf gPerfTotals;   // since the last RendererPerfTake

    void TemporalMessage(uint32_t type, const wchar_t* message)
    {
        static int logged = 0;
        if (logged++ < 20) Log("[fsr3] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
    }

    bool TemporalLoaded()
    {
        if (gTemporal.state) return gTemporal.state > 0;
        gTemporal.state = -1;
        gTemporal.library = LoadLibraryA("amd_fidelityfx_loader_dx12.dll");
        if (!gTemporal.library)
        {
            Log("[fsr3] amd_fidelityfx_loader_dx12.dll not found beside the executable (tools/get_fsr_sdk.ps1): FSR 3.1 falls back to FSR 1\n");
            return false;
        }
        gTemporal.create = (PfnFfxCreateContext)GetProcAddress(gTemporal.library, "ffxCreateContext");
        gTemporal.destroy = (PfnFfxDestroyContext)GetProcAddress(gTemporal.library, "ffxDestroyContext");
        gTemporal.query = (PfnFfxQuery)GetProcAddress(gTemporal.library, "ffxQuery");
        gTemporal.dispatch = (PfnFfxDispatch)GetProcAddress(gTemporal.library, "ffxDispatch");
        if (!gTemporal.create || !gTemporal.destroy || !gTemporal.query || !gTemporal.dispatch)
        {
            Log("[fsr3] the FSR loader DLL lacks the expected functions\n");
            return false;
        }
        const char kMotionShader[] = R"(
cbuffer Motion : register(b1) { row_major float4x4 reproject; float4 texelAndJitter; };
Texture2D<float4> depthTexture : register(t0, space1);
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
// Where this pixel's surface was on screen a frame ago, given only how the camera moved: the pixel
// is taken back to the world with this frame's camera and projected with the last one's. The result
// is the offset to the previous position, in screen fractions.
float2 PSMain(float4 position : SV_Position) : SV_Target
{
    float depth = depthTexture.Load(int3(position.xy, 0)).r;
    float2 uv = position.xy * texelAndJitter.xy;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - texelAndJitter.zw;
    float4 before = mul(reproject, float4(ndc, depth, 1.0));
    float2 ndcBefore = before.xy / (abs(before.w) > 1e-6 ? before.w : 1e-6);
    return float2(ndcBefore.x - ndc.x, ndc.y - ndcBefore.y) * 0.5;
}
)";
        ComPtr<ID3DBlob> vs, ps, errors;
        if (FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, "motion", nullptr, nullptr, "VSMain", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors)) ||
            FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, "motion", nullptr, nullptr, "PSMain", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors)))
        {
            Log("[fsr3] motion vector shader failed to compile: %s\n", errors ? (const char*)errors->GetBufferPointer() : "unknown");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = gDrawRoot.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        if (!Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gTemporal.motionPipeline)), "motion vector pipeline")) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (!Check(gDevice->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&gTemporal.rtvHeap)), "motion vector RTV heap")) return false;
        gTemporal.state = 1;
        return true;
    }

    // 0 off, 1 FSR 1, 2 temporal (FSR 3.1 / 4); only below the display's resolution.
    int UpscalingSetting()
    {
        static const int forced = [] { char v[8]; return GetEnvironmentVariableA("NIER_UPSCALING", v, sizeof(v)) ? atoi(v) : -1; }();
        return forced >= 0 ? forced : Settings().upscaling.load();
    }

    // 0 plain filtering, 1 FSR 1, 2 temporal (the FSR 3.1 and FSR 4 settings: TemporalWantsNewest).
    int UpscalingMode()
    {
        if (gResTwelfths >= gDisplayTwelfths) return 0;
        const int mode = std::min(UpscalingSetting(), 2);
        if (mode == 2 && !TemporalLoaded()) return 1;
        return mode;
    }

    bool TemporalWantsNewest() { return UpscalingSetting() >= 3; }

    // The upscaler version to ask the FSR loader for. Left to itself it picks the newest it can find,
    // and an AMD driver supplies FSR 4 in place of 3.1: a far heavier upscaler, which on an integrated
    // GPU costs more than rendering fewer pixels saves. So the FSR 3.1 setting asks for the newest
    // 3.x, and only the FSR 4 setting takes a 4.x (3.1 when the driver has none). 0: no choice made.
    uint64_t TemporalVersion(bool newest)
    {
        TemporalState& t = gTemporal;
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = gDevice.Get();
        versions.outputCount = &count;
        if (t.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) return 0;
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        versions.versionIds = ids.data();
        versions.versionNames = names.data();
        if (t.query(nullptr, &versions.header) != FFX_API_RETURN_OK) return 0;
        std::string listed;
        uint64_t chosen = 0, fallback = 0;
        int chosenRank[3] = { -1, -1, -1 }, fallbackRank[3] = { -1, -1, -1 };
        for (uint64_t i = 0; i < count; i++)
        {
            const char* name = names[i] ? names[i] : "";
            listed += listed.empty() ? "" : ", ";
            listed += name;
            int rank[3] = { 0, 0, 0 };
            sscanf(name, "%d.%d.%d", &rank[0], &rank[1], &rank[2]);
            const bool wanted = newest ? rank[0] >= 4 : rank[0] == 3;
            int* best = wanted ? chosenRank : fallbackRank;
            if (wanted || rank[0] == 3)
                if (std::lexicographical_compare(best, best + 3, rank, rank + 3))
                {
                    memcpy(best, rank, sizeof(rank));
                    (wanted ? chosen : fallback) = ids[i];
                }
        }
        static std::string logged;
        if (logged != listed) { Log("[fsr3] upscaler versions on this system: %s\n", listed.c_str()); logged = listed; }
        if (!chosen && newest) Log("[fsr3] no FSR 4 on this system (it comes with the AMD driver, for GPUs that support it): using FSR 3.1\n");
        return chosen ? chosen : fallback;
    }

    float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f, result = 0.0f;
        for (uint32_t i = index; i > 0; i /= base) { f /= float(base); result += f * float(i % base); }
        return result;
    }

    // Start of a frame's commands: this frame's jitter, the camera of the frame before, and this
    // frame's camera: the perspective view-projection matrix (vertex constants c5-c8, by rows: a
    // unit w row, and a z row differing from it only in the last term) that the most depth-tested
    // draws outside the shadow and 2D passes carry. Objects with their own combined matrix in
    // those registers pass the same test, so the first one found would not do.
    void TemporalBeginFrame(const RendererFrame& frame, const std::vector<std::pair<size_t, size_t>>& shadowPasses, size_t flatStart)
    {
        gTemporalOn = UpscalingMode() == 2;
        if (gCameraNowValid) memcpy(gCameraBefore, gCameraNow, sizeof(gCameraBefore));
        gCameraBeforeValid = gCameraNowValid;
        gCameraNowValid = false;
        gSceneDepthBase = 0;
        if (UpscalingMode() == 2)
        {
            std::unordered_map<uint64_t, std::pair<uint32_t, const float*>> votes;
            size_t pass = 0;
            for (size_t i = 0; i < frame.commands.size() && i < flatStart; i++)
            {
                while (pass < shadowPasses.size() && i > shadowPasses[pass].second) pass++;
                if (pass < shadowPasses.size() && i >= shadowPasses[pass].first) continue;
                const RendererDraw* draw = std::get_if<RendererDraw>(&frame.commands[i]);
                if (!draw || !draw->hasColor || !draw->depthBound || !(draw->depthControl & 2)) continue;
                const float* c = draw->constants.data() + 5 * 4;
                const float wLength = std::sqrt(c[12] * c[12] + c[13] * c[13] + c[14] * c[14]);
                const float apart = std::fabs(c[8] - c[12]) + std::fabs(c[9] - c[13]) + std::fabs(c[10] - c[14]);
                if (std::fabs(wLength - 1.0f) > 0.02f || apart > 0.02f) continue;
                auto& vote = votes[XXH3_64bits(c, 64)];
                vote.first++;
                vote.second = c;
            }
            const float* best = nullptr;
            uint32_t most = 0;
            for (const auto& [hash, vote] : votes) if (vote.first > most) { most = vote.first; best = vote.second; }
            if (best && most >= 3)
            {
                for (int i = 0; i < 16; i++) gCameraNow[i] = best[i];
                gCameraNowValid = true;
            }
        }
        gJitterPixels[0] = gJitterPixels[1] = gJitterNdc[0] = gJitterNdc[1] = 0.0f;
        if (!gTemporalOn) { gTemporal.history = false; return; }
        // The sequence FSR recommends: Halton(2, 3), 8 phases times the area ratio.
        const float ratio = float(gDisplayTwelfths) / float(gResTwelfths);
        const uint32_t phases = std::max(1u, uint32_t(8.0f * ratio * ratio + 0.5f));
        const uint32_t index = (gTemporal.jitterIndex++ % phases) + 1;
        gJitterPixels[0] = Halton(index, 2) - 0.5f;
        gJitterPixels[1] = Halton(index, 3) - 0.5f;
        // NIER_JITTER_SCALE=<x>[,<y>] multiplies the offset drawn with (not the one reported to the
        // upscaler): large values make the jitter visible, -1 flips an axis (debugging).
        static const std::pair<float, float> scale = [] {
            char v[32];
            float x = 1.0f, y = 1.0f;
            if (GetEnvironmentVariableA("NIER_JITTER_SCALE", v, sizeof(v)) && sscanf(v, "%f,%f", &x, &y) == 1) y = x;
            return std::make_pair(x, y);
        }();
        gJitterNdc[0] = 2.0f * gJitterPixels[0] * scale.first / float(Scaled(1280));
        gJitterNdc[1] = -2.0f * gJitterPixels[1] * scale.second / float(Scaled(720));
    }

    bool Invert4x4(const double m[16], double out[16])
    {
        double a[4][8];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
        for (int col = 0; col < 4; col++)
        {
            int pivot = col;
            for (int r = col + 1; r < 4; r++) if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
            if (std::fabs(a[pivot][col]) < 1e-12) return false;
            for (int c = 0; c < 8; c++) std::swap(a[col][c], a[pivot][c]);
            const double scale = 1.0 / a[col][col];
            for (int c = 0; c < 8; c++) a[col][c] *= scale;
            for (int r = 0; r < 4; r++)
                if (r != col)
                {
                    const double factor = a[r][col];
                    for (int c = 0; c < 8; c++) a[r][c] -= factor * a[col][c];
                }
        }
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
        return true;
    }

    // The display-sized picture the temporal upscaler makes of a render-sized scene surface. Null if
    // it can't this frame (the caller then uses FSR 1).
    ID3D12Resource* TemporalUpscaled(uint32_t base, const ResolvedResource& surface)
    {
        TemporalState& t = gTemporal;
        if (!gTemporalOn || !gCameraNowValid || !gSceneDepthBase) return nullptr;
        static const bool bypass = GetEnvironmentVariableA("NIER_FSR3_BYPASS", nullptr, 0) > 0;   // jittered frames straight to FSR 1 (debugging)
        if (bypass) return nullptr;
        if (t.output && t.base == base && t.source == surface.texture.Get() && t.version == surface.version) return t.output.Get();
        if (t.frame == gExecutingFrame && t.base != base) return nullptr;   // one scene surface a frame
        auto depthFound = gResolved.find(gSceneDepthBase);
        if (depthFound == gResolved.end() || !depthFound->second.depth || !depthFound->second.texture) return nullptr;
        const D3D12_RESOURCE_DESC in = surface.texture->GetDesc(), depthDesc = depthFound->second.texture->GetDesc();
        if (depthDesc.Width != in.Width || depthDesc.Height != in.Height) return nullptr;
        const uint32_t renderWidth = uint32_t(in.Width), renderHeight = uint32_t(in.Height);
        const uint32_t outWidth = (surface.width * gDisplayTwelfths + 6) / 12, outHeight = (surface.height * gDisplayTwelfths + 6) / 12;
        if (gDrawSrvUsed + kTextureSlots > kDrawDescriptors) return nullptr;

        const bool newest = TemporalWantsNewest();
        if (!t.context || t.renderWidth != renderWidth || t.renderHeight != renderHeight || t.outWidth != outWidth || t.outHeight != outHeight || t.newest != newest)
        {
            if (t.context) { WaitForGpu(); t.destroy(&t.context, nullptr); t.context = nullptr; }
            if (t.motion) gRetired.push_back(std::move(t.motion));
            if (t.output) gRetired.push_back(std::move(t.output));
            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = gDevice.Get();
            ffxCreateContextDescUpscale create{};
            create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            create.header.pNext = &backend.header;
            ffxOverrideVersion pinned{};
            pinned.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            pinned.versionId = TemporalVersion(newest);
            if (pinned.versionId) backend.header.pNext = &pinned.header;
            t.newest = newest;
            create.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
            create.maxRenderSize = { renderWidth, renderHeight };
            create.maxUpscaleSize = { outWidth, outHeight };
            create.fpMessage = TemporalMessage;
            const ffxReturnCode_t code = t.create(&t.context, &create.header, nullptr);
            if (code != FFX_API_RETURN_OK)
            {
                Log("[fsr3] creating the upscaler failed (code %u): falling back to FSR 1\n", code);
                t.context = nullptr;
                t.state = -1;
                return nullptr;
            }
            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            t.query(&t.context, &provider.header);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Width = renderWidth;
            desc.Height = renderHeight;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kShaderRead, nullptr, IID_PPV_ARGS(&t.motion)), "motion vector texture")) return nullptr;
            gDevice->CreateRenderTargetView(t.motion.Get(), nullptr, t.rtvHeap->GetCPUDescriptorHandleForHeapStart());
            desc.Width = outWidth;
            desc.Height = outHeight;
            desc.Format = kTargetFormat;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&t.output)), "upscaler output")) return nullptr;
            t.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            t.renderWidth = renderWidth;
            t.renderHeight = renderHeight;
            t.outWidth = outWidth;
            t.outHeight = outHeight;
            t.history = false;
            Log("[fsr3] upscaler ready: %s, %ux%u -> %ux%u\n", provider.versionName ? provider.versionName : "unknown version", renderWidth, renderHeight, outWidth, outHeight);
            gTemporalName = std::string("FSR ") + (provider.versionName ? provider.versionName : "3.1");
        }

        // Motion vectors: last frame's camera applied to this frame's positions.
        double inverse[16], reproject[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const bool reprojected = gCameraBeforeValid && Invert4x4(gCameraNow, inverse);
        if (reprojected)
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    double sum = 0;
                    for (int k = 0; k < 4; k++) sum += gCameraBefore[r * 4 + k] * inverse[k * 4 + c];
                    reproject[r * 4 + c] = sum;
                }
        float constants[20];
        for (int i = 0; i < 16; i++) constants[i] = float(reproject[i]);
        constants[16] = 1.0f / float(renderWidth);
        constants[17] = 1.0f / float(renderHeight);
        constants[18] = gJitterNdc[0];
        constants[19] = gJitterNdc[1];
        {
            auto toTarget = Transition(t.motion.Get(), kShaderRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
            gList->ResourceBarrier(1, &toTarget);
            const UINT slot = gDrawSrvUsed;
            gDrawSrvUsed += kTextureSlots;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(slot) * gSrvStep;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(slot) * gSrvStep;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = kDepthCopyFormat;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            gDevice->CreateShaderResourceView(depthFound->second.texture.Get(), &view, cpu);
            ArenaBlock block = ArenaAlloc(sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            memcpy(block.cpu, constants, sizeof(constants));
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            D3D12_VIEWPORT viewport{ 0.0f, 0.0f, float(renderWidth), float(renderHeight), 0.0f, 1.0f };
            D3D12_RECT scissor{ 0, 0, LONG(renderWidth), LONG(renderHeight) };
            gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            gList->RSSetViewports(1, &viewport);
            gList->RSSetScissorRects(1, &scissor);
            BindDrawRoot();
            gList->SetPipelineState(t.motionPipeline.Get());
            gList->SetGraphicsRootConstantBufferView(1, block.gpu);
            gList->SetGraphicsRootDescriptorTable(3, gpu);
            gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            gList->DrawInstanced(3, 1, 0, 0);
            ObjectMotion(rtv, depthFound->second.texture.Get());   // skinned geometry: its own movement on top
            D3D12_RESOURCE_BARRIER barriers[2] = { Transition(t.motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kShaderRead), {} };
            UINT count = 1;
            if (t.outputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) barriers[count++] = Transition(t.output.Get(), t.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            gList->ResourceBarrier(count, barriers);
            t.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        // The camera's near plane and field of view, from the matrix: z = w - near, and the y row's
        // length is 1 / tan(fov / 2).
        const double* m = gCameraNow;
        const double nearPlane = std::max(m[15] - m[11], 1e-3);
        const double yScale = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = gList.Get();
        dispatch.color = ffxApiGetResourceDX12(surface.texture.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.depth = ffxApiGetResourceDX12(depthFound->second.texture.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.motionVectors = ffxApiGetResourceDX12(t.motion.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatch.output = ffxApiGetResourceDX12(t.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = { gJitterPixels[0], gJitterPixels[1] };
        dispatch.motionVectorScale = { float(renderWidth), float(renderHeight) };
        dispatch.renderSize = { renderWidth, renderHeight };
        dispatch.upscaleSize = { outWidth, outHeight };
        static const float sharpness = [] { char v[16]; return GetEnvironmentVariableA("NIER_FSR3_SHARPNESS", v, sizeof(v)) ? float(atof(v)) : 0.3f; }();
        dispatch.enableSharpening = sharpness > 0.0f;
        dispatch.sharpness = sharpness;
        dispatch.frameTimeDelta = 1000.0f / 60.0f;
        dispatch.preExposure = 1.0f;
        dispatch.reset = !t.history || !reprojected || t.base != base;
        dispatch.cameraNear = float(nearPlane);
        dispatch.cameraFar = FLT_MAX;
        dispatch.cameraFovAngleVertical = float(2.0 * std::atan(1.0 / std::max(yScale, 1e-3)));
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        static const bool debugView = GetEnvironmentVariableA("NIER_FSR3_DEBUG", nullptr, 0) > 0;   // the SDK's debug overlay
        if (debugView) dispatch.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
        {
            static uint32_t calls = 0, resets = 0;   // how it is going (debugging)
            resets += dispatch.reset;
            if (++calls == 1 || (debugView && calls % 300 == 1))
                Log("[fsr3] f%llu: %u dispatches, %u resets; jitter %.3f %.3f px; near %.3f fov %.1f deg; depth %08X; reprojection %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f\n",
                    gExecutingFrame, calls, resets, gJitterPixels[0], gJitterPixels[1], dispatch.cameraNear, dispatch.cameraFovAngleVertical * 57.2958f, gSceneDepthBase,
                    reproject[0], reproject[1], reproject[2], reproject[3], reproject[4], reproject[5], reproject[6], reproject[7]);
        }
        const ffxReturnCode_t code = t.dispatch(&t.context, &dispatch.header);
        // The SDK leaves its own heaps and pipeline on the command list.
        ID3D12DescriptorHeap* heaps[] = { gDrawSrvHeap.Get(), gSamplerHeap.Get() };
        gList->SetDescriptorHeaps(2, heaps);
        auto toRead = Transition(t.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
        gList->ResourceBarrier(1, &toRead);
        t.outputState = kShaderRead;
        if (code != FFX_API_RETURN_OK)
        {
            static int failures = 0;
            if (failures++ < 4) Log("[fsr3] dispatch failed (code %u)\n", code);
            t.history = false;
            return nullptr;
        }
        t.history = true;
        t.frame = gExecutingFrame;
        t.base = base;
        t.source = surface.texture.Get();
        t.version = surface.version;
        return t.output.Get();
    }

    // GPU time by stage: a timestamp is written into the command list wherever the stage changes, and
    // the differences are summed per stage once the frame is done (the render thread waits for it).
    enum GpuStage : int { kStageOther, kStageShadows, kStageScene, kStagePost, kStageData, kStageUpscale, kStageFlat, kStagePresent };
    const char* const kStageNames[kRendererPerfStages] = { "other", "shadows", "scene", "post", "exposure", "upscaling", "2D", "present" };
    struct StageTimer
    {
        static const UINT kMax = 256;
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> readback;
        UINT used = 0;
        int stages[kMax] = {};
        int current = kStageOther;
        bool tried = false, ok = false;
        UINT64 frequency = 0;
    } gStages;

    void StageBegin()
    {
        StageTimer& t = gStages;
        if (!t.tried)
        {
            t.tried = true;
            D3D12_QUERY_HEAP_DESC desc{};
            desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            desc.Count = StageTimer::kMax;
            t.readback = MakeBuffer(StageTimer::kMax * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            t.ok = SUCCEEDED(gDevice->CreateQueryHeap(&desc, IID_PPV_ARGS(&t.heap))) && t.readback && SUCCEEDED(gQueue->GetTimestampFrequency(&t.frequency)) && t.frequency;
            if (!t.ok) Log("[perf] no GPU timestamps on this system: the stage times stay at zero\n");
        }
        t.used = 0;
        t.current = kStageOther;
        if (t.ok) gList->EndQuery(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, t.used++);
    }

    void StageMark(int stage)
    {
        StageTimer& t = gStages;
        if (!t.ok || stage == t.current || t.used == 0 || t.used >= StageTimer::kMax - 1) return;
        t.stages[t.used - 1] = t.current;
        gList->EndQuery(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, t.used++);
        t.current = stage;
    }

    void StageEnd()
    {
        StageTimer& t = gStages;
        if (!t.ok || t.used == 0) return;
        t.stages[t.used - 1] = t.current;
        gList->EndQuery(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, t.used++);
        gList->ResolveQueryData(t.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, t.used, t.readback.Get(), 0);
    }

    // After the frame's GPU work is complete: adds each stage's milliseconds to `ms`.
    void StageCollect(double ms[kRendererPerfStages])
    {
        StageTimer& t = gStages;
        if (!t.ok || t.used < 2) return;
        void* mapped = nullptr;
        D3D12_RANGE range{ 0, SIZE_T(t.used) * sizeof(UINT64) };
        if (FAILED(t.readback->Map(0, &range, &mapped))) return;
        const UINT64* stamps = static_cast<const UINT64*>(mapped);
        for (UINT i = 0; i + 1 < t.used; i++)
            if (stamps[i + 1] > stamps[i]) ms[t.stages[i]] += double(stamps[i + 1] - stamps[i]) * 1000.0 / double(t.frequency);
        D3D12_RANGE none{ 0, 0 };
        t.readback->Unmap(0, &none);
    }

    // Before a draw of the 2D pass: the scene-sized resolved surfaces it samples get their upscaled
    // stand-ins (WriteTextureDescriptor binds them).
    void PrepareUpscaledTextures(const RendererDraw& draw)
    {
        gUpscaledNow.clear();
        const int mode = UpscalingMode();
        if (!mode) return;
        for (const TextureBinding& binding : draw.textures)
        {
            if (!binding.valid || binding.decoded || binding.resolvedLevelCount || binding.cube) continue;
            auto found = gResolved.find(binding.resolvedBase);
            if (found == gResolved.end() || found->second.depth || !found->second.texture || found->second.twelfths != gResTwelfths) continue;
            if (found->second.width < 1024 || found->second.height < 576) continue;   // the scene, not a small effect buffer
            StageMark(kStageUpscale);
            ID3D12Resource* upscaled = mode == 2 ? TemporalUpscaled(binding.resolvedBase, found->second) : nullptr;
            if (!upscaled) upscaled = Upscaled(binding.resolvedBase, found->second);
            if (upscaled) gUpscaledNow[binding.resolvedBase] = upscaled;
        }
        StageMark(kStageFlat);
    }

    void ExecuteGuestCommands(const RendererFrame& frame)
    {
        uint32_t shownCopies = 0;
        gExecutingFrame = frame.frame;
        gArenaOffset = 0;
        gDrawSrvUsed = 0;
        ID3D12DescriptorHeap* heaps[] = { gDrawSrvHeap.Get(), gSamplerHeap.Get() };
        gList->SetDescriptorHeaps(2, heaps);
        gFlatDepthsCleared.clear();
        RenderTargetKey flatTarget;
        const size_t flatStart = FindFlatPhase(frame, flatTarget);
        const bool hasFlat = flatStart < frame.commands.size();
        bool flatBegun = false, drawnThisFrame = false;
        const std::vector<std::pair<size_t, size_t>> shadowPasses = ShadowPasses(frame);
        size_t shadowPass = 0;
        const std::vector<std::pair<size_t, size_t>> dataPasses = DataPasses(frame);
        size_t dataPass = 0;
        TemporalBeginFrame(frame, shadowPasses, flatStart);
        // For the stage times the passes are found whether or not they need their own treatment, so
        // the same work is counted under the same stage at every setting.
        const std::vector<std::pair<size_t, size_t>> timedShadows = shadowPasses.empty() ? ShadowPasses(frame, true) : shadowPasses;
        const std::vector<std::pair<size_t, size_t>> timedData = dataPasses.empty() ? DataPasses(frame, true) : dataPasses;
        RenderTargetKey timedFlatTarget = flatTarget;
        const size_t timedFlatStart = hasFlat ? flatStart : FindFlatPhase(frame, timedFlatTarget, true);
        size_t timedShadow = 0, timedDataPass = 0;
        for (size_t index = 0; index < frame.commands.size(); index++)
        {
            const auto& command = frame.commands[index];
            {
                while (timedShadow < timedShadows.size() && index > timedShadows[timedShadow].second) timedShadow++;
                while (timedDataPass < timedData.size() && index > timedData[timedDataPass].second) timedDataPass++;
                const RendererDraw* draw = std::get_if<RendererDraw>(&command);
                if (timedShadow < timedShadows.size() && index >= timedShadows[timedShadow].first) StageMark(kStageShadows);
                else if (timedDataPass < timedData.size() && index >= timedData[timedDataPass].first) StageMark(kStageData);
                else if (index >= timedFlatStart && (draw ? draw->hasColor && SameTarget(draw->target, timedFlatTarget) : SameTarget(std::get<RendererResolve>(command).source, timedFlatTarget)))
                    StageMark(kStageFlat);
                else if (draw) StageMark(draw->rectList ? kStagePost : kStageScene);   // full-screen passes are rectangles
            }
            gActiveTwelfths = gResTwelfths;
            gInShadowPass = gInFlatPass = false;
            while (shadowPass < shadowPasses.size() && index > shadowPasses[shadowPass].second) shadowPass++;
            if (shadowPass < shadowPasses.size() && index >= shadowPasses[shadowPass].first)
            {
                // A shadow map pass: its own scale, nothing else applies.
                gActiveTwelfths = gShadowTwelfths;
                gInShadowPass = true;
                if (auto* draw = std::get_if<RendererDraw>(&command)) ExecuteDraw(*draw);
                else ExecuteResolve(std::get<RendererResolve>(command));
                gInShadowPass = false;
                continue;
            }
            while (dataPass < dataPasses.size() && index > dataPasses[dataPass].second) dataPass++;
            if (dataPass < dataPasses.size() && index >= dataPasses[dataPass].first)
            {
                // A data surface: a whole multiple of the game's own size.
                gActiveTwelfths = DataTwelfths();
                gInFlatPass = true;   // nothing here is scene geometry (no jitter)
                if (auto* draw = std::get_if<RendererDraw>(&command)) ExecuteDraw(*draw);
                else ExecuteResolve(std::get<RendererResolve>(command));
                gInFlatPass = false;
                continue;
            }
            if (auto* draw = std::get_if<RendererDraw>(&command))
            {
                const bool onTarget = hasFlat && draw->hasColor && SameTarget(draw->target, flatTarget);
                if (onTarget && index >= flatStart)
                {
                    if (!flatBegun) { BeginFlatPhase(flatTarget, drawnThisFrame); flatBegun = true; }
                    gActiveTwelfths = gDisplayTwelfths;
                    gInFlatPass = true;
                    PrepareUpscaledTextures(*draw);
                }
                else if (onTarget) drawnThisFrame = true;
                ExecuteDraw(*draw);
                gUpscaledNow.clear();
            }
            else
            {
                const RendererResolve& resolve = std::get<RendererResolve>(command);
                const bool onTarget = hasFlat && SameTarget(resolve.source, flatTarget);
                const bool flat = onTarget && index >= flatStart;
                if (flat)
                {
                    if (!flatBegun) { BeginFlatPhase(flatTarget, drawnThisFrame); flatBegun = true; }
                    gActiveTwelfths = gDisplayTwelfths;
                }
                else if (onTarget && resolve.clearColor) drawnThisFrame = true;
                if (Shown().occurrence && (resolve.copyColor || resolve.copyDepth) && resolve.destBase == Shown().base && ++shownCopies == Shown().occurrence)
                {
                    RendererResolve snapshot = resolve;
                    snapshot.destBase = kSnapshotBase;
                    snapshot.clearColor = snapshot.clearDepth = false;
                    ExecuteResolve(snapshot);
                }
                ExecuteResolve(resolve);
                if (flat && (resolve.clearColor || resolve.clearDepth))
                {
                    // The clear that ends the frame is meant for the next frame's scene too.
                    gActiveTwelfths = gResTwelfths;
                    ApplyResolveClears(resolve);
                }
            }
        }
        gActiveTwelfths = gResTwelfths;
        gInShadowPass = gInFlatPass = false;
        RememberSkinnedDraws();
    }

    // Shows the scanout surface in the back buffer. It's in render target channel order, so no swap.
    void BlitScanout(uint32_t base)
    {
        auto found = gResolved.find(base);
        if (found == gResolved.end()) return;
        const UINT slot = gDrawSrvUsed;
        gDrawSrvUsed += kTextureSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = found->second.depth ? kDepthCopyFormat : kTargetFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        // NIER_SHOW_ALPHA=1 shows the alpha channel as grey (debugging). Depth copies show as grey.
        static const bool showAlpha = GetEnvironmentVariableA("NIER_SHOW_ALPHA", nullptr, 0) > 0;
        view.Shader4ComponentMapping = found->second.depth ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 5)
                                     : showAlpha ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 3, 3, 5) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(found->second.texture.Get(), &view, cpu);

        float sourceAspect = float(found->second.width) / found->second.height;
        float windowAspect = float(gWidth) / gHeight;
        float w = float(gWidth), h = float(gHeight);
        if (windowAspect > sourceAspect) w = h * sourceAspect; else h = w / sourceAspect;
        D3D12_VIEWPORT viewport{ (gWidth - w) * 0.5f, (gHeight - h) * 0.5f, w, h, 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(gWidth), LONG(gHeight) };
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(gBlitPipeline.Get());
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
    }

    // Our own picture over the game's (the on-screen keyboard), in the largest 16:9 area of the window.
    void DrawOverlay()
    {
        std::shared_ptr<DecodedTexture> overlay;
        {
            std::lock_guard lock(gOverlayMutex);
            overlay = gOverlay;
        }
        if (!overlay || gDrawSrvUsed + kTextureSlots > kDrawDescriptors) return;
        ID3D12Resource* texture = GetDecoded(*overlay);
        if (!texture) return;
        const UINT slot = gDrawSrvUsed;
        gDrawSrvUsed += kTextureSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(texture, &view, cpu);
        float w = float(gWidth), h = float(gHeight);
        if (w / h > 16.0f / 9.0f) w = h * 16.0f / 9.0f; else h = w * 9.0f / 16.0f;
        D3D12_VIEWPORT viewport{ (gWidth - w) * 0.5f, (gHeight - h) * 0.5f, w, h, 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(gWidth), LONG(gHeight) };
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(gOverlayPipeline.Get());
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
    }

    // The system's load, for the readout beside the frame rate counter: processor use (the whole system's), GPU use (the
    // busiest of the 3D engines' totals, as Task Manager shows it; from the system's performance
    // counters), and the memory and video memory this process holds.
    struct Load { double cpu = -1, gpu = -1, ramGb = 0, vramGb = 0; };
    Load SampleLoad()
    {
        Load load;
        static ULONGLONG idleBefore = 0, busyBefore = 0;
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user))
        {
            const auto value = [](const FILETIME& t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            const ULONGLONG idleNow = value(idle), allNow = value(kernel) + value(user);   // kernel time includes idle
            if (busyBefore && allNow > busyBefore) load.cpu = 100.0 * (1.0 - double(idleNow - idleBefore) / double(allNow - busyBefore));
            idleBefore = idleNow;
            busyBefore = allNow;
        }
        static PDH_HQUERY query = nullptr;
        static PDH_HCOUNTER counter = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS)
            {
                Log("[perf] no GPU use counters on this system\n");
                query = nullptr;
            }
            else PdhCollectQueryData(query);
        }
        else if (query && PdhCollectQueryData(query) == ERROR_SUCCESS)
        {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA)
            {
                std::vector<uint8_t> buffer(size);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS)
                {
                    // Instances are "pid_N_luid_..._phys_P_eng_E_engtype_3D": every process's share of each engine.
                    std::unordered_map<std::wstring, double> engines;
                    for (DWORD i = 0; i < count; i++)
                    {
                        const std::wstring name = items[i].szName;
                        if (name.find(L"engtype_3D") == std::wstring::npos || items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                        const size_t luid = name.find(L"luid_");
                        engines[luid == std::wstring::npos ? name : name.substr(luid)] += items[i].FmtValue.doubleValue;
                    }
                    double most = 0;
                    for (const auto& [engine, use] : engines) most = (std::max)(most, use);
                    load.gpu = (std::min)(most, 100.0);
                }
            }
        }
        PROCESS_MEMORY_COUNTERS memory{};
        memory.cb = sizeof(memory);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) load.ramGb = double(memory.WorkingSetSize) / (1024.0 * 1024.0 * 1024.0);
        DXGI_QUERY_VIDEO_MEMORY_INFO video{};
        if (gAdapter3 && SUCCEEDED(gAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video))) load.vramGb = double(video.CurrentUsage) / (1024.0 * 1024.0 * 1024.0);
        return load;
    }

    // Frame rate counter (PC Settings; NIER_SHOW_FPS=1 forces it on): the game frames presented a
    // second, counted here and redrawn twice a second, in the top left corner of the picture. With
    // NIER_SHOW_LOAD=1 it also shows the system's load.
    void DrawFpsCounter()
    {
        static const bool showLoad = GetEnvironmentVariableA("NIER_SHOW_LOAD", nullptr, 0) > 0;
        static const bool forced = showLoad || GetEnvironmentVariableA("NIER_SHOW_FPS", nullptr, 0) > 0;
        static uint32_t frames = 0;
        static LARGE_INTEGER since{};
        static std::shared_ptr<DecodedTexture> label;
        if (!forced && !Settings().showFps) { frames = 0; since.QuadPart = 0; label.reset(); return; }
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (!since.QuadPart) since = now;
        frames++;
        const double elapsed = double(now.QuadPart - since.QuadPart) / double(frequency.QuadPart);
        if (elapsed >= 0.5)
        {
            char text[128];
            int length = snprintf(text, sizeof(text), "%.0f fps", frames / elapsed);
            if (showLoad)
            {
                const Load load = SampleLoad();
                if (load.cpu >= 0) length += snprintf(text + length, sizeof(text) - length, "   CPU %.0f%%", load.cpu);
                if (load.gpu >= 0) length += snprintf(text + length, sizeof(text) - length, "   GPU %.0f%%", load.gpu);
                length += snprintf(text + length, sizeof(text) - length, "   RAM %.1f GB   VRAM %.1f GB", load.ramGb, load.vramGb);
                if (load.cpu >= 0 && load.gpu >= 0)
                {
                    std::lock_guard lock(gPerfMutex);
                    gPerfTotals.loadSamples++;
                    gPerfTotals.cpuPercent += load.cpu;
                    gPerfTotals.gpuPercent += load.gpu;
                    gPerfTotals.ramGb = (std::max)(gPerfTotals.ramGb, load.ramGb);
                    gPerfTotals.vramGb = (std::max)(gPerfTotals.vramGb, load.vramGb);
                }
            }
            auto made = std::make_shared<DecodedTexture>();
            if (DrawSmallLabel(text, made->data, made->width, made->height))
            {
                static uint64_t version = 0;
                made->key = 0xFFFFFFFFFFFF0F5Dull;
                made->contentHash = ++version;
                made->rowPitch = made->width * 4;
                made->subresources.push_back({ 0, made->width * 4, made->height });
                label = made;
            }
            frames = 0;
            since = now;
        }
        if (!label || gDrawSrvUsed + kTextureSlots > kDrawDescriptors) return;
        ID3D12Resource* texture = GetDecoded(*label);
        if (!texture) return;
        const UINT slot = gDrawSrvUsed;
        gDrawSrvUsed += kTextureSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        gDevice->CreateShaderResourceView(texture, &view, cpu);
        // Sized by the picture (the largest 16:9 area of the window), a little in from its corner.
        float w = float(gWidth), h = float(gHeight);
        if (w / h > 16.0f / 9.0f) w = h * 16.0f / 9.0f; else h = w * 9.0f / 16.0f;
        const float labelHeight = h * 0.045f, labelWidth = labelHeight * float(label->width) / float(label->height), margin = h * 0.015f;
        D3D12_VIEWPORT viewport{ (gWidth - w) * 0.5f + margin, (gHeight - h) * 0.5f + margin, labelWidth, labelHeight, 0.0f, 1.0f };
        D3D12_RECT scissor{ 0, 0, LONG(gWidth), LONG(gHeight) };
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        BindDrawRoot();
        gList->SetPipelineState(gOverlayPipeline.Get());
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
    }

    void CALLBACK DebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*)
    {
        static std::atomic<int> logged{ 0 };
        if (severity <= D3D12_MESSAGE_SEVERITY_WARNING && logged.fetch_add(1) < 40) Log("[d3d12 debug] #%d %s\n", int(id), description);
    }

    bool CreateDeviceObjects()
    {
        char debug[8];
        if (GetEnvironmentVariableA("NIER_D3D_DEBUG", debug, sizeof(debug)))
        {
            ComPtr<ID3D12Debug> layer;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
        }

        ComPtr<IDXGIFactory6> factory;
        if (!Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory")) return false;
        ComPtr<IDXGIAdapter1> adapter;
        if (!Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adapter")) return false;
        DXGI_ADAPTER_DESC1 adapterDesc;
        adapter->GetDesc1(&adapterDesc);
        Log("[d3d12] adapter: %ls\n", adapterDesc.Description);
        {
            char name[128] = "";
            WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            gAdapterName = name;
            adapter.As(&gAdapter3);
        }
        if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gDevice)), "device")) return false;
        if (GetEnvironmentVariableA("NIER_D3D_DEBUG", debug, sizeof(debug)))
        {
            ComPtr<ID3D12InfoQueue1> queue;
            DWORD cookie = 0;
            if (SUCCEEDED(gDevice.As(&queue)) && SUCCEEDED(queue->RegisterMessageCallback(DebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie)))
                Log("[d3d12] debug layer messages go to this log\n");
            else
                Log("[d3d12] debug layer requested but its messages are unavailable (Graphics Tools may not be installed)\n");
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(gDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gQueue)), "command queue")) return false;

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = gWidth;
        swapDesc.Height = gHeight;
        swapDesc.Format = kBackBufferFormat;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBackBuffers;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        // VSync off (PC Settings) presents at once, tearing where the display path allows it.
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
            gSwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        swapDesc.Flags = gSwapChainFlags;
        ComPtr<IDXGISwapChain1> swapChain;
        if (!Check(factory->CreateSwapChainForHwnd(gQueue.Get(), gWindow, &swapDesc, nullptr, nullptr, &swapChain), "swap chain")) return false;
        factory->MakeWindowAssociation(gWindow, DXGI_MWA_NO_ALT_ENTER);
        if (!Check(swapChain.As(&gSwapChain), "swap chain interface")) return false;
        gSwapChain->SetMaximumFrameLatency(1);
        gFrameWait = gSwapChain->GetFrameLatencyWaitableObject();

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = kBackBuffers;
        if (!Check(gDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&gRtvHeap)), "RTV heap")) return false;
        gRtvStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = 3;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (!Check(gDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&gSrvHeap)), "SRV heap")) return false;

        if (!Check(gDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gAllocator)), "allocator")) return false;
        if (!Check(gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gAllocator.Get(), nullptr, IID_PPV_ARGS(&gList)), "command list")) return false;
        gList->Close();
        if (!Check(gDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)), "fence")) return false;
        gFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        AcquireBackBuffers();
        return CreatePipeline() && CreateDrawObjects();
    }

    void CreateTexturesIfNeeded(const MailboxPlane planes[3])
    {
        bool matches = true;
        for (int i = 0; i < 3; i++)
        {
            if (!gTextures[i]) { matches = false; break; }
            auto desc = gTextures[i]->GetDesc();
            matches = matches && desc.Width == planes[i].width && desc.Height == planes[i].height;
        }
        if (matches) return;

        WaitForGpu();
        D3D12_CPU_DESCRIPTOR_HANDLE srv = gSrvHeap->GetCPUDescriptorHandleForHeapStart();
        const UINT srvStep = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (int i = 0; i < 3; i++)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = planes[i].width;
            desc.Height = planes[i].height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8_UNORM;
            desc.SampleDesc.Count = 1;
            gTextures[i].Reset();
            Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gTextures[i])), "plane texture");

            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = desc.Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            gDevice->CreateShaderResourceView(gTextures[i].Get(), &view, srv);
            srv.ptr += srvStep;
        }
        gUploadCapacity = 0;
    }

    uint64_t HashPlane(const MailboxPlane& plane)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t row = 0; row < plane.height; row += 16)
            for (uint32_t x = 0; x < plane.width; x += 16) hash = (hash ^ plane.data[row * plane.width + x]) * 1099511628211ull;
        return hash;
    }

    void RecordImageTiming(const MailboxPlane& luma)
    {
        uint64_t hash = HashPlane(luma);
        if (hash == gLastHash) return;
        gLastHash = hash;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (gLastImage.QuadPart)
        {
            double gap = double(now.QuadPart - gLastImage.QuadPart) * 1000.0 / frequency.QuadPart;
            if (gap < 500.0)
            {
                gMovieImages++;
                gGapBins[std::min<int>(int(gap / 16.683 + 0.5), 7)]++;
                gWorstGap = std::max(gWorstGap, gap);
                if (gap > 60.0) gLateImages++;
            }
        }
        gLastImage = now;
    }

    void WriteSavedFrame(uint64_t label, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint)
    {
        void* mapped = nullptr;
        if (FAILED(gReadback->Map(0, nullptr, &mapped))) return;
        CreateDirectoryA("..\\frames", nullptr);
        std::string path = "..\\frames\\f" + std::to_string(label) + ".ppm";
        if (FILE* file = fopen(path.c_str(), "wb"))
        {
            const uint32_t width = footprint.Footprint.Width, height = footprint.Footprint.Height;
            fprintf(file, "P6\n%u %u\n255\n", width, height);
            std::vector<uint8_t> row(width * 3);
            for (uint32_t y = 0; y < height; y++)
            {
                const uint8_t* source = (const uint8_t*)mapped + footprint.Offset + y * footprint.Footprint.RowPitch;
                for (uint32_t x = 0; x < width; x++)
                {
                    row[x * 3] = source[x * 4 + 2];
                    row[x * 3 + 1] = source[x * 4 + 1];
                    row[x * 3 + 2] = source[x * 4];
                }
                fwrite(row.data(), 1, row.size(), file);
            }
            fclose(file);
            Log("[present] saved frame %llu\n", label);
        }
        gReadback->Unmap(0, nullptr);
    }

    // The resolution asked for, in twelfths: the setting is an index into this list of scales;
    // NIER_RES_SCALE=<scale> (0.5 to 4) overrides it.
    uint32_t ResolutionWanted()
    {
        static const double forced = [] {
            char value[16];
            return GetEnvironmentVariableA("NIER_RES_SCALE", value, sizeof(value)) ? atof(value) : 0.0;
        }();
        static const uint32_t kSettingTwelfths[] = { 6, 8, 9, 12, 15, 18, 24, 30, 36, 48 };   // 360p 480p 540p 720p 900p 1080p 1440p 1800p 2160p 2880p
        const double asked = forced > 0.0 ? forced : kSettingTwelfths[std::clamp(Settings().resolutionScale.load(), 0, 9)] / 12.0;
        return uint32_t(std::clamp(int(asked * 12.0 + 0.5), 6, 48));
    }

    // Applies a changed resolution between frames (the GPU is idle: the last frame waited for it).
    // Every surface whose size depends on the scale is dropped; the frame about to be drawn creates
    // what it uses at the new size. Surfaces the game resolved in the frame before are missing for
    // that one frame.
    // Shadow Quality: 0 the original 1024, 1 2048, 2 3072, 3 4096 texels a side (the map's surface is
    // 1040 wide and about 2500 high in guest pixels, and a texture side can't pass 16384: four times
    // is the most). NIER_SHADOW_SCALE=<1-4> overrides with a scale.
    uint32_t ShadowWanted()
    {
        static const double forced = [] {
            char value[16];
            return GetEnvironmentVariableA("NIER_SHADOW_SCALE", value, sizeof(value)) ? atof(value) : 0.0;
        }();
        static const uint32_t kSettingTwelfths[] = { 12, 24, 36, 48 };
        const double asked = forced > 0.0 ? forced : kSettingTwelfths[std::clamp(Settings().shadowQuality.load(), 0, 3)] / 12.0;
        return uint32_t(std::clamp(int(asked * 12.0 + 0.5), 12, 48));
    }

    void ApplyResolutionSetting()
    {
        const uint32_t wanted = ResolutionWanted(), shadow = ShadowWanted();
        // The display's scale: the largest 16:9 picture that fits the window, against 1280x720.
        const double fit = (std::min)(gWidth / 1280.0, gHeight / 720.0);
        const uint32_t display = uint32_t(std::clamp(int(fit * 12.0 + 0.5), 6, 48));
        if (wanted == gResTwelfths && display == gDisplayTwelfths && shadow == gShadowTwelfths && gResolutionLogged) return;
        if (wanted != gResTwelfths || display != gDisplayTwelfths || shadow != gShadowTwelfths)
        {
            gTargets.clear();
            gDepths.clear();
            gResolved.clear();
            for (auto& [base, up] : gUpscaled) { if (up.easu) gRetired.push_back(std::move(up.easu)); if (up.sharpened) gRetired.push_back(std::move(up.sharpened)); }
            gUpscaled.clear();
            gResolvedChains.clear();
            gTargetsCreated = 0;
            gDepthsCreated = 0;
            gResTwelfths = wanted;
            gDisplayTwelfths = display;
            gShadowTwelfths = shadow;
        }
        gActiveTwelfths = gResTwelfths;
        gResolutionLogged = true;
        Log("[d3d12] internal resolution %ux%u (scale %.3f), 2D at %ux%u, shadow maps x%.1f\n", Scaled(1280), Scaled(720), gResTwelfths / 12.0,
            (1280 * gDisplayTwelfths + 6) / 12, (720 * gDisplayTwelfths + 6) / 12, gShadowTwelfths / 12.0);
    }

    // NIER_LOG_PACING=1: one line a frame with where the render thread's time went (ms): waiting for
    // the swap chain to take another frame, recording, the read-backs, Present, the rest; how many
    // frames were queued behind this one and the present interval used.
    int gPaceQueued = -1, gPaceInterval = -1;

    void RenderFrame(const MailboxPlane planes[3], bool movie, uint64_t frame, const RendererFrame* guest)
    {
        LARGE_INTEGER perfEntered;
        QueryPerformanceCounter(&perfEntered);
        WaitForSingleObjectEx(gFrameWait, 1000, TRUE);
        ApplyResolutionSetting();
        LARGE_INTEGER perfStart;
        QueryPerformanceCounter(&perfStart);

        gAllocator->Reset();
        gList->Reset(gAllocator.Get(), nullptr);
        StageBegin();

        if (movie)
        {
            CreateTexturesIfNeeded(planes);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[3];
            UINT64 total = 0;
            for (int i = 0; i < 3; i++)
            {
                auto desc = gTextures[i]->GetDesc();
                UINT64 size = 0;
                gDevice->GetCopyableFootprints(&desc, 0, 1, total, &footprints[i], nullptr, nullptr, &size);
                total = footprints[i].Offset + size;
                total = (total + 511) & ~UINT64(511);
            }
            if (gUploadCapacity < total)
            {
                gUpload = MakeBuffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                gUploadCapacity = total;
            }
            uint8_t* mapped = nullptr;
            gUpload->Map(0, nullptr, (void**)&mapped);
            for (int i = 0; i < 3; i++)
                for (uint32_t y = 0; y < planes[i].height; y++)
                    memcpy(mapped + footprints[i].Offset + y * footprints[i].Footprint.RowPitch, planes[i].data.data() + y * planes[i].width, planes[i].width);
            gUpload->Unmap(0, nullptr);

            for (int i = 0; i < 3; i++)
            {
                D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
                destination.pResource = gTextures[i].Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.pResource = gUpload.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprints[i];
                gList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            D3D12_RESOURCE_BARRIER toRead[3];
            for (int i = 0; i < 3; i++) toRead[i] = Transition(gTextures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
            gList->ResourceBarrier(3, toRead);
        }

        if (guest) ExecuteGuestCommands(*guest);
        StageMark(kStagePresent);

        const UINT index = gSwapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = gBackBuffers[index].Get();
        auto toTarget = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gList->ResourceBarrier(1, &toTarget);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * gRtvStep;
        const FLOAT black[4] = { 0, 0, 0, 1 };
        gList->ClearRenderTargetView(rtv, black, 0, nullptr);
        gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const uint32_t shownSurface = Shown().occurrence ? kSnapshotBase : Shown().base;
        if (guest && (shownSurface || guest->scanoutBase)) BlitScanout(shownSurface && gResolved.count(shownSurface) ? shownSurface : guest->scanoutBase);
        if (guest) DrawOverlay();
        if (guest) DrawFpsCounter();

        if (movie)
        {
            // Letterbox to keep the source's aspect ratio in any window shape.
            float sourceAspect = float(planes[0].width) / planes[0].height;
            float windowAspect = float(gWidth) / gHeight;
            float w = float(gWidth), h = float(gHeight);
            if (windowAspect > sourceAspect) w = h * sourceAspect; else h = w / sourceAspect;
            D3D12_VIEWPORT viewport{ (gWidth - w) * 0.5f, (gHeight - h) * 0.5f, w, h, 0.0f, 1.0f };
            D3D12_RECT scissor{ 0, 0, LONG(gWidth), LONG(gHeight) };
            gList->RSSetViewports(1, &viewport);
            gList->RSSetScissorRects(1, &scissor);
            ID3D12DescriptorHeap* heaps[] = { gSrvHeap.Get() };
            gList->SetDescriptorHeaps(1, heaps);
            gList->SetGraphicsRootSignature(gRootSignature.Get());
            gList->SetPipelineState(gPipeline.Get());
            gList->SetGraphicsRootDescriptorTable(0, gSrvHeap->GetGPUDescriptorHandleForHeapStart());
            gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            gList->DrawInstanced(3, 1, 0, 0);
        }

        if (uint64_t requested = gRequestedSave.exchange(0)) gFramesToSave.insert(requested);
        uint64_t saveLabel = 0;
        bool saving = !gFramesToSave.empty() && frame >= *gFramesToSave.begin();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT saveFootprint{};
        if (saving)
        {
            saveLabel = *gFramesToSave.begin();
            gFramesToSave.erase(gFramesToSave.begin());
            auto desc = backBuffer->GetDesc();
            UINT64 size = 0;
            gDevice->GetCopyableFootprints(&desc, 0, 1, 0, &saveFootprint, nullptr, nullptr, &size);
            if (gReadbackCapacity < size)
            {
                gReadback = MakeBuffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                gReadbackCapacity = size;
            }
            auto toCopy = Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gList->ResourceBarrier(1, &toCopy);
            D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
            destination.pResource = gReadback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = saveFootprint;
            source.pResource = backBuffer;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            gList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }

        auto toPresent = Transition(backBuffer, saving ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        gList->ResourceBarrier(1, &toPresent);
        if (movie)
        {
            D3D12_RESOURCE_BARRIER toCopyDest[3];
            for (int i = 0; i < 3; i++) toCopyDest[i] = Transition(gTextures[i].Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
            gList->ResourceBarrier(3, toCopyDest);
        }
        StageEnd();
        gList->Close();
        LARGE_INTEGER perfRecorded;
        QueryPerformanceCounter(&perfRecorded);

        ID3D12CommandList* lists[] = { gList.Get() };
        gQueue->ExecuteCommandLists(1, lists);
        // Resolves the game reads back (its luminance reduction, from which it sets the exposure of the
        // frames after) go into guest memory as soon as the GPU has made them, before the frame is
        // presented: Present can wait a whole refresh, and with the pixels arriving that much later
        // the game corrected its exposure against a picture several frames old and overshot (a room
        // faded in from black flashed white before settling).
        // NIER_LATE_WRITEBACK=1: after the present, as it used to be (for comparing).
        static const bool lateWriteBack = GetEnvironmentVariableA("NIER_LATE_WRITEBACK", nullptr, 0) > 0;
        LARGE_INTEGER perfWrittenBack = perfRecorded;
        const auto deliverWriteBacks = [&]
        {
            if (!gPendingWriteBacks.empty()) WaitForGpu();
            static const bool logged = GetEnvironmentVariableA("NIER_LOG_WRITEBACK", nullptr, 0) > 0;
            if (logged && !gPendingWriteBacks.empty()) Log("[writeback] from guest frame %llu:\n", gExecutingFrame);
            if (!gPendingWriteBacks.empty())
            {
                void* mapped = nullptr;
                D3D12_RANGE range{ 0, SIZE_T(gWriteBackUsed) };
                if (SUCCEEDED(gWriteBack->Map(0, &range, &mapped)))
                {
                    for (const PendingWriteBack& pending : gPendingWriteBacks)
                    {
                        const uint8_t* host = static_cast<const uint8_t*>(mapped) + pending.offset;
                        const uint32_t width = pending.resolve.width, height = pending.resolve.height;
                        const uint32_t hostWidth = pending.hostWidth, hostHeight = pending.hostHeight;
                        if (hostWidth == width && hostHeight == height) { GpuWriteResolvedPixels(pending.resolve, host, pending.rowPitch); continue; }
                        // The game expects its own size (it reads the luminance reduction to set exposure):
                        // each guest pixel is the average of the host pixels it covers.
                        std::vector<uint8_t> guest(size_t(width) * height * 4);
                        for (uint32_t y = 0; y < height; y++)
                        {
                            const uint32_t y0 = y * hostHeight / height, y1 = (std::min)((std::max)((y + 1) * hostHeight / height, y0 + 1), hostHeight);
                            for (uint32_t x = 0; x < width; x++)
                            {
                                const uint32_t x0 = x * hostWidth / width, x1 = (std::min)((std::max)((x + 1) * hostWidth / width, x0 + 1), hostWidth);
                                uint32_t sum[4] = {}, count = 0;
                                for (uint32_t sy = (std::min)(y0, hostHeight - 1); sy < (std::max)(y1, (std::min)(y0, hostHeight - 1) + 1); sy++)
                                    for (uint32_t sx = (std::min)(x0, hostWidth - 1); sx < (std::max)(x1, (std::min)(x0, hostWidth - 1) + 1); sx++)
                                    {
                                        const uint8_t* texel = host + size_t(sy) * pending.rowPitch + size_t(sx) * 4;
                                        for (int k = 0; k < 4; k++) sum[k] += texel[k];
                                        count++;
                                    }
                                for (int k = 0; k < 4; k++) guest[(size_t(y) * width + x) * 4 + k] = uint8_t((sum[k] + count / 2) / count);
                            }
                        }
                        GpuWriteResolvedPixels(pending.resolve, guest.data(), width * 4);
                    }
                    D3D12_RANGE none{ 0, 0 };
                    gWriteBack->Unmap(0, &none);
                }
                gPendingWriteBacks.clear();
            }
            gWriteBackUsed = 0;
        };
        if (!lateWriteBack) { deliverWriteBacks(); QueryPerformanceCounter(&perfWrittenBack); }
        const bool vsync = Settings().vsync != 0;
        const bool tear = !vsync && (gSwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        // Even frame pacing: on a display refreshing at a multiple of 60 Hz (120, 180, 240) a 60 fps
        // frame presented at the next refresh lasted 3, 4 or 5 refreshes depending on when it happened
        // to be ready, which shows as micro stutter in slow camera moves. Each game frame is instead
        // held for the whole multiple. With frames waiting (the game is ahead) the frame is shown for
        // one refresh, so the delay never builds up. NIER_PRESENT_INTERVAL=<n> forces the count; 1 turns this off (debugging).
        UINT interval = vsync ? 1 : 0;
        if (!vsync) gVblankDivider = 0;
        if (vsync && guest)
        {
            static const UINT forced = [] { char v[8]; return GetEnvironmentVariableA("NIER_PRESENT_INTERVAL", v, sizeof(v)) ? UINT(atoi(v)) : 0u; }();
            static UINT multiple = 1, checked = 0;
            if (checked++ % 120 == 0)   // the window can move to another display
            {
                MONITORINFOEXA info{};
                info.cbSize = sizeof(info);
                DEVMODEA mode{};
                mode.dmSize = sizeof(mode);
                UINT hz = 0;
                if (GetMonitorInfoA(MonitorFromWindow(gWindow, MONITOR_DEFAULTTONEAREST), &info) && EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
                    hz = mode.dmDisplayFrequency;
                const UINT guestHz = UINT(GuestRefreshHz());   // the game's own frame clock: 60, or more; 0 unlocked
                const UINT n = guestHz ? (hz + guestHz / 2) / guestHz : 0;   // unlocked: no fixed relation to the display
                const UINT now = forced ? std::min<UINT>(forced, 4) : (n >= 2 && n <= 4 && hz + 1 >= n * guestHz && hz <= n * guestHz + 1) ? n : 1;
                if (now != multiple || checked == 1) Log("[present] display at %u Hz, game at %u Hz: each frame is held for %u refresh%s\n", hz, guestHz, now, now == 1 ? "" : "es");
                multiple = now;
                gVblankDivider = (guestHz && n >= 1 && n <= 4 && hz + 1 >= n * guestHz && hz <= n * guestHz + 1) ? n : 0;   // uncapped: the vblank doesn't follow the display
                gDisplayHz = hz;
            }
            bool waiting;
            size_t queued;
            {
                std::lock_guard lock(gMailboxMutex);
                queued = gFrameQueue.size();
                waiting = queued != 0;
            }
            if (!waiting && Settings().frameRate60) interval = multiple;
            // With the game's rate the display's own (60 on 60 Hz), "one refresh" is all a frame ever
            // gets, and frames that queued up while the renderer was held (a load, a slow frame) stayed
            // queued for good: the game then ran 4 to 5 frames ahead of the picture, so its controls
            // lagged and it set its exposure from a measurement that old (it overshot: a room fading in
            // flashed white). Such a frame is presented without waiting for the refresh, which drops
            // one from view and brings the game back level. NIER_KEEP_QUEUE=1 leaves them queued.
            // NIER_QUEUE_CUSHION=<n> (testing): that many frames may stay queued (default 0). In play the game is
            // a little late with a frame several times in ten seconds and catches up with the next;
            // with nothing queued each of those shows as one picture repeated and one skipped.
            static const bool keepQueue = GetEnvironmentVariableA("NIER_KEEP_QUEUE", nullptr, 0) > 0;
            static const size_t cushion = [] { char v[8]; return GetEnvironmentVariableA("NIER_QUEUE_CUSHION", v, sizeof(v)) ? size_t(atoi(v)) : size_t(0); }();
            static uint32_t skipped = 0, starved = 0, counted = 0;
            if (queued > cushion && interval == 1 && !keepQueue) { interval = 0; skipped++; }
            if (queued == 0) starved++;
            gPaceQueued = int(queued);
            gPaceInterval = int(interval);
            if (++counted == 600)
            {
                Log("[present] last 600 frames: %u shown with none queued behind them, %u skipped from view to shorten the queue (cushion %zu)\n", starved, skipped, cushion);
                skipped = starved = counted = 0;
            }
        }
        HRESULT presented = gSwapChain->Present(interval, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (FAILED(presented))
        {
            Log("[d3d12] present failed: 0x%08lX, device status 0x%08lX\n", presented, gDevice->GetDeviceRemovedReason());
            gReady = false;
            return;
        }
        gPresented++;
        if (movie) RecordImageTiming(planes[0]);
        LARGE_INTEGER perfPresented;
        QueryPerformanceCounter(&perfPresented);
        static const bool logPacing = GetEnvironmentVariableA("NIER_LOG_PACING", nullptr, 0) > 0;
        if (logPacing && guest)
        {
            static LARGE_INTEGER last{}, frequency{};
            if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
            const double k = 1000.0 / double(frequency.QuadPart);
            Log("[pace] %llu: %.1f since the last present; swap chain %.1f, recording %.1f, read-backs %.1f, present %.1f; %d queued, interval %d\n", (unsigned long long)frame,
                last.QuadPart ? double(perfPresented.QuadPart - last.QuadPart) * k : 0.0, double(perfStart.QuadPart - perfEntered.QuadPart) * k,
                double(perfRecorded.QuadPart - perfStart.QuadPart) * k, double(perfWrittenBack.QuadPart - perfRecorded.QuadPart) * k,
                double(perfPresented.QuadPart - perfWrittenBack.QuadPart) * k, gPaceQueued, gPaceInterval);
            last = perfPresented;
        }
        WaitForGpu();
        gRetired.clear();   // the frame's GPU work is complete
        if (lateWriteBack) deliverWriteBacks();
        {
            // Every 600 frames: the frame rate and where the time went, on average: the render thread's
            // (it waits for the GPU to finish each frame, so that wait is the GPU's time unless the
            // present itself waited: VSync) and the GPU's by stage. The same sums feed RendererPerfTake.
            static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
            static int64_t since = 0, slowest = 0;
            static RendererPerf logged;
            LARGE_INTEGER perfDone;
            QueryPerformanceCounter(&perfDone);
            const double ms = 1000.0 / double(frequency.QuadPart);
            double stages[kRendererPerfStages] = {};
            StageCollect(stages);
            const int mode = UpscalingMode();
            {
                std::lock_guard lock(gPerfMutex);
                for (RendererPerf* perf : { &logged, &gPerfTotals })
                {
                    perf->frames++;
                    perf->recordMs += double(perfRecorded.QuadPart - perfStart.QuadPart) * ms;
                    perf->presentMs += double(perfPresented.QuadPart - perfWrittenBack.QuadPart) * ms;
                    perf->gpuWaitMs += double(perfDone.QuadPart - perfPresented.QuadPart + perfWrittenBack.QuadPart - perfRecorded.QuadPart) * ms;
                    for (int i = 0; i < kRendererPerfStages; i++) perf->stageMs[i] += stages[i];
                    perf->slowestMs = (std::max)(perf->slowestMs, double(perfDone.QuadPart - perfStart.QuadPart) * ms);
                }
                gPerfTotals.renderWidth = Scaled(1280);
                gPerfTotals.renderHeight = Scaled(720);
                gPerfTotals.windowWidth = gWidth;
                gPerfTotals.windowHeight = gHeight;
                snprintf(gPerfTotals.upscaler, sizeof(gPerfTotals.upscaler), "%s", mode == 0 ? "-" : mode == 1 || !gTemporal.history ? "FSR 1" : gTemporalName.c_str());
            }
            slowest = std::max<int64_t>(slowest, perfDone.QuadPart - perfStart.QuadPart);
            if (since == 0) since = perfStart.QuadPart;
            if (logged.frames == 600)
            {
                char gpu[256] = "";
                for (int i = 0, at = 0; i < kRendererPerfStages; i++)
                    if (logged.stageMs[i] / 600.0 >= 0.005) at += snprintf(gpu + at, sizeof(gpu) - at, "%s%s %.2f", at ? ", " : "", kStageNames[i], logged.stageMs[i] / 600.0);
                Log("[perf] frame %llu: %.1f fps; render thread %.2f ms a frame (recording %.2f, present %.2f, waiting for the GPU %.2f; slowest %.1f); GPU: %s; %ux%u shown at %ux%u, upscaling %s\n",
                    frame, 600.0 * double(frequency.QuadPart) / double(perfDone.QuadPart - since), (logged.recordMs + logged.presentMs + logged.gpuWaitMs) / 600.0,
                    logged.recordMs / 600.0, logged.presentMs / 600.0, logged.gpuWaitMs / 600.0, double(slowest) * ms, gpu,
                    Scaled(1280), Scaled(720), gWidth, gHeight, gPerfTotals.upscaler);
                since = perfDone.QuadPart;
                slowest = 0;
                logged = RendererPerf{};
            }
        }
        if (saving)
        {
            WriteSavedFrame(saveLabel, saveFootprint);
            if (Shown().base) Log("[d3d12] saved frame %llu (guest frame %llu) shows %08X@%u\n", saveLabel, gExecutingFrame, Shown().base, Shown().occurrence);
        }
    }

    void Resize(uint32_t width, uint32_t height)
    {
        if (!width || !height) return;
        WaitForGpu();
        for (auto& buffer : gBackBuffers) buffer.Reset();
        if (!Check(gSwapChain->ResizeBuffers(kBackBuffers, width, height, kBackBufferFormat, gSwapChainFlags), "resize")) return;
        gWidth = width;
        gHeight = height;
        AcquireBackBuffers();
    }

    void RenderThread()
    {
        if (!CreateDeviceObjects())
        {
            Log("[d3d12] renderer unavailable; the game will run without a picture\n");
            return;
        }
        gReady = true;

        MailboxPlane planes[3];
        uint64_t seen = 0;
        while (gReady)
        {
            bool movie = false, hasWork = false;
            uint64_t frame = 0;
            uint32_t resizeWidth = 0, resizeHeight = 0;
            bool resize = false;
            std::optional<RendererFrame> guest;
            {
                std::unique_lock lock(gMailboxMutex);
                gMailboxChanged.wait(lock, [&] { return gMailboxSequence != seen || gResizePending || !gFrameQueue.empty(); });
                resize = gResizePending;
                resizeWidth = gResizeWidth;
                resizeHeight = gResizeHeight;
                gResizePending = false;
                if (!gFrameQueue.empty())
                {
                    guest = std::move(gFrameQueue.front());
                    gFrameQueue.pop_front();
                    frame = guest->frame;
                    hasWork = true;
                    gQueueSpace.notify_one();
                }
                else if (gMailboxSequence != seen)
                {
                    seen = gMailboxSequence;
                    movie = gMailboxMovie;
                    frame = gMailboxFrame;
                    hasWork = true;
                    if (movie) for (int i = 0; i < 3; i++) planes[i] = gPlanes[i];
                }
            }
            if (resize) Resize(resizeWidth, resizeHeight);
            if (hasWork) RenderFrame(planes, movie, frame, guest ? &*guest : nullptr);
            if (guest)
            {
                // The frame's commands are done with: empty the list and keep it for a later frame.
                guest->commands.clear();
                std::lock_guard lock(gCommandListMutex);
                if (gCommandLists.size() < 6) gCommandLists.push_back(std::move(guest->commands));
            }
        }
        std::lock_guard lock(gMailboxMutex);
        gFrameQueue.clear();
        gQueueSpace.notify_all();
    }

    void CopyPlane(MailboxPlane& out, const RendererPlane& in)
    {
        out.width = in.width;
        out.height = in.height;
        out.data.resize(size_t(in.width) * in.height);
        for (uint32_t y = 0; y < in.height; y++) memcpy(out.data.data() + size_t(y) * in.width, in.data + size_t(y) * in.pitch, in.width);
    }
}

void RendererInit(void* nativeWindow, uint32_t width, uint32_t height)
{
    gWindow = static_cast<HWND>(nativeWindow);
    gWidth = width;
    gHeight = height;

    char* list = nullptr;
    size_t length = 0;
    _dupenv_s(&list, &length, "NIER_SAVE_FRAMES");
    if (list)
    {
        char* context = nullptr;
        for (char* token = strtok_s(list, ",", &context); token; token = strtok_s(nullptr, ",", &context))
            gFramesToSave.insert(strtoull(token, nullptr, 10));
    }

    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode)) Log("[d3d12] primary display refresh: %lu Hz\n", mode.dmDisplayFrequency);
    std::thread(RenderThread).detach();
}

void RendererResize(uint32_t width, uint32_t height)
{
    {
        std::lock_guard lock(gMailboxMutex);
        gResizeWidth = width;
        gResizeHeight = height;
        gResizePending = true;
    }
    gMailboxChanged.notify_one();
}

void RendererSubmitMovieFrame(uint64_t frame, const RendererPlane& y, const RendererPlane& u, const RendererPlane& v)
{
    gSubmitted++;
    {
        std::lock_guard lock(gMailboxMutex);
        CopyPlane(gPlanes[0], y);
        CopyPlane(gPlanes[1], u);
        CopyPlane(gPlanes[2], v);
        gMailboxMovie = true;
        gMailboxFrame = frame;
        gMailboxSequence++;
    }
    gMailboxChanged.notify_one();
}

// Guest frames are queued rather than replaced: a dropped frame could lose a one-time resolve.
std::vector<RendererCommand> RendererTakeCommandList()
{
    std::lock_guard lock(gCommandListMutex);
    if (gCommandLists.empty()) return {};
    std::vector<RendererCommand> list = std::move(gCommandLists.back());
    gCommandLists.pop_back();
    return list;
}

void RendererSubmitFrame(RendererFrame&& frame)
{
    gSubmitted++;
    {
        std::unique_lock lock(gMailboxMutex);
        gQueueSpace.wait(lock, [] { return gFrameQueue.size() < 3 || !gReady; });
        if (!gReady) return;
        gFrameQueue.push_back(std::move(frame));
    }
    gMailboxChanged.notify_one();
}

void RendererDumpStats()
{
    Log("[present] %llu frames submitted, %llu presented\n", gSubmitted.load(), gPresented.load());
    Log("[d3d12] %llu draws executed, %llu failed, %zu pipelines, %zu textures, %zu render targets, %zu resolve surfaces\n", gDrawsExecuted.load(), gDrawsFailed.load(), gPipelines.size(), gDecoded.size(), gTargets.size(), gResolved.size());
    Log("[d3d12] waited for shader compiles: %.0f ms in total, %llu waits over 1 ms, worst %.1f ms\n", gShaderWaitMs.load(), gShaderWaits.load(), gShaderWaitWorstMs.load());
    Log("[d3d12] creating %zu pipelines took %.0f ms, %llu over 5 ms, worst %.1f ms\n", gPipelines.size(), gPipelineMs.load(), gPipelineSlow.load(), gPipelineWorstMs.load());
    Log("[d3d12] render thread waited for pipelines: %.0f ms in total, %llu waits over 1 ms\n", gPipelineWaitMs.load(), gPipelineWaits.load());
    Log("[d3d12] pipelines not ready in time: %llu, with %llu draws left out until they were\n", gPipelinesDeferred.load(), gDrawsDeferred.load());
    if (gMovieImages.load())
    {
        Log("[present] %llu new images on screen: worst gap %.1fms, %llu gaps over 60ms\n", gMovieImages.load(), gWorstGap, gLateImages.load());
        Log("[present] gaps in refreshes: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7+:%llu\n", gGapBins[0], gGapBins[1], gGapBins[2], gGapBins[3], gGapBins[4], gGapBins[5], gGapBins[6], gGapBins[7]);
    }
}

// Waits for the display refreshes that make up one 60 Hz period, so that the game's vblank keeps
// step with the display itself (two free-running 60 Hz clocks drift apart, and each time they
// cross a frame is shown too long or too briefly). False when that isn't possible (the rate isn't
// a multiple of 60, vsync is off, the display is asleep): the caller then keeps its own time.
bool RendererWaitDisplayVblank()
{
    static const bool off = GetEnvironmentVariableA("NIER_NO_VBLANK_LOCK", nullptr, 0) > 0;   // debugging
    const UINT divider = gVblankDivider.load();
    if (off || !divider || !gReady || !gSwapChain) return false;
    static ComPtr<IDXGIOutput> output;   // only the vblank thread comes here
    static uint32_t uses = 0;
    if (!output || uses++ % 240 == 0)    // the window can move to another display
    {
        output.Reset();
        if (FAILED(gSwapChain->GetContainingOutput(&output))) return false;
    }
    LARGE_INTEGER frequency, begin, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    for (UINT i = 0; i < divider; i++)
        if (FAILED(output->WaitForVBlank())) { output.Reset(); return false; }
    QueryPerformanceCounter(&end);
    const double ms = double(end.QuadPart - begin.QuadPart) * 1000.0 / double(frequency.QuadPart);
    static bool logged = false;
    const double expected = 1000.0 / (GuestRefreshHz() > 0 ? GuestRefreshHz() : int(std::max<UINT>(gDisplayHz.load(), 1)));   // unlocked: the display's own period
    // With one refresh to wait for, the wait is as short as the time left to it; only an output that
    // returns at once (no real wait) is refused.
    const double least = divider > 1 ? expected * 0.5 : 0.3;
    if (!logged && ms >= least && ms <= expected * 2.5) { logged = true; Log("[present] the game's vblank follows the display (every %u refresh%s)\n", divider, divider == 1 ? "" : "es"); }
    return ms >= least && ms <= expected * 2.5;   // an output that doesn't really wait returns at once
}

void RendererSetOverlay(const uint8_t* rgba, uint32_t width, uint32_t height)
{
    std::shared_ptr<DecodedTexture> overlay;
    if (rgba && width && height)
    {
        static std::atomic<uint64_t> version{ 0 };
        overlay = std::make_shared<DecodedTexture>();
        overlay->key = 0xFFFFFFFFFFFF0F5Cull;
        overlay->contentHash = ++version;
        overlay->width = width;
        overlay->height = height;
        overlay->rowPitch = width * 4;
        overlay->data.assign(rgba, rgba + size_t(width) * height * 4);
        overlay->subresources.push_back({ 0, width * 4, height });
    }
    std::lock_guard lock(gOverlayMutex);
    gOverlay = overlay;
}

uint32_t RendererDisplayHz() { return gDisplayHz.load(); }
void RendererSaveFrame(uint64_t frame) { gRequestedSave = frame; }
bool RendererReady() { return gReady; }
void RendererWaitTotals(double totals[3]) { totals[0] = gShaderWaitMs.load(); totals[1] = gPipelineMs.load(); totals[2] = gPipelineWaitMs.load(); }
void RendererPrepareShader(const std::shared_ptr<GuestShader>& shader) { QueueShader(shader); }
void RendererSetSynchronousPipelines(bool on) { gSynchronousPipelines = on; }

// Starts creating the pipeline a draw needs, if this combination is new (GPU emulation thread).
void RendererPreparePipeline(const RendererDraw& draw)
{
    if (!draw.vs || !draw.ps || !gReady) return;   // device and draw objects exist once ready
    static thread_local std::unordered_set<uint64_t> seen;   // cheap check before the shared map
    const PipelineState state = PipelineStateOf(draw);
    const uint64_t key = PipelineKey(state);
    if (!seen.insert(key).second) return;
    QueuePipeline(state, key);

    // Every combination the game uses is noted in recomp/pipelines_seen.txt (kept across runs, not
    // committed): tools/pipeline_list turns it into the list that prewarm.cpp replays at boot.
    static std::unordered_set<uint64_t> recorded;
    static FILE* file = [] {
        if (FILE* old = fopen("..\\pipelines_seen.txt", "r"))
        {
            char line[160];
            unsigned long long vs, ps;
            unsigned blend, mask, depth, stencil, flags;
            while (fgets(line, sizeof(line), old))
                if (sscanf(line, "P %llx %llx %x %x %x %x %x", &vs, &ps, &blend, &mask, &depth, &stencil, &flags) == 7)
                    recorded.insert(XXH3_64bits(line, strcspn(line, "\r\n")));
            fclose(old);
        }
        return fopen("..\\pipelines_seen.txt", "a");
    }();
    if (!file) return;
    char line[160];
    const uint32_t flags = (state.rectList ? 1 : 0) | (state.hasColor ? 2 : 0) | (state.depthBound ? 4 : 0) | (uint32_t(state.cull) << 3) | (state.frontCounterClockwise ? 32 : 0);
    const int length = snprintf(line, sizeof(line), "P %016llX %016llX %08X %X %08X %08X %X", (unsigned long long)state.vs->hash, (unsigned long long)state.ps->hash,
                                state.blendControl, state.writeMask, state.depthControl, state.stencilRefMask & 0xFFFF00, flags);
    if (recorded.insert(XXH3_64bits(line, size_t(length))).second) { fprintf(file, "%s\n", line); fflush(file); }
}

std::atomic<uint64_t> gPrewarmedShaders{ 0 }, gPrewarmedPipelines{ 0 };

void RendererPrewarmShader(const std::shared_ptr<GuestShader>& shader)
{
    gPrewarmedShaders++;
    QueueShader(shader, false);
}

void RendererPrewarmPipeline(const std::shared_ptr<GuestShader>& vs, const std::shared_ptr<GuestShader>& ps, uint32_t blendControl, uint32_t writeMask,
                             uint32_t depthControl, uint32_t stencilRefMask, uint32_t flags)
{
    if (!vs || !ps || !gReady) return;
    PipelineState state;
    state.vs = vs;
    state.ps = ps;
    state.blendControl = blendControl;
    state.writeMask = writeMask;
    state.depthControl = depthControl;
    state.stencilRefMask = stencilRefMask;
    state.rectList = flags & 1;
    state.hasColor = (flags >> 1) & 1;
    state.depthBound = (flags >> 2) & 1;
    state.cull = uint8_t((flags >> 3) & 3);
    state.frontCounterClockwise = (flags >> 5) & 1;
    gPrewarmedPipelines++;
    QueuePipeline(state, PipelineKey(state), false);
}

// Shaders compiled and pipelines created so far, of those asked for (the list's and the game's).
void RendererPrewarmProgress(uint32_t& done, uint32_t& total)
{
    done = gPipelineJobsDone.load() + gShaderJobsDone.load();
    total = gPipelineJobsQueued.load() + gShaderJobsQueued.load();
}

// On: extra shader and pipeline workers, for as long as the game itself is not running. Off: they
// finish what they hold and leave.
void RendererPrewarmBoost(bool on)
{
    if (on && !gPrewarmBoost.exchange(true))
    {
        const unsigned threads = std::thread::hardware_concurrency();
        const unsigned extra = threads > 6 ? (threads - 4) / 2 : 1;   // of each kind, beside the usual 2-6 and 2-4
        for (unsigned i = 0; i < extra; i++)
        {
            std::thread(ShaderWorker, true).detach();
            std::thread(PipelineWorker, true).detach();
        }
    }
    else if (!on && gPrewarmBoost.exchange(false))
    {
        { std::lock_guard lock(gShaderJobs.mutex); }
        gShaderJobs.work.notify_all();
        { std::lock_guard lock(gPipelineJobs.mutex); }
        gPipelineJobs.work.notify_all();
    }
}

bool RendererFsr4Available()
{
    if (!gDevice) return false;
    static const bool available = [] {
        HMODULE library = LoadLibraryA("amd_fidelityfx_loader_dx12.dll");
        const PfnFfxQuery query = library ? (PfnFfxQuery)GetProcAddress(library, "ffxQuery") : nullptr;
        if (!query) return false;
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = gDevice.Get();
        versions.outputCount = &count;
        if (query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) return false;
        std::vector<const char*> names(count);
        versions.versionNames = names.data();
        if (query(nullptr, &versions.header) != FFX_API_RETURN_OK) return false;
        bool found = false;
        for (uint64_t i = 0; i < count; i++) found = found || (names[i] && atoi(names[i]) >= 4);
        Log("[fsr3] FSR 4 %s on this system\n", found ? "is available" : "is not available: the Upscaling setting leaves it out");
        return found;
    }();
    return available;
}

RendererPerf RendererPerfTake()
{
    std::lock_guard lock(gPerfMutex);
    RendererPerf taken = gPerfTotals;
    snprintf(taken.adapter, sizeof(taken.adapter), "%s", gAdapterName.c_str());
    gPerfTotals = RendererPerf{};
    return taken;
}

const char* RendererPerfStageName(int stage) { return stage >= 0 && stage < kRendererPerfStages ? kStageNames[stage] : ""; }
