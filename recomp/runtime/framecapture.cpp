// Frame captures (see framecapture.h).
//
// File layout, little-endian: "NFRM", version, frame, scanout base; the shaders (hash, stage,
// microcode words, HLSL); the textures (DecodedTexture fields and data); the commands, each a type
// byte (0 draw, 1 resolve) and its fields, with shaders and textures referred to by index.
//
// Replay (NIER_REPLAY_FRAME=<file>): the shaders are translated again from their microcode, so
// translator changes, NIER_PS_DEBUG and NIER_SHADER_PATCH apply; the frame is submitted repeatedly
// (textures resolved by one pass feed the next, as from the previous frame in the game). Options:
//   NIER_REPLAY_FRAMES=<n>        exit after n frames (default: run until closed)
//   NIER_REPLAY_KEEP_HLSL=1       use the HLSL stored in the capture instead of translating again
//   NIER_REPLAY_SKIP=<a,b-c,...>  leave out those commands (indices as listed in the log)
//   NIER_REPLAY_STOP=<n>          leave out every draw from command n on (resolves still run)
//   NIER_REPLAY_CULL=<mode>,<ccw>  force face culling on every draw that isn't a rectangle: mode 0
//                                 none, 1 front, 2 back; ccw 1 if counter-clockwise is the front
//                                 (version 1 captures hold no cull state)
//   NIER_REPLAY_ZOOM=<z>,<x>,<y>  magnify the scene z times around the point x,y (0-1 across the
//                                 screen): applied to depth-tested draws into full-width targets
// NIER_SAVE_FRAMES saves images as usual (frame numbers count replayed frames from 1).
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "guest.h"
#include "framecapture.h"
#include "xenos_shader.h"

namespace
{
    constexpr uint32_t kMagic = 0x4D52464E, kVersion = 2;   // "NFRM"; version 2 added the cull state

    struct Writer
    {
        FILE* file;
        template <typename T> void Put(const T& value) { fwrite(&value, sizeof(T), 1, file); }
        void Bytes(const void* data, size_t size) { if (size) fwrite(data, 1, size, file); }
        void Key(const RenderTargetKey& k) { Put(k.edramBase); Put(k.width); Put(k.height); }
    };

    struct Reader
    {
        FILE* file;
        bool ok = true;
        template <typename T> T Get() { T value{}; if (fread(&value, sizeof(T), 1, file) != 1) ok = false; return value; }
        void Bytes(void* data, size_t size) { if (size && fread(data, 1, size, file) != size) ok = false; }
        RenderTargetKey Key() { RenderTargetKey k; k.edramBase = Get<uint32_t>(); k.width = Get<uint32_t>(); k.height = Get<uint32_t>(); return k; }
    };

    struct StoredShader { std::shared_ptr<GuestShader> shader; std::vector<uint32_t> words; };

    // A list like "3,10-14" as a set.
    std::set<size_t> ParseList(const char* name)
    {
        std::set<size_t> list;
        char value[1024];
        if (!GetEnvironmentVariableA(name, value, sizeof(value))) return list;
        for (char* item = strtok(value, ","); item; item = strtok(nullptr, ","))
        {
            const size_t first = strtoull(item, nullptr, 10);
            const char* dash = strchr(item, '-');
            const size_t last = dash ? strtoull(dash + 1, nullptr, 10) : first;
            for (size_t i = first; i <= last && i < first + 100000; i++) list.insert(i);
        }
        return list;
    }

    const char* FormatName(GuestTextureFormat f)
    {
        static const char* names[] = { "RGBA8", "BC1", "BC2", "BC3", "R8", "RGBA16F", "RG16F", "R32F", "RG32F", "RGBA32F" };
        return names[int(f)];
    }
}

void FrameCaptureSave(const RendererFrame& frame, const std::string& path,
                      const std::function<const std::vector<uint32_t>*(bool pixel, uint64_t hash)>& microcode)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) { Log("[capture] couldn't write %s\n", path.c_str()); return; }
    Writer w{ file };

    // Shaders and textures are shared between draws: each is stored once.
    std::vector<const GuestShader*> shaders;
    std::map<const GuestShader*, int32_t> shaderIndex;
    std::vector<const DecodedTexture*> textures;
    std::map<const DecodedTexture*, int32_t> textureIndex;
    auto shaderOf = [&](const std::shared_ptr<GuestShader>& s) -> int32_t {
        if (!s) return -1;
        auto found = shaderIndex.find(s.get());
        if (found != shaderIndex.end()) return found->second;
        shaders.push_back(s.get());
        return shaderIndex[s.get()] = int32_t(shaders.size() - 1);
    };
    auto textureOf = [&](const std::shared_ptr<DecodedTexture>& t) -> int32_t {
        if (!t) return -1;
        auto found = textureIndex.find(t.get());
        if (found != textureIndex.end()) return found->second;
        textures.push_back(t.get());
        return textureIndex[t.get()] = int32_t(textures.size() - 1);
    };
    for (const RendererCommand& command : frame.commands)
        if (const RendererDraw* draw = std::get_if<RendererDraw>(&command))
        {
            shaderOf(draw->vs); shaderOf(draw->ps);
            for (const TextureBinding& binding : draw->textures) if (binding.valid) textureOf(binding.decoded);
        }

    w.Put(kMagic); w.Put(kVersion); w.Put(frame.frame); w.Put(frame.scanoutBase);
    w.Put(uint32_t(shaders.size()));
    for (const GuestShader* shader : shaders)
    {
        w.Put(shader->hash); w.Put(uint8_t(shader->pixel));
        const std::vector<uint32_t>* words = microcode ? microcode(shader->pixel, shader->hash) : nullptr;
        w.Put(uint32_t(words ? words->size() : 0));
        if (words) w.Bytes(words->data(), words->size() * 4);
        w.Put(uint32_t(shader->hlsl.size()));
        w.Bytes(shader->hlsl.data(), shader->hlsl.size());
    }
    w.Put(uint32_t(textures.size()));
    uint64_t textureBytes = 0;
    for (const DecodedTexture* t : textures)
    {
        w.Put(t->key); w.Put(t->contentHash); w.Put(t->width); w.Put(t->height); w.Put(t->rowPitch);
        w.Put(t->faces); w.Put(t->levels); w.Put(uint32_t(t->format));
        w.Put(uint64_t(t->data.size()));
        w.Bytes(t->data.data(), t->data.size());
        w.Put(uint32_t(t->subresources.size()));
        for (const DecodedTexture::Subresource& sub : t->subresources) { w.Put(uint64_t(sub.offset)); w.Put(sub.rowPitch); w.Put(sub.rows); }
        textureBytes += t->data.size();
    }
    w.Put(uint32_t(frame.commands.size()));
    for (const RendererCommand& command : frame.commands)
    {
        if (const RendererDraw* d = std::get_if<RendererDraw>(&command))
        {
            w.Put(uint8_t(0));
            w.Put(shaderIndex.count(d->vs.get()) ? shaderIndex[d->vs.get()] : -1);
            w.Put(shaderIndex.count(d->ps.get()) ? shaderIndex[d->ps.get()] : -1);
            w.Key(d->target); w.Put(uint8_t(d->hasColor)); w.Put(uint8_t(d->depthBound)); w.Key(d->depth);
            w.Put(d->depthControl); w.Put(d->stencilRefMask); w.Put(d->blendControl); w.Put(d->writeMask);
            w.Put(uint8_t(d->rectList)); w.Put(d->cull); w.Put(uint8_t(d->frontCounterClockwise)); w.Put(d->vertexCount);
            w.Put(uint32_t(d->indices.size())); w.Bytes(d->indices.data(), d->indices.size() * 4);
            w.Put(uint32_t(d->vertexData.size())); w.Bytes(d->vertexData.data(), d->vertexData.size());
            w.Bytes(d->vertexBase.data(), sizeof(d->vertexBase));
            w.Bytes(d->constants.data(), sizeof(d->constants));
            w.Bytes(d->boolConstants.data(), sizeof(d->boolConstants));
            for (const TextureBinding& b : d->textures)
            {
                w.Put(uint8_t(b.valid));
                if (!b.valid) continue;
                w.Put(b.decoded && textureIndex.count(b.decoded.get()) ? textureIndex[b.decoded.get()] : -1);
                w.Put(b.resolvedBase); w.Bytes(b.resolvedLevels, sizeof(b.resolvedLevels)); w.Put(b.resolvedLevelCount);
                w.Bytes(b.swizzle, 4); w.Put(uint8_t(b.point)); w.Put(b.addressU); w.Put(b.addressV);
                w.Put(uint8_t(b.cube)); w.Put(uint8_t(b.baseOnly)); w.Put(b.scale[0]); w.Put(b.scale[1]); w.Put(b.sign); w.Put(b.expScale);
            }
            w.Put(uint8_t(d->bypassViewport));
            w.Bytes(d->ndcScale, sizeof(d->ndcScale)); w.Bytes(d->ndcOffset, sizeof(d->ndcOffset));
            w.Bytes(d->viewport, sizeof(d->viewport)); w.Bytes(d->scissor, sizeof(d->scissor));
        }
        else if (const RendererResolve* r = std::get_if<RendererResolve>(&command))
        {
            w.Put(uint8_t(1));
            w.Key(r->source); w.Put(uint8_t(r->copyColor)); w.Put(uint8_t(r->copyDepth)); w.Put(r->scale);
            w.Put(r->destBase); w.Put(r->width); w.Put(r->height);
            w.Put(r->sourceX); w.Put(r->sourceY); w.Put(r->destX); w.Put(r->destY); w.Put(r->copyWidth); w.Put(r->copyHeight);
            w.Put(uint8_t(r->swapRedBlue)); w.Put(uint8_t(r->writeBack)); w.Put(r->destEndian); w.Put(r->destFormat);
            w.Put(uint8_t(r->clearColor)); w.Put(uint8_t(r->clearDepth)); w.Put(r->colorClear); w.Put(r->depthClear);
            w.Key(r->depth);
        }
    }
    const long long size = _ftelli64(file);
    fclose(file);
    Log("[capture] frame %llu saved to %s: %zu commands, %zu shaders, %zu textures (%.0f MB), %.0f MB in all\n",
        frame.frame, path.c_str(), frame.commands.size(), shaders.size(), textures.size(), textureBytes / 1048576.0, size / 1048576.0);
}

namespace
{
    bool LoadCapture(const char* path, RendererFrame& frame, std::vector<StoredShader>& shaders, std::vector<std::shared_ptr<DecodedTexture>>& textures)
    {
        FILE* file = fopen(path, "rb");
        if (!file) { Log("[replay] couldn't open %s\n", path); return false; }
        Reader r{ file };
        const uint32_t magic = r.Get<uint32_t>(), version = r.Get<uint32_t>();
        if (magic != kMagic || version < 1 || version > kVersion) { Log("[replay] %s isn't a frame capture this build can read\n", path); fclose(file); return false; }
        frame.frame = r.Get<uint64_t>();
        frame.scanoutBase = r.Get<uint32_t>();

        const uint32_t shaderCount = r.Get<uint32_t>();
        for (uint32_t i = 0; i < shaderCount && r.ok; i++)
        {
            StoredShader stored;
            stored.shader = std::make_shared<GuestShader>();
            stored.shader->hash = r.Get<uint64_t>();
            stored.shader->pixel = r.Get<uint8_t>() != 0;
            stored.words.resize(r.Get<uint32_t>());
            r.Bytes(stored.words.data(), stored.words.size() * 4);
            stored.shader->hlsl.resize(r.Get<uint32_t>());
            r.Bytes(stored.shader->hlsl.data(), stored.shader->hlsl.size());
            shaders.push_back(std::move(stored));
        }
        const uint32_t textureCount = r.Get<uint32_t>();
        for (uint32_t i = 0; i < textureCount && r.ok; i++)
        {
            auto t = std::make_shared<DecodedTexture>();
            t->key = r.Get<uint64_t>(); t->contentHash = r.Get<uint64_t>();
            t->width = r.Get<uint32_t>(); t->height = r.Get<uint32_t>(); t->rowPitch = r.Get<uint32_t>();
            t->faces = r.Get<uint32_t>(); t->levels = r.Get<uint32_t>(); t->format = GuestTextureFormat(r.Get<uint32_t>());
            t->data.resize(size_t(r.Get<uint64_t>()));
            r.Bytes(t->data.data(), t->data.size());
            t->subresources.resize(r.Get<uint32_t>());
            for (DecodedTexture::Subresource& sub : t->subresources) { sub.offset = size_t(r.Get<uint64_t>()); sub.rowPitch = r.Get<uint32_t>(); sub.rows = r.Get<uint32_t>(); }
            textures.push_back(std::move(t));
        }
        const uint32_t commandCount = r.Get<uint32_t>();
        frame.commands.reserve(commandCount);
        for (uint32_t i = 0; i < commandCount && r.ok; i++)
        {
            const uint8_t type = r.Get<uint8_t>();
            if (type == 0)
            {
                frame.commands.emplace_back(RendererDraw{});
                RendererDraw& d = std::get<RendererDraw>(frame.commands.back());
                const int32_t vs = r.Get<int32_t>(), ps = r.Get<int32_t>();
                if (vs >= 0 && size_t(vs) < shaders.size()) d.vs = shaders[vs].shader;
                if (ps >= 0 && size_t(ps) < shaders.size()) d.ps = shaders[ps].shader;
                d.target = r.Key(); d.hasColor = r.Get<uint8_t>() != 0; d.depthBound = r.Get<uint8_t>() != 0; d.depth = r.Key();
                d.depthControl = r.Get<uint32_t>(); d.stencilRefMask = r.Get<uint32_t>(); d.blendControl = r.Get<uint32_t>(); d.writeMask = r.Get<uint32_t>();
                d.rectList = r.Get<uint8_t>() != 0;
                if (version >= 2) { d.cull = r.Get<uint8_t>(); d.frontCounterClockwise = r.Get<uint8_t>() != 0; }
                d.vertexCount = r.Get<uint32_t>();
                d.indices.resize(r.Get<uint32_t>()); r.Bytes(d.indices.data(), d.indices.size() * 4);
                d.vertexData.resize(r.Get<uint32_t>()); r.Bytes(d.vertexData.data(), d.vertexData.size());
                r.Bytes(d.vertexBase.data(), sizeof(d.vertexBase));
                r.Bytes(d.constants.data(), sizeof(d.constants));
                r.Bytes(d.boolConstants.data(), sizeof(d.boolConstants));
                for (TextureBinding& b : d.textures)
                {
                    b.valid = r.Get<uint8_t>() != 0;
                    if (!b.valid) continue;
                    const int32_t texture = r.Get<int32_t>();
                    if (texture >= 0 && size_t(texture) < textures.size()) b.decoded = textures[texture];
                    b.resolvedBase = r.Get<uint32_t>(); r.Bytes(b.resolvedLevels, sizeof(b.resolvedLevels)); b.resolvedLevelCount = r.Get<uint32_t>();
                    r.Bytes(b.swizzle, 4); b.point = r.Get<uint8_t>() != 0; b.addressU = r.Get<uint8_t>(); b.addressV = r.Get<uint8_t>();
                    b.cube = r.Get<uint8_t>() != 0; b.baseOnly = r.Get<uint8_t>() != 0; b.scale[0] = r.Get<float>(); b.scale[1] = r.Get<float>();
                    b.sign = r.Get<uint32_t>(); b.expScale = r.Get<float>();
                }
                d.bypassViewport = r.Get<uint8_t>() != 0;
                r.Bytes(d.ndcScale, sizeof(d.ndcScale)); r.Bytes(d.ndcOffset, sizeof(d.ndcOffset));
                r.Bytes(d.viewport, sizeof(d.viewport)); r.Bytes(d.scissor, sizeof(d.scissor));
            }
            else if (type == 1)
            {
                RendererResolve v;
                v.source = r.Key(); v.copyColor = r.Get<uint8_t>() != 0; v.copyDepth = r.Get<uint8_t>() != 0; v.scale = r.Get<uint32_t>();
                v.destBase = r.Get<uint32_t>(); v.width = r.Get<uint32_t>(); v.height = r.Get<uint32_t>();
                v.sourceX = r.Get<int>(); v.sourceY = r.Get<int>(); v.destX = r.Get<int>(); v.destY = r.Get<int>();
                v.copyWidth = r.Get<uint32_t>(); v.copyHeight = r.Get<uint32_t>();
                v.swapRedBlue = r.Get<uint8_t>() != 0; v.writeBack = r.Get<uint8_t>() != 0; v.destEndian = r.Get<uint32_t>(); v.destFormat = r.Get<uint32_t>();
                v.clearColor = r.Get<uint8_t>() != 0; v.clearDepth = r.Get<uint8_t>() != 0; v.colorClear = r.Get<uint32_t>(); v.depthClear = r.Get<uint32_t>();
                v.depth = r.Key();
                v.writeBack = false;   // there is no game to read the pixels back
                frame.commands.emplace_back(std::move(v));
            }
            else r.ok = false;
        }
        fclose(file);
        if (!r.ok) Log("[replay] %s is cut short or damaged\n", path);
        return r.ok;
    }

    // One line per command, so a draw can be picked out by index.
    void ListCommands(const RendererFrame& frame)
    {
        for (size_t i = 0; i < frame.commands.size(); i++)
        {
            if (const RendererDraw* d = std::get_if<RendererDraw>(&frame.commands[i]))
            {
                std::string textures;
                for (size_t slot = 0; slot < d->textures.size(); slot++)
                {
                    const TextureBinding& b = d->textures[slot];
                    if (!b.valid) continue;
                    char item[96];
                    if (b.decoded) snprintf(item, sizeof(item), " [%zu %s %ux%u L%u%s]", slot, FormatName(b.decoded->format), b.decoded->width, b.decoded->height, b.decoded->levels, b.cube ? " cube" : "");
                    else snprintf(item, sizeof(item), " [%zu resolved %08X]", slot, b.resolvedBase);
                    textures += item;
                }
                Log("[replay] %4zu draw vs %016llX ps %016llX verts %u idx %zu target %X/%u%s depth %08X blend %08X vp %.0f,%.0f %.0fx%.0f%s%s%s\n", i,
                    d->vs ? d->vs->hash : 0ull, d->ps ? d->ps->hash : 0ull, d->vertexCount, d->indices.size(), d->target.edramBase, d->target.width,
                    d->hasColor ? "" : " (depth only)", d->depthControl, d->blendControl, d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3],
                    d->rectList ? " rect" : d->cull == 1 ? " cull-front" : d->cull == 2 ? " cull-back" : "", d->bypassViewport ? " screen" : "", textures.c_str());
            }
            else if (const RendererResolve* r = std::get_if<RendererResolve>(&frame.commands[i]))
            {
                Log("[replay] %4zu resolve %X/%u -> %08X %ux%u%s%s%s\n", i, r->source.edramBase, r->source.width, r->destBase, r->width, r->height,
                    r->copyDepth ? " depth" : r->copyColor ? " colour" : " (no copy)", r->clearColor ? " clear colour" : "", r->clearDepth ? " clear depth" : "");
            }
        }
    }
}

void FrameReplayRunIfRequested()
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("NIER_REPLAY_FRAME", path, sizeof(path))) return;

    RendererFrame frame;
    std::vector<StoredShader> shaders;
    std::vector<std::shared_ptr<DecodedTexture>> textures;
    if (!LoadCapture(path, frame, shaders, textures)) { fflush(stdout); ExitProcess(2); }
    Log("[replay] %s: frame %llu, %zu commands, %zu shaders, %zu textures\n", path, frame.frame, frame.commands.size(), shaders.size(), textures.size());

    // Translate the shaders again, so the current translator and the debugging switches apply.
    if (!GetEnvironmentVariableA("NIER_REPLAY_KEEP_HLSL", nullptr, 0))
    {
        CreateDirectoryA("..\\shaders", nullptr);
        size_t translated = 0;
        for (StoredShader& stored : shaders)
        {
            if (stored.words.empty()) continue;
            TranslatedShader info = TranslateShader(stored.words, stored.shader->pixel, stored.shader->hash);
            if (!info.ok) { Log("[replay] %s shader %016llX not translated: %s\n", stored.shader->pixel ? "pixel" : "vertex", stored.shader->hash, info.error.c_str()); continue; }
            stored.shader->hlsl = info.hlsl;
            translated++;
            char file[96];
            snprintf(file, sizeof(file), "..\\shaders\\%s_%016llX.hlsl", stored.shader->pixel ? "ps" : "vs", stored.shader->hash);
            if (FILE* out = fopen(file, "wb")) { fwrite(info.hlsl.data(), 1, info.hlsl.size(), out); fclose(out); }
        }
        Log("[replay] %zu shaders translated again from their microcode\n", translated);
    }
    ListCommands(frame);

    const std::set<size_t> skip = ParseList("NIER_REPLAY_SKIP");
    char value[64];
    const size_t stop = GetEnvironmentVariableA("NIER_REPLAY_STOP", value, sizeof(value)) ? strtoull(value, nullptr, 10) : ~size_t(0);
    float zoom = 1.0f, zoomX = 0.5f, zoomY = 0.5f;
    if (GetEnvironmentVariableA("NIER_REPLAY_ZOOM", value, sizeof(value))) sscanf(value, "%f,%f,%f", &zoom, &zoomX, &zoomY);
    int forcedCull = -1, forcedCcw = 1;
    if (GetEnvironmentVariableA("NIER_REPLAY_CULL", value, sizeof(value))) sscanf(value, "%d,%d", &forcedCull, &forcedCcw);
    uint32_t fullWidth = 0;   // the widest target drawn to: the scene's
    for (const RendererCommand& command : frame.commands)
        if (const RendererDraw* d = std::get_if<RendererDraw>(&command)) fullWidth = (std::max)(fullWidth, uint32_t(d->viewport[2]));

    RendererFrame prepared;
    prepared.scanoutBase = frame.scanoutBase;
    size_t zoomed = 0;
    for (size_t i = 0; i < frame.commands.size(); i++)
    {
        const bool isDraw = std::holds_alternative<RendererDraw>(frame.commands[i]);
        if (skip.count(i) || (isDraw && i >= stop)) continue;
        prepared.commands.push_back(frame.commands[i]);
        if (!isDraw) continue;
        RendererDraw& d = std::get<RendererDraw>(prepared.commands.back());
        if (forcedCull >= 0 && !d.rectList) { d.cull = uint8_t(forcedCull); d.frontCounterClockwise = forcedCcw != 0; }
        if (zoom == 1.0f) continue;
        // The scene's own geometry: depth-tested draws through a full-width viewport. Full-screen
        // passes and the HUD (no depth test, or one that always passes) and shadow or reduced-size
        // passes keep their viewport, or the picture would be magnified twice.
        const bool depthTested = (d.depthControl & 2) && ((d.depthControl >> 4) & 7) != 7;   // enabled, and not "always"
        if (d.bypassViewport || d.rectList || !depthTested || d.viewport[2] < fullWidth * 0.9f) continue;
        const float px = d.viewport[0] + d.viewport[2] * zoomX, py = d.viewport[1] + d.viewport[3] * zoomY;
        const float cx = d.viewport[0] + d.viewport[2] * 0.5f, cy = d.viewport[1] + d.viewport[3] * 0.5f;
        d.viewport[0] = cx + (d.viewport[0] - px) * zoom;
        d.viewport[1] = cy + (d.viewport[1] - py) * zoom;
        d.viewport[2] *= zoom;
        d.viewport[3] *= zoom;
        zoomed++;
    }
    if (zoom != 1.0f) Log("[replay] zoom %.2f around %.2f,%.2f applied to %zu draws\n", zoom, zoomX, zoomY, zoomed);
    Log("[replay] rendering %zu of %zu commands\n", prepared.commands.size(), frame.commands.size());
    while (!RendererReady()) Sleep(10);
    for (const StoredShader& stored : shaders) RendererPrepareShader(stored.shader);

    const uint64_t frames = GetEnvironmentVariableA("NIER_REPLAY_FRAMES", value, sizeof(value)) ? strtoull(value, nullptr, 10) : ~0ull;
    for (uint64_t number = 1; number <= frames; number++)
    {
        RendererFrame next;
        next.frame = number;
        next.scanoutBase = prepared.scanoutBase;
        next.commands = prepared.commands;
        RendererSubmitFrame(std::move(next));   // dropped until the renderer is ready
        Sleep(16);
    }
    Sleep(500);   // let the last frame reach the screen and any requested image be written
    RendererDumpStats();
    fflush(stdout);
    ExitProcess(0);
}
