// D3D12 backend. A dedicated thread takes the newest submitted frame and presents it with vsync,
// so waiting for the display never holds up the emulated GPU.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <variant>
#include <string>
#include <thread>
#include <vector>
#include "guest.h"
#include "renderer.h"

using Microsoft::WRL::ComPtr;

namespace
{
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
    MailboxPlane gPlanes[3];
    bool gMailboxMovie = false;
    uint64_t gMailboxFrame = 0, gMailboxSequence = 0;
    uint32_t gResizeWidth = 0, gResizeHeight = 0;
    bool gResizePending = false;

    HWND gWindow = nullptr;
    uint32_t gWidth = 1280, gHeight = 720;
    bool gReady = false;

    ComPtr<ID3D12Device> gDevice;
    ComPtr<ID3D12CommandQueue> gQueue;
    ComPtr<IDXGISwapChain3> gSwapChain;
    ComPtr<ID3D12DescriptorHeap> gRtvHeap, gSrvHeap;
    ComPtr<ID3D12CommandAllocator> gAllocator;
    ComPtr<ID3D12GraphicsCommandList> gList;
    ComPtr<ID3D12RootSignature> gRootSignature;
    ComPtr<ID3D12PipelineState> gPipeline;
    ComPtr<ID3D12Fence> gFence;
    ComPtr<ID3D12Resource> gBackBuffers[kBackBuffers];
    ComPtr<ID3D12Resource> gTextures[3];
    ComPtr<ID3D12Resource> gUpload, gReadback;
    UINT64 gUploadCapacity = 0, gReadbackCapacity = 0;
    UINT gRtvStep = 0;
    UINT64 gFenceValue = 0;
    HANDLE gFenceEvent = nullptr, gFrameWait = nullptr;

    std::set<uint64_t> gFramesToSave;
    std::atomic<uint64_t> gSubmitted{ 0 }, gPresented{ 0 }, gMovieImages{ 0 }, gLateImages{ 0 };
    uint64_t gGapBins[8];
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
    constexpr UINT kDrawDescriptors = 65536, kTargetSlots = 256;
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
Texture2D<float4> source : register(t0);
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
)";

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
    };
    struct SystemConstants
    {
        float ndcScale[2], ndcOffset[2];
        uint32_t bypassViewport, padding[3];
        uint32_t vertexBase[96];
        uint32_t samplerPoint[16];
        float texScale[16][4];
        uint32_t boolConstants[8];
    };
    struct ArenaBlock { uint8_t* cpu; D3D12_GPU_VIRTUAL_ADDRESS gpu; UINT64 offset; };

    ComPtr<ID3D12RootSignature> gDrawRoot;
    ComPtr<ID3D12PipelineState> gBlitPipeline;
    ComPtr<ID3D12DescriptorHeap> gDrawSrvHeap, gTargetRtvHeap;
    ComPtr<ID3D12Resource> gArena;
    ComPtr<ID3DBlob> gRectBlob;
    uint8_t* gArenaCpu = nullptr;
    UINT64 gArenaOffset = 0;
    UINT gDrawSrvUsed = 0, gSrvStep = 0, gTargetRtvStep = 0, gTargetsCreated = 0;
    std::unordered_map<uint32_t, TargetResource> gTargets;
    std::unordered_map<uint32_t, ResolvedResource> gResolved;
    std::unordered_map<uint64_t, ComPtr<ID3D12Resource>> gDecoded;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> gShaderBlobs;
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
        range.NumDescriptors = 16;
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2].Descriptor.ShaderRegister = 16;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 1;
        parameters[3].DescriptorTable.pDescriptorRanges = &range;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1] = samplers[0];
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 4;
        signature.pParameters = parameters;
        signature.NumStaticSamplers = 2;
        signature.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> blob, errors;
        if (!Check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "draw root signature serialize")) return false;
        if (!Check(gDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gDrawRoot)), "draw root signature")) return false;

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
        return Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&gBlitPipeline)), "blit pipeline");
    }

    ID3DBlob* ShaderBlob(const GuestShader& shader)
    {
        auto found = gShaderBlobs.find(shader.hash);
        if (found != gShaderBlobs.end()) return found->second.Get();
        ComPtr<ID3DBlob> blob, errors;
        HRESULT result = D3DCompile(shader.hlsl.data(), shader.hlsl.size(), "guest", nullptr, nullptr, "main", shader.pixel ? "ps_5_1" : "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &errors);
        if (FAILED(result))
        {
            Log("[shader] %s %016llX failed to compile: %s\n", shader.pixel ? "pixel" : "vertex", shader.hash, errors ? (const char*)errors->GetBufferPointer() : "unknown");
            blob.Reset();
        }
        gShaderBlobs[shader.hash] = blob;
        return blob.Get();
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

    ID3D12PipelineState* PipelineFor(const RendererDraw& draw)
    {
        uint64_t key = draw.vs->hash * 31 + draw.ps->hash;
        key = (key ^ draw.blendControl) * 1099511628211ull;
        key = (key ^ (uint64_t(draw.writeMask) << 1) ^ (draw.rectList ? 1 : 0)) * 1099511628211ull;
        auto found = gPipelines.find(key);
        if (found != gPipelines.end()) return found->second.Get();

        ID3DBlob* vs = ShaderBlob(*draw.vs), * ps = ShaderBlob(*draw.ps);
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
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = kTargetFormat;
            desc.SampleDesc.Count = 1;
            Check(gDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "guest pipeline");
        }
        gPipelines[key] = pipeline;
        return pipeline.Get();
    }

    TargetResource* GetTarget(const RenderTargetKey& key)
    {
        if (!key.width || !key.height) return nullptr;
        auto found = gTargets.find(key.edramBase);
        if (found != gTargets.end() && found->second.width == key.width && found->second.height == key.height) return &found->second;

        TargetResource created;
        created.rtvIndex = found != gTargets.end() ? found->second.rtvIndex : gTargetsCreated++;
        if (created.rtvIndex >= kTargetSlots) return nullptr;
        created.width = key.width;
        created.height = key.height;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = key.width;
        desc.Height = key.height;
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
        Log("[d3d12] render target at EDRAM 0x%X: %ux%u\n", key.edramBase, key.width, key.height);
        return &(gTargets[key.edramBase] = std::move(created));
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
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    ID3D12Resource* GetDecoded(const DecodedTexture& decoded)
    {
        auto found = gDecoded.find(decoded.key);
        if (found != gDecoded.end()) return found->second.Get();

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = decoded.width;
        desc.Height = decoded.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DecodedFormat(decoded.format);
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> texture;
        if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "guest texture")) return nullptr;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT rows = 0;
        UINT64 total = 0;
        gDevice->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, nullptr, &total);
        ArenaBlock block = ArenaAlloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        for (UINT row = 0; row < rows; row++)
            memcpy(block.cpu + UINT64(row) * footprint.Footprint.RowPitch, decoded.data.data() + size_t(row) * decoded.rowPitch, decoded.rowPitch);
        footprint.Offset = block.offset;
        D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.pResource = gArena.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        gList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        auto barrier = Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gList->ResourceBarrier(1, &barrier);
        gDecoded[decoded.key] = texture;
        return texture.Get();
    }

    void WriteTextureDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const TextureBinding& binding)
    {
        ID3D12Resource* resource = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool swap = false;
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
                swap = found->second.swapRedBlue;
            }
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        // A resolve that swapped red and blue changes which channel each guest component reads.
        auto channel = [&](int i) { uint32_t c = binding.swizzle[i]; return (swap && (c == 0 || c == 2)) ? 2 - c : c; };
        view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(channel(0), channel(1), channel(2), channel(3));
        gDevice->CreateShaderResourceView(resource, &view, handle);
    }

    void ExecuteDraw(const RendererDraw& draw)
    {
        ID3D12PipelineState* pipeline = PipelineFor(draw);
        TargetResource* target = GetTarget(draw.target);
        if (!pipeline || !target || !draw.vertexCount) { gDrawsFailed++; return; }
        SetTargetState(*target, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ArenaBlock constants = ArenaAlloc(draw.constants.size() * 4, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        memcpy(constants.cpu, draw.constants.data(), draw.constants.size() * 4);

        ArenaBlock system = ArenaAlloc(sizeof(SystemConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        auto* s = reinterpret_cast<SystemConstants*>(system.cpu);
        memset(s, 0, sizeof(*s));
        memcpy(s->ndcScale, draw.ndcScale, sizeof(s->ndcScale));
        memcpy(s->ndcOffset, draw.ndcOffset, sizeof(s->ndcOffset));
        s->bypassViewport = draw.bypassViewport ? 1 : 0;
        memcpy(s->vertexBase, draw.vertexBase.data(), sizeof(s->vertexBase));
        memcpy(s->boolConstants, draw.boolConstants.data(), sizeof(s->boolConstants));

        UINT64 vertexBytes = std::max<UINT64>(draw.vertexData.size(), 16);
        ArenaBlock vertices = ArenaAlloc(vertexBytes, 16);
        if (!draw.vertexData.empty()) memcpy(vertices.cpu, draw.vertexData.data(), draw.vertexData.size());

        const UINT firstDescriptor = gDrawSrvUsed;
        gDrawSrvUsed += 16;
        if (gDrawSrvUsed > kDrawDescriptors) { gDrawSrvUsed = 16; gDrawsFailed++; return; }
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(firstDescriptor) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(firstDescriptor) * gSrvStep;
        for (int i = 0; i < 16; i++)
        {
            const TextureBinding& binding = draw.textures[i];
            s->samplerPoint[i] = binding.point ? 1 : 0;
            s->texScale[i][0] = binding.scale[0];
            s->texScale[i][1] = binding.scale[1];
            WriteTextureDescriptor(cpu, binding);
            cpu.ptr += gSrvStep;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gTargetRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(target->rtvIndex) * gTargetRtvStep;
        gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport{ draw.viewport[0], draw.viewport[1], draw.viewport[2], draw.viewport[3], 0.0f, 1.0f };
        D3D12_RECT scissor{ draw.scissor[0], draw.scissor[1], draw.scissor[2], draw.scissor[3] };
        gList->RSSetViewports(1, &viewport);
        gList->RSSetScissorRects(1, &scissor);
        gList->SetGraphicsRootSignature(gDrawRoot.Get());
        gList->SetPipelineState(pipeline);
        gList->SetGraphicsRootConstantBufferView(0, constants.gpu);
        gList->SetGraphicsRootConstantBufferView(1, system.gpu);
        gList->SetGraphicsRootShaderResourceView(2, vertices.gpu);
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
        }
        gDrawsExecuted++;
    }

    void ExecuteResolve(const RendererResolve& resolve)
    {
        TargetResource* source = GetTarget(resolve.source);
        if (!source) return;
        const uint32_t width = std::min(resolve.width, source->width), height = std::min(resolve.height, source->height);
        if (!width || !height) return;

        ResolvedResource& destination = gResolved[resolve.destBase];
        bool created = false;
        if (!destination.texture || destination.width != width || destination.height != height)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kTargetFormat;
            desc.SampleDesc.Count = 1;
            destination.texture.Reset();
            if (!Check(gDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&destination.texture)), "resolve texture")) return;
            destination.width = width;
            destination.height = height;
            created = true;
        }
        destination.swapRedBlue = resolve.swapRedBlue;

        SetTargetState(*source, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!created)
        {
            auto toCopy = Transition(destination.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            gList->ResourceBarrier(1, &toCopy);
        }
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = destination.texture.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = source->texture.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{ 0, 0, 0, width, height, 1 };
        gList->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        auto toRead = Transition(destination.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gList->ResourceBarrier(1, &toRead);
    }

    void ExecuteGuestCommands(const RendererFrame& frame)
    {
        gArenaOffset = 0;
        gDrawSrvUsed = 0;
        ID3D12DescriptorHeap* heaps[] = { gDrawSrvHeap.Get() };
        gList->SetDescriptorHeaps(1, heaps);
        for (const auto& command : frame.commands)
        {
            if (auto* draw = std::get_if<RendererDraw>(&command)) ExecuteDraw(*draw);
            else ExecuteResolve(std::get<RendererResolve>(command));
        }
    }

    // Shows the scanout surface in the back buffer. It's in render target channel order, so no swap.
    void BlitScanout(uint32_t base)
    {
        auto found = gResolved.find(base);
        if (found == gResolved.end()) return;
        const UINT slot = gDrawSrvUsed;
        gDrawSrvUsed += 16;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = gDrawSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * gSrvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gDrawSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(slot) * gSrvStep;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = kTargetFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
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
        gList->SetGraphicsRootSignature(gDrawRoot.Get());
        gList->SetPipelineState(gBlitPipeline.Get());
        gList->SetGraphicsRootDescriptorTable(3, gpu);
        gList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gList->DrawInstanced(3, 1, 0, 0);
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
        if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gDevice)), "device")) return false;

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
        swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
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

    void RenderFrame(const MailboxPlane planes[3], bool movie, uint64_t frame, const RendererFrame* guest)
    {
        WaitForSingleObjectEx(gFrameWait, 1000, TRUE);

        gAllocator->Reset();
        gList->Reset(gAllocator.Get(), nullptr);

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
            for (int i = 0; i < 3; i++) toRead[i] = Transition(gTextures[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            gList->ResourceBarrier(3, toRead);
        }

        if (guest) ExecuteGuestCommands(*guest);

        const UINT index = gSwapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = gBackBuffers[index].Get();
        auto toTarget = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gList->ResourceBarrier(1, &toTarget);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * gRtvStep;
        const FLOAT black[4] = { 0, 0, 0, 1 };
        gList->ClearRenderTargetView(rtv, black, 0, nullptr);
        gList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        if (guest && guest->scanoutBase) BlitScanout(guest->scanoutBase);

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
            for (int i = 0; i < 3; i++) toCopyDest[i] = Transition(gTextures[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            gList->ResourceBarrier(3, toCopyDest);
        }
        gList->Close();

        ID3D12CommandList* lists[] = { gList.Get() };
        gQueue->ExecuteCommandLists(1, lists);
        HRESULT presented = gSwapChain->Present(1, 0);
        if (FAILED(presented))
        {
            Log("[d3d12] present failed: 0x%08lX, device status 0x%08lX\n", presented, gDevice->GetDeviceRemovedReason());
            gReady = false;
            return;
        }
        gPresented++;
        if (movie) RecordImageTiming(planes[0]);
        WaitForGpu();
        if (saving) WriteSavedFrame(saveLabel, saveFootprint);
    }

    void Resize(uint32_t width, uint32_t height)
    {
        if (!width || !height) return;
        WaitForGpu();
        for (auto& buffer : gBackBuffers) buffer.Reset();
        if (!Check(gSwapChain->ResizeBuffers(kBackBuffers, width, height, kBackBufferFormat, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "resize")) return;
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
    if (gMovieImages.load())
    {
        Log("[present] %llu new images on screen: worst gap %.1fms, %llu gaps over 60ms\n", gMovieImages.load(), gWorstGap, gLateImages.load());
        Log("[present] gaps in refreshes: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6:%llu 7+:%llu\n", gGapBins[0], gGapBins[1], gGapBins[2], gGapBins[3], gGapBins[4], gGapBins[5], gGapBins[6], gGapBins[7]);
    }
}
