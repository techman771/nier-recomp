# NieR (Xbox 360) static recompilation notes

`Nier/` is the untouched extracted game. Treat it as read-only, and never commit or
redistribute it or anything generated from it (recompiled sources contain the game's code).

## default.xex

| Field | Value |
| --- | --- |
| Title ID | `0x535107E8` |
| Media ID | `0x507D2720` |
| Original PE name | `nier.exe` |
| Load address | `0x82000000` |
| Entry point | `0x8255CEA0` |
| Image size | `0x4090000` |
| Stack size | `0x40000` |
| TLS slots | 64 |
| Encryption | AES (normal) |
| Compression | basic |
| XDK | 2.0.9328 |
| Imports | `xam.xex` (88 records), `xboxkrnl.exe` (284 records) |

Static libraries: XAUDIO2, XAPOBA, XMCORE, XAPILIB, D3D9, XGRAPHC, XBOXKRNL, LIBCPMT,
XONLINE, XMP, D3DX9.

No title update (`.xexp`) is present. `$SystemUpdate/` is a console dashboard update and is
not needed for the port.

## Data

- `media/layer1.cpk` (3.0 GB), `media/layer2.cpk` (1.4 GB): CRI archives
- `media/movie/*.sfd`: 4 CRI Sofdec movies (1.6 GB)

## Toolchain

Everything comes from Visual Studio 2026 Community. Dot-source `tools/devshell.ps1` to get it on PATH.

`tools/xexinfo.py <xex>` dumps the XEX header (read-only).

## Recompiler setup

XenonRecomp is at `tools/XenonRecomp` (commit `ddd128b`), built into `tools/build`.

To regenerate everything, from a shell with `devshell.ps1` loaded:

    tools/make_config.ps1
    tools/build/XenonRecomp/XenonRecomp.exe recomp/config/nier.toml tools/XenonRecomp/XenonUtils/ppc_context.h

`recomp/config/nier.base.toml` is hand-written. `nier.toml`, `nier_switch_tables.toml` and
`nier_functions.snippet.toml` are generated; don't edit them.

### Do not use XenonAnalyse on this game

It matches `lis, addi, rlwinm, lwzx`, but this compiler emits `lis, rlwinm, addi, lwzx`, so it
finds 1 bogus table. `tools/xenonscan/jumptables.cpp` replaces it. Other things it handles:

- Jump tables are inline in `.text`, directly after the `bctr`.
- 43 leaf functions containing tables have no `.pdata` entry, so they get explicit bounds.
- The index register is sometimes a copy of the compared register.
- The default branch sometimes uses `ble +8; b default`.

### Confirmed addresses

| Function | Address |
| --- | --- |
| `longjmp` | `0x825D5AC0` |
| `setjmp` | `0x825D5DE0` |
| `RtlUnwind` thunk | `0x82625C14` |

The eight save/restore helpers are in `nier.base.toml`.

## Status

The recompile runs clean: 22,354 functions, 248 of 248 jump tables, 0 label errors, 0 missing
tables, 0 unrecognized instructions. All generated C++ compiles (`tools/compile_ppc.ps1`, `/Od`,
warnings suppressed). Nothing has been linked or run, because the Windows runtime doesn't exist yet.

### Local XenonRecomp changes

`tools/xenonrecomp-nier.patch` holds them, against upstream commit `ddd128b`. It adds 29 opcodes
and `simde_mm_cmpgt_epu32`. The recompiler emits only a comment for opcodes it doesn't know, so
**always check `recomp/recomp.log` for `Unrecognized instruction` after a recompile.**

`tools/xenonscan/opcode_tests` runs the emitted C++ for each added opcode against expected values
from the PowerPC architecture. `gen.py` extracts real instances from `recomp/out`, so rerun it
after a recompile. The harness caught all three bugs deliberately injected to test it.

These tests cover the added opcodes only. The opcodes XenonRecomp already implemented have not
been re-verified for this game. The existing `bdnzf` assumes the `eq` bit, which holds for every
use in NieR but would need fixing for another title.

### Exception handler data (fixed)

Functions with an exception handler have 8 bytes of data just before them (handler address, then
handler data). XenonRecomp decoded those as code, creating ghost functions that ran into the real
one, and its scan then resumed mid-function and created more. Their branches had no valid target,
which produced 51 `// ERROR` markers, where the recompiler emits no branch at all.

`jumptables.exe` now reads the handlers from the `.pdata` exception flag and writes them to
`invalid_instructions`. The flag is also set on three register-restore helpers (`0x825D17F0`,
`0x825D224C`, `0x825D2538`) where the preceding words are code, so only words that are valid code
addresses count as handlers.

**After every recompile, run the audit.** It fails on any `// ERROR` marker or ghost function:

    python -I tools/xenonscan/check_functions.py recomp/out tools/build/xenonscan/dis.exe Nier/default.xex tools/build/xenonscan

Current result: 22,021 functions, 0 markers, 0 ghosts. 28 symbols start inside a `.pdata` function;
all are genuine `bl` targets, called once from their parent.

`recomp/ppc` is stale output from before this fix and can be deleted. Output now goes to `recomp/out`.

### Hard traps (fixed)

XenonRecomp also emits `__builtin_debugtrap()` for some variants it doesn't implement. NieR had 5:
`vcmpbfp128`, `blrl`, `vpkd3d128` type 3 (float16_2) and `vupkd3d128` type 4 (4 shorts).
`tools/recompile.ps1` reports the trap count, which must stay 0. The two pack variants were
derived from neighbouring cases, call sites and memory of the XDK's pack types, not from a spec.

## Runtime

`recomp/runtime` plus the generated code link to `recomp/build/nier.exe`.

    . tools/devshell.ps1
    tools/recompile.ps1                       # only after changing XenonRecomp or the config
    tools/compile_ppc.ps1                     # builds nier.exe (add -Opt /O2 for speed)
    tools/run_boot.ps1 -Seconds 40 -DumpAfter 25

Output goes to `recomp/boot.log`. `-DumpAfter N` prints every guest thread's call stack after N
seconds and exits. `-Trace` logs every import call. Crashes print the guest function, registers,
recent imports and call stack.

Imports are defined with `GUEST_IMPORT(Name)`. `tools/gen_stubs.py` generates a logging stub
returning 0 for the rest (`stubs.gen.cpp`). Currently 117 implemented, 63 stubbed.

Layout: 4GB guest space committed flat; heap `0x20000000`, virtual `0x40000000`, physical
`0xA6100000`; the indirect call table sits at `0x86090000`, right after the image. Physical
addresses in `0x02000000-0x06100000` belong to the image, which is why physical memory starts
above them (`PhysicalToGuest`). Files are
read-only and resolved under `Nier/`; writes are refused. Each guest thread is a host thread.

### Status

Runs for over 4 minutes with no crash at 60 frames per second (15,523 frames, unoptimized build) and
shows the title movie in a window. `TITLE_NIER_ALL.SFD` plays to the end (about 3 minutes), then
about 33 seconds pass with no movie source, then the game reopens the movie. That gap is presumably
the title screen, but it can't be confirmed because only movies have a picture so far.

`run_boot.ps1 -SaveFrames 2400,3600` writes `recomp/frames/f<N>.ppm`, and `tools/ppm2png.py` converts
them for inspection. Always look at the output: frame counts stayed healthy while the image was black.

### Window and movies (`present.cpp`, `gpu.cpp`)

`renderer_d3d12.cpp` presents with vsync on its own thread, taking the newest submitted frame. Movies
are detected as a quad list over three 8-bit linear textures (Y in slot 0, half-size U and V in slots 1
and 2) and converted with BT.601 limited-range constants in a pixel shader. That detection is a shortcut
that real shader translation will replace. Everything else presents black. `SET_CONSTANT` and related
packets now fill the register file, so fetch constants are available to the renderer. Saved frames are
read back from the swap chain, so they show exactly what the GPU produced. `NIER_D3D_DEBUG=1` enables
the D3D12 debug layer.

### Audio timing (`audio.cpp`)

Movies stall unless the render callback runs. The callback's parameter is the **address** of the
argument, because it starts with `lwz r3,0(r3)`. Output is WASAPI stereo float 48 kHz, and the device's
consumption decides when the next 256-sample block is requested (about 43ms queued), falling back to
a timer with no device. Frames arrive as six planes of 256 big-endian floats (FL, FR, FC, LFE, BL, BR),
downmixed to stereo. The thread dump prints RMS, peaks and a smoothness figure for checking the signal.

### Input (`input.cpp`)

XInput pads map directly. The keyboard acts as player one's pad while the window has focus:
WASD left stick, IJKL right stick, arrows d-pad, Space A, Left Shift B, E X, R Y, Q LB, F RB, Z LT,
C RT, V L3, B R3, Enter Start, Tab Back.

`run_boot.ps1 -AutoPress 2000:10,3200:1000` presses hex button masks at frame numbers, for 12 frames.
Pressing Start at frame 2000 ended the title movie, and draws per frame rose. Confirmed by hand on
real hardware: movie sound is audible and live input skips the movie.

### Menus (stage 2)

The title screen, storage prompts and main menu render through the real pipeline and match reference
footage of the original. Flow: `gpu.cpp` captures each draw (vertices, constants, decoded textures) and
each resolve into a `RendererFrame`; `xenos_shader.cpp` translates microcode to HLSL; `renderer_d3d12.cpp`
executes them. `tools/xenos_dis.py` disassembles the `.bin` files in `recomp/shaders`, where the
generated `.hlsl` is also written.

Xenos facts confirmed here: ALU source swizzles are offsets from identity, but fetch source swizzles
are absolute. A tiled surface with a dimension of 16 or less and the packed-mips flag (fetch constant
dword 5, bit 11) starts at a 16-texel offset inside its tile; missing this made the copyright text
vanish. Resolves with the swap bit change which channel each guest component reads, but scanout does not.

Known shortcuts, all to revisit for 3D: depth is ignored and depth-only passes are skipped; indexed
draws and point lists are skipped; render target heights are assumed 16:9; only color resolves and
one sampler address mode (clamp) exist; only base mip levels are decoded; the translator rejects
loops, jumps, predicates and relative addressing. It logs a reason whenever it declines a shader.
Rect lists use a geometry shader, which MoltenVK won't have. Decoded textures are never evicted.

### Gameplay rendering (stage 3, in progress)

New Game now reaches the prologue (`C_TOKYO_FIELD_01`) and renders 3D: indexed draws with primitive
restart, depth/stencil targets, depth-only passes, predicates and kills in the translator, packed vertex
formats, the address register, pixel depth export, and cube maps (the `CUBE` instruction, cube
fetches, six-face textures as `TextureCube`; used by the character materials). The opening
credits, the snow-sky shot and the Grimoire scene ("Take it and save your daughter...") play through.
Only 24 draws a run are skipped (primitive type 1). The scene looks like the game: textured ruined
buildings, fences, snow, fog and light shafts. Characters render with plausible
materials. The cinematic letterbox bars are translucent in the real game too (checked against a
reference video of the Xbox version, https://www.youtube.com/watch?v=7UjhobXbIU0, 0:00-5:00).

16-bit vertex formats (`k_16_16`, `k_16_16_16_16` and their float versions) take component x from
the low half of each 32-bit word and y from the high half (Xenia: offsets 0, 16). The translator had
them the other way round, which swapped every 16-bit attribute pair: texture coordinates came out as
(v, u), so textures were smeared into stripes, and much of the store's geometry was garbled.

Packed 10/11-bit vertex formats: names list fields from the most significant bit (Xenia):
`k_10_11_11` (16) is x 11 bits at bit 0, y 11 at 11, z 10 at 22; `k_11_11_10` (17) is x 10 at 0,
y 11 at 10, z 11 at 21. The translator had the two swapped, which scrambled every character's
normals (format 16): faces under Nier's hood rendered black, cloth looked glossy, Yonah was lit
wrongly. Fixed; the user confirmed Nier and Yonah now match the reference video.

Textures are made readable for every shader stage (`kShaderRead` in `renderer_d3d12.cpp`), not just
pixel shaders: the bone texture is read by vertex shaders, and with the pixel-shader state alone a
vertex shader may read it before the upload into it has finished.

Fixed: `memset` (`sub_825D3D90`) was cut short by XenonRecomp. Its byte tail is
`stb; bdzlr; stb r4,1(r6); bdzlr; stb r4,2(r6); blr`, and the recompiler ended the function at the
first `bdzlr`, splitting the rest into unreachable fragments, so sets leaving two or three tail bytes
stopped one or two bytes short. Nier's skeleton update (`sub_8218BF70`) fills a 95-byte "needs
parent" mask with `memset(mask, 1, 95)`; the last two entries stayed unset, so bones 92 and 93 skipped
the hierarchy pass (`sub_821898F0` / `sub_82189840`: world = parent x local, parents resolved
recursively) and stayed in local space. Vertices weighted to them landed near the world origin: a
black beam from Nier's chest across the screen in the Grimoire scene, and a split pipe. The true
bounds are now in `recomp/config/nier_functions.manual.toml` (merged by `make_config.ps1`), and
`tools/find_fallthrough.py recomp/out` lists recompiled functions whose last instruction can fall
through (now only calls to functions that never return, and one data word).
How it was found: dumped draw data (`NIER_DUMP_DRAW`) skinned on the CPU reproduced the beam, so
the bone matrices were wrong; write watches (`NIER_WATCH_WRITE`) and a memory scan (`NIER_SCAN`)
followed the matrices back through the copies to the hierarchy pass, and a hardware breakpoint
(`NIER_BREAK`) on `sub_821898F0` showed the mask's last two bytes unset.
Texture address modes: every texture used to be sampled with clamp addressing, so tiling textures
(floors, shelves, walls, building facades, with coordinates far outside 0-1) smeared their edge
texels into stripes; only textures used within 0-1 (product labels) looked right. The fetch
constant's clamp modes (dword 0 bits 10-12 for x, 13-15 for y) now pick the sampler, mapped as Xenia
does: repeat -> wrap, mirrored repeat -> mirror, clamp to edge / halfway -> clamp, the mirror-clamps ->
mirror once, clamp to border -> border. The renderer has 50 samplers (point/linear x 5 x 5 address
modes) in a sampler descriptor table (root parameter 4; static samplers can't back an indexed
array); the shader samples `samplers[samplerIndex[slot]]`. Border colour is transparent black for
now (the fetch constant's border colour is not read yet). Found with `NIER_DUMP_TEX=<base>[,...]`,
which writes a decoded texture to `recomp/captures/` (`tools/tex2png.py` converts it), plus
per-triangle texture-coordinate checks of dumped draws.

Character materials (Nier's shader `C67CA1BB...`): t7 colour atlas (BC1), t8 normal map (CTX1), t9 BC3
material parameters (all four channels feed the lighting), t1 a 64x64 `k_16_16_16_16_EXPAND` cube
that is the main (ambient) lighting, not a reflection: zeroing it leaves only the head visible. The
cube's data is float16 (values cluster at 0x3C00 = 1.0, range 0.04-15.6), as Xenia uploads EXPAND
formats; its sign modes (two's complement on every channel) are ignored for float formats, as in
Xenia. With the texture coordinate and address mode fixes, close-ups match the reference video.
`NIER_SHADER_PATCH=<hash>|<find>|<replace>[;;...]` edits a translated shader's HLSL before compiling
(e.g. zero a term to see what it contributes); the log lists each texture format's component swizzle.

An intermittent crash was seen once, right after New Game (the log's last line was the open of
`STAGE_SELECT.INI`): the main thread in the game's initialisation (`sub_825A5FE8` <- `sub_825A5858` <-
`sub_825A7B60`, linked-list updates) wrote guest address 0x10, a null pointer plus 16. It did not recur
in 12 boots and 5 New Game runs, so it is probably a rare thread-timing race in the kernel emulation.
Not fixed; the crash handler logs the host and guest stacks if it happens again.

Mipmaps follow Xenia's `GetGuestTextureLayout`: levels 1 and up start at the fetch constant's mip
address (dword 5); each level's pitch and height come from the power-of-two base size shifted down
and padded to 32x32-block tiles, and each level (all six faces for a cube) is padded to 4 KB. From
the packed level (shorter side 16 texels or less) on, the remaining levels share that level's tile at
`GetPackedMipOffset` positions; when the base itself is packed, all mips share one tile at the mip
address. The max level is dword 4 bits 6-9; mip filter "base map" (dword 3 bits 23-24 = 2) binds
the base only. The set-LOD fetch (opcode 24) stores a level that fetches with "use register LOD"
(dword 1 bit 29) sample with `SampleLevel`; the character shaders use it to blur reflections on
rough surfaces, and without it they looked like chrome.

Scalar ALU ops read source 3 with Xenia's component rule: one-operand ops use swizzle component 3,
two-operand ops use component 3 then component 0. The translator used components 0 and 1, which only
works for replicated swizzles; it broke the character lighting (dark bodies with chrome-like edges)
and shifted the city's colours. The game runs slower than real time in the character scenes (about 40 fps).

Characters are skinned in the vertex shader from a bone texture (fetch constant 16, format 38,
1024x1, 1D). Vertex shaders sample with `SampleLevel` at level 0; 1D textures are bound as one-row
2D textures. Texture bindings cover all 32 fetch constants, in register space 1 (space 0 `t16` is
the vertex buffer).

Texture formats decoded: 2 (R8), 6 (RGBA8), 18-20 (BC1-3), 29 (`k_16_16_16_16_EXPAND`, uploaded as
half floats like Xenia), 31/32 (16-bit float), 36-38 (32-bit float), 60 (`k_CTX1`, decoded on the
CPU to (x, y, y, y) following Xenia's `texture_load_ctx1`), and 22 when it is a depth resolve.
Texture sign modes (fetch constant dword 0 bits 2-9) are applied after sampling: two's complement
for 8-bit data, biased (2x-1), and gamma (Xenia's piecewise-linear curve). The game's colour maps
(formats 18 and 20) use gamma. Host textures are keyed by address and shape and re-uploaded when the
contents change; keying by contents created a new texture every frame for the bone matrices.

Scalar "constant" ops (opcodes 42-47: MULsc/ADDsc/SUBsc with a constant and a register) take the
constant from source 3's register with swizzle component 3, and a temporary whose index is
`opcode bit 0 | src3_sel << 1 | swizzle bits 2-5`, with swizzle component 0 (Xenia
`scalar_const_reg_op_src_temp_reg`). Getting the bit order and components wrong made every scene
vertex shader compute the fog factor from the wrong register, so all surfaces came out as flat fog
colour.

Physical memory windows: on the console, 0xA0000000 and 0xC0000000 show the same physical memory.
D3D writes vertex shader microcode through one and copies it into command buffers through the other,
so with separate memory every game vertex shader arrived as zeros except the fetch instructions D3D
patches in (they "translated" into shaders that drew nothing; roughly 43,000 draws a run were lost).
`memory.cpp` now maps one 512 MB section at both addresses inside the 4 GB reservation (placeholders:
`VirtualAlloc2` / `MapViewOfFile3`, Windows 10 1803+). The 0xE0000000 window is offset by 4 KB on the
console, which 64 KB mapping granularity can't express, so it is still separate. Linux and macOS
will need the same arrangement with `memfd_create` / `shm_open` and `mmap`.

Depth resolves (copy source 4) draw the depth buffer into an R32_FLOAT texture, which shaders sample as
format 22 (`k_24_8`) with the depth in every channel, as Xenia does. Depth buffers are created
`R24G8_TYPELESS` so they can be read. The shadow map pass and the scene depth (`07A7D000`,
`072E2000`, `14D55000` in the prologue) are real now.

Auto-exposure and resolve write-back: the scene is rendered in HDR (RGBM) and the game measures its
brightness by resolving each downsampling step into the next mip level of one texture (`07184000`
512x512, levels at `07284000`, `072C4000` ... `072DC000`, then `0767A000`), sampling the levels with
set-LOD. The CPU then reads the result to set the exposure constant (`c0`) of the next frames.
Gameplay (not the cutscenes, which use fixed exposure) turned white because resolves only existed as
host textures. Now:
- a texture whose base is a resolved surface gets its mip levels from the resolved surfaces at the
  level addresses, combined into one mipmapped host texture and recopied when a level is rewritten;
- resolves of 64x64 texels or less are read back after the frame's GPU work and written into guest
  memory as the copy hardware would (tiled, red/blue swap, endian swap from `RB_COPY_DEST_INFO`).
  `NIER_RESOLVE_WRITEBACK=all` writes back every resolve (slow), `none` disables it.

Gameplay renders at 768x432 into 800-pixel-wide EDRAM surfaces (cutscenes use 1280x720).

Performance: the GPU command thread (PM4 parsing, texture decoding, vertex copies) bounded gameplay
speed. Measured with the sampling profiler (`NIER_PROFILE`), 20 s of the first fight:
- runtime built `/Od`: 7 fps (under the profiler); `compile_ppc.ps1` now builds the runtime `/O2`
  too (`-RuntimeOpt /Od` for a debuggable runtime; symbols are kept either way): 17 fps;
- textures over 64 KB are hashed once per frame instead of on every use (small ones, such as bone
  matrices, are still hashed on every use, every byte), and when a vertex shader addresses vertices
  only with the unmodified vertex index, only the vertices between a draw's smallest and largest
  index are copied (fetch constants often cover a whole level's geometry): 30 fps.
The command thread is now about 60% busy; the game's main thread is always busy.
`NIER_HASH_EVERY_USE=1` and `NIER_NO_VERTEX_TRIM=1` turn the two shortcuts off for comparison.
60 fps: the original runs 3D scenes at 30 fps (menus at 60). D3D queues each flip from the swap
interrupt (`sub_8257A4B0`, reached only through the indirect call table); its swap word carries the
present interval in bits 8-11 (2 in 3D scenes, 3 somewhere at boot), the flip waits that many vblanks
after the previous one, and the GPU waits on a flip gate (`WAIT_REG_MEM` on the word at physical
`0630E004`) until it has happened. `NIER_60FPS=1` (set by `run_boot.ps1` unless `-Fps30`) replaces the
table entry with a wrapper that lowers the interval to 1, like the Series X FPS Boost. Game logic is
time-based: at 60 fps every scene arrives at a later frame number and real time to reach the first
fight is unchanged (~10,300 vblanks at 30 fps, ~11,250 at 60), so nothing runs at double speed.
Speed work for 60 fps (first fight, 15 s): vertex range trimming now also applies when a shader writes
r0 after its last vertex fetch or only r0's other components (the skinning shaders); vertex data is
appended and swapped in place instead of resized (zero-filled) first; the frame's command list
reserves capacity; index reads reuse a buffer; texture contents are hashed with XXH3 (every byte);
the command thread polls for 4 ms before sleeping and the vblank thread spins its last 2 ms. Guest
shaders compile on worker threads as soon as they are translated, with the DXBC cached on disk
(`recomp/shadercache`, keyed by the HLSL's hash, ignored by git): compiling at first use on the render
thread caused 100-500 ms hitches. Result: 59.8 fps in the fight with a warm cache (58.7 cold), the
command thread ~40% busy and waiting on the flip gate the rest. The stats dump (profiler exit) now
lists frame times in vblanks, on-time and late frames split into waiting / idle / working time, and
each `WAIT_REG_MEM` location's count and total wait.
Where the shaders live (for ahead-of-time compilation): of 97 captured shaders long enough to search
(`tools/find_shaders.py`), 45 are stored byte-for-byte in the CPK archives, uncompressed, packed back
to back (each model/package carries its own copies, so most appear dozens of times), and 36 in the
executable image (`NIER_DUMP_IMAGE=1` writes it to `recomp/captures/image.bin`): D3D's utility shaders,
post-processing and UI. Pixel shaders match exactly; vertex shaders differ only in their vertex fetch
instructions, which D3D patches at runtime (`tools/shader_context.py` compares a capture with its stored
copy). The archives also hold 1,175 CRILAYLA-compressed blocks (530 MB unpacked, `tools/crilayla.cpp`);
no further shaders are in them. 16 shaders, mostly the main scene and character material shaders
(`8263A851`, Nier's head `D461B875` and hair `8330682F`), are not stored as-is anywhere searched: their
closest stored relatives differ like separate compiles (instructions shifted, registers renumbered),
and the executable contains the Xbox 360 HLSL compiler's intrinsic tables and messages, though no HLSL
source was found in the archives. So these are probably generated at runtime; how is not known yet.

Load-time shader compilation (investigated, not done): the game copies blocks of shaders into
physical memory while loading (`memcpy` from `sub_8255C260`, called from the game's loader around
`sub_8211D2D0`; e.g. 0x1B00 bytes to `AB933400` at frame 1669, the store scene). In the block each
shader's microcode follows a 64-byte table of float literals, with padding after; sizes are not
stored next to them. No D3D shader object holds the microcode address: the encoded address (physical
| stage, e.g. `0B933421`) appears only in `IM_LOAD` packets in the live ring buffer, a frame before
use, so command buffers aren't recorded ahead of time and can't be scanned for upcoming shaders
(`NIER_SCAN_IMLOAD=1` at a profiler exit lists the `IM_LOAD` packets in physical memory).
Discovering shaders at load time therefore needs the game's package format (the loader knows the
sizes). Compile cost per shader, measured on 111 captured shaders: FXC as used now 54 ms average /
210 ms worst; with registers as separate variables instead of the `r[]` array and
`D3DCOMPILE_SKIP_OPTIMIZATION` 21 / 75 ms; DXC (`-O0`, SM 6.0) 14.5 / 64 ms (the SDK's DXC lacks SPIR-V
output). Debug switches added: `NIER_SCAN_U32=<hex>,...` (find 32-bit values in guest memory),
`NIER_DUMP_RANGE=<address>,<bytes>` (both at a profiler exit), and the log names each shader's
microcode address the first time it is loaded.

Cheaper first-use compiles (both measured over the 60 fps route, fight profiled at frame 11000):
registers are emitted as separate variables (`r0`, `r1`, ...) instead of an `r[]` array (written as
`r[N]` during translation and rewritten at the end, so `NIER_PS_DEBUG` / `NIER_SHADER_PATCH`
expressions still use `r[N]`), and FXC skips optimization (the flags are part of the cache key;
`NIER_SHADER_OPTIMIZE=1` restores level 1 for comparison). Pipelines are now also created on worker
threads (their own pool, since they wait for shader compiles), started by the GPU emulation when it
first sees a shader/state combination (`RendererPreparePipeline`). On a first run (nothing cached,
forced with `NIER_SHADER_SALT=1`, which makes bytecode neither cache has seen) the driver spends about
4.2 s building ~114 pipelines (worst ~220 ms) over the route; the render thread now waits 1.5 s of that
in total (optimized FXC: 2.1 s), and the fight runs at 58.6 fps. Warm: no waits, 59.9 fps. The stats
dump reports shader-compile and pipeline waits on the render thread and total pipeline creation time.

Fast route to the fight: `recomp/newgame_args.txt` (Start every 60 frames skips the logos and menus;
A at the "Overwrite previously-saved system data?" prompt keeps the default No; Left, A answers Yes
to "Set your name as Nier?"). The opening credits can't be skipped; the fight starts around frame
5,600 at 30 fps and 11,000 at 60 fps.

Runs on the scripted path don't reach the same scene at the same frame number when speed changes
(loading takes wall-clock time), so compare frames by scene, not by number.

Scripted path into gameplay (the first fight in the prologue starts around frame 10600):
`recomp/gameplay_args.txt` holds the `-AutoPress` string (the opening sequence above, then Start /
Left / A to skip the cutscene). F12 in the game window logs every draw of the next frame to
`recomp/captures/capture_f<frame>.log` and saves its image (not yet verified).

ALU exports follow Xenia's rules (`ucode.h`, `GetConstant0WriteMask` / `GetConstant1WriteMask`): both
the vector and scalar results go to the vector destination, components in both write masks receive
1.0, and with the scalar-relative bit set, components in neither receive 0.0. Getting this wrong left
full-screen quads with position w = y, which clipped the top half of every post-processing pass, so
the prologue showed nothing above the horizon. Fetch source swizzles are absolute (the disassembler
used to print them relative).

Register meanings were checked against Xenia's `register_table.inc` and `registers.h`; every bit field
used here matches. `SQ_PS_CONST` reads 0x000FF100, so pixel constants do start at 256. Not yet
handled: alpha test (`RB_COLORCONTROL` at 0x2202), culling, vertex texture fetch, mipmaps, and EDRAM
addresses wrapping every 2048 tiles.

Render targets are keyed by EDRAM address and width, with height covering the rest of EDRAM, because
the same address is reused at several sizes in one frame; replaced resolve textures are retired only
after the frame's GPU work completes.

Test runs open the window on the secondary monitor (`NIER_MONITOR=secondary`, set by `run_boot.ps1`;
a number picks a monitor by enumeration order).

More debugging switches:
- `NIER_PROFILE=<frame>,<seconds>[,exit]`: samples every thread about 1000 times a second from that
  guest frame on and logs each busy thread's hot functions, inclusive time, callers and waits;
  `exit` ends the process after the report.
- `NIER_DUMP_DRAW=<vs hash>[,...]` with `-LogDrawsAt <frame>`: writes the next three draws with each
  shader to `recomp/captures/` (byte-swapped vertex data, indices, constants, the slot 16 texture).
- `NIER_WATCH_WRITE=<guest address>,<bytes>` (and `NIER_WATCH_FROM=<frame>`): write-protects that
  page (in both physical windows for 0xA/0xC addresses) and logs the guest call chain and registers
  of each writer (registers are as last stored to the context, so they can be stale).
- `NIER_BREAK=<guest function>,<frame>` (`NIER_BREAK_MIN_R3`, `NIER_BREAK_DUMP=r5:192,r6:96`): a
  hardware breakpoint on the function's recompiled code logs the guest registers (exact, at entry)
  and memory at given registers. Overriding a recompiled function by defining `sub_X` in the runtime
  does not work for callers in the same generated file (they call `__imp__sub_X` directly).
- `NIER_SCAN=<f1>,<f2>,<f3>[;...]` with `NIER_PROFILE=...,exit`: logs guest addresses holding those
  big-endian floats (within 0.01); `NIER_PROFILE` exits also dump every guest thread's stack.
- `NIER_FIXED_TIME=1`: the kernel time stamp bundle and the game's QueryPerformanceCounter advance
  1/60 s per submitted frame (experiment; did not affect the skeleton problem).

`NIER_PS_DEBUG=<hash>=<HLSL float4 expression>[;...]` replaces a pixel shader's colour with any
expression over its registers (`r[n]`), inputs and textures; splitting by `input.position.x` shows
several intermediate terms side by side, which is how the scalar operand bug was found.

Debugging switches: `NIER_SHOW_SURFACE=<hex>` shows a resolved surface instead of the scanout;
`<hex>@n` picks the n-th copy to that address in a frame, and a comma-separated list shows each entry
for 60 frames in turn (never per frame, which flickers badly in the visible window; save frames 60
apart to capture each pass; the log says which frame shows which). `NIER_SHOW_ALPHA=1` shows the alpha channel. `NIER_SKIP_SHADER=<hash>,...` drops draws using
those shaders. `NIER_NO_DEPTH=1` disables depth and stencil, `NIER_D3D_DEBUG=1` routes D3D12 debug-layer messages into
the log, `-LogDrawsAt <frame>` logs every draw's state. Crashes print a symbolized host stack from
`nier.pdb`. Scripted path from boot to the prologue (about frame 9200):
`-AutoPress 1300:10,2300:10,3000:1000,4400:1000,5000:1000,5550:4,5650:1000,6900:1000,7700:1000,8700:4,8800:1000`.

### System services and saves (`xam.cpp`, `io.cpp`)

Asynchronous requests complete immediately through the XOVERLAPPED. One profile is signed in as
"Player", with one virtual hard drive as device 1. `XamNotifyCreateListener` must return a real handle:
sign-in, storage and input notifications are what let the title proceed. Saves go to `save/<package>/`
beside the game folder, the only writable location. `XamEnumerate` always reports no content, so
Continue will not list existing saves yet. `run_boot.ps1` sets `NIER_MUTE` unless given `-Sound`.

### Movie smoothness

Unsolved. Optimization (now the default, `/O2`) barely changed it, and moving from GDI to a vsynced
D3D12 presenter changed nothing: gaps on screen match the gaps at the guest swap, and every frame is
presented. So the unevenness is produced before presentation. With D3D12, gaps between new images in
60Hz refreshes were 1:170, 2:716, 3:169, 4:6, 5:17, 6:65, 7+:23. The movie is 30fps, so 2 is correct.
The 1s and 3s are images landing a refresh early or late. Next things to look at: the movie library's
service thread (`sub_8251AC78`, which self-suspends and is resumed by the game), audio blocks being
requested in bursts of the device period, and whether the 6-refresh gaps line up with file reads or
keyframes. They may also be deliberately held frames in the source.

## Portability plan

Target: Windows, Linux and a macOS Universal binary, with D3D12 first and Vulkan tested later.
Only Windows x64 has been built or run so far. Known work:

- **Renderer:** `renderer.h` is backend-neutral. D3D12 is `renderer_d3d12.cpp`; add `renderer_vulkan.cpp`
  implementing the same functions. macOS has no native Vulkan, so it needs MoltenVK or a Metal
  backend. Xenos features such as rect lists may need emulation there.
- **Platform layer:** all runtime files except the generated stubs include `windows.h`. Threads, sync,
  files, window, WASAPI and XInput need cross-platform versions; SDL would cover window, input and
  audio.
- **Apple Silicon memory ordering (the largest CPU risk):** the recompiler emits nothing for `sync`,
  `lwsync` and `eieio`, which is safe only on x86's strong memory model. On ARM these must become real
  barriers. NieR has about 70 such sites and 134 `lwarx`/`ldarx` atomics.
- **Generated code:** `ppc_context.h` already has AArch64 paths for FP control and the timebase.
  XenonRecomp's weak-alias output is disabled on Apple, so generated code differs per platform
  unless alias use is turned off everywhere.
- **Page size:** Apple Silicon uses 16KB pages, which the guest memory setup must allow for.
- **Universal binary:** build x86_64 and arm64 separately, then combine with `lipo`. Needs a Mac.
- All of this also has to be built and tested on those systems; this machine can only verify Windows.

### GPU stub (`gpu.cpp`)

Parses the real PM4 stream including indirect buffers and carries out what the CPU can observe:
read pointer writeback, memory and scratch writes, fences, `WAIT_REG_MEM`, CP interrupts and vblank.
**Draws, shaders and textures are counted and discarded.** Facts learned from the game's own code:

- `0x7FC80714` is the ring write pointer, `0x7FC86110` the scanout surface, and `0x7FC86544` must
  have bit 0 set on vblank or the game's handler won't flip.
- CP interrupts are PM4 opcode `0x54`, delivered once per CPU in the packet's mask, with the CPU
  number in the PCR at `+0x10C`. The handler clears that CPU's bit in `*(device+10900)`.

### Still to do

- Real renderer: PM4 state tracking, Xenos shader translation, textures, EDRAM resolves and a
  D3D12/Vulkan presenter. This is the largest remaining piece.
- `EVENT_WRITE_EXT`/`ZPD` are ignored, so queries that read their results get nothing.
- `VdGetCurrentDisplayInformation` is still a stub that fills nothing.
- Stubs that return 0 without filling their output are suspect in general.
- Physical memory has one mapping only. If the game reaches a physical page through another alias
  (`0xC...`, `0xE...`), it will see different memory.
- 71 functions have exception handlers, and their `__except` bodies are only reachable through
  exception dispatch. `RtlRaiseException` and `RtlUnwind` are still stubs.
- XMA decoding is unimplemented and `XMACreateContext` is still a stub, so XMA-encoded music will be
  silent. The title movie's audio doesn't need it.
- Movie smoothness: see below.

### Resolution

PC Settings > Resolution chooses the size the game is drawn at, any of ten 16:9 sizes
(`resolution_scale` in settings.ini, an index: 0 640x360, 1 854x480, 2 960x540, 3 1280x720 = the
original and the default, 4 1600x900, 5 1920x1080, 6 2560x1440, 7 3200x1800, 8 3840x2160,
9 5120x2880; `NIER_RES_SCALE=<0.5-4>` overrides with a scale). A change applies between frames
(`ApplyResolutionSetting` at the start of `RenderFrame`, when the GPU is idle): every scaled
surface is dropped and the next frame creates what it uses at the new size, so surfaces resolved
in the frame before are missing for one frame (a brief dip in exposure). The menu stores the
value when the PC Settings page is left. `NIER_RES_AT=<frame>,<index>` changes it in a test run.
All of it is in renderer_d3d12.cpp: the scale is
kept in twelfths (`gResTwelfths`, 12 = 1x) so every size is exact, and `Scaled()` converts guest
lengths. Every colour target, depth target, resolved surface and resolved mip chain is created
at the scaled size, and draw viewports, scissors and the resolve copy rectangles are scaled to
match (copies are clamped to both textures, since rounding can add a pixel at fractional
scales). Everything the guest sees (target keys, sizes, rectangles) stays in guest pixels, and
shaders use normalized texture coordinates, so nothing else changes; shadow maps and
reduced-size passes scale too. Resolve write-back still hands the game pixels at its
own size: the read-back is averaged down (or up) to the guest size first. The game reads its
luminance reduction that way to set exposure; skipping it at other scales (an early version did)
left the exposure on a stale value, and the scene went nearly black at low sizes after a high
one (`NIER_NO_SCALED_WRITEBACK=1` reproduces that). New render targets and resolve textures are
also cleared when created, not left holding old memory. The window shows the result with one linear-filtered copy, so a size far above the
window's is not properly downsampled.
Checked on frame captures at 0.5, 0.667, 0.75, 1, 1.25, 1.5, 2, 2.5 and 3 (all draws run, the
picture is the same, softer or sharper) and in live runs at 1.25 and 2 (title, save list,
village). At 2x the single-pixel highlights on characters go from 46 to 3 in the Nier area of
capture f7338. Not measured: frame rate at any size. Not done: sizes that aren't 16:9 (16:10
for the Steam Deck is wanted later), choosing the size from the display, modern upscalers.

### 2D at the display's resolution

The Resolution setting sizes the 3D scene; the HUD, menus, text and the title screen are drawn at
the display's own size whatever it is set to (renderer_d3d12.cpp; `NIER_NATIVE_2D=0` turns this
off). A frame ends with the copy that produces the picture shown (the scanout). Before it the
game draws, into the same target and with no depth test, the final full-screen composite of the
scene and then every 2D element. `FindFlatPhase` finds that run by walking back from the scanout
copy: draws into that target that aren't depth-tested (depth test off, or one that always passes)
belong to it; it stops at the first depth-tested draw or at a copy out of that target (the
scene being captured); passes into other targets in between are left alone. In the village frame
it starts at the full-screen copy of the finished scene, then the bloom composite, then the HUD.
Those draws, and the scanout copy, run at the display's scale (`gDisplayTwelfths`: the largest
16:9 picture that fits the window against 1280x720, in twelfths) instead of the render scale:
`gActiveTwelfths` is the scale in force for the command being executed and is part of the key
of render and depth targets, so the display-sized target is a separate object from the
render-sized one with the same guest address. When the pass begins, the render-sized target's
picture is copied (scaled) into the display-sized one, unless nothing drew to it this frame
(menus drawn over the last picture); the pass has its own depth and stencil surface, cleared
once a frame (the minimap's stencil mask works); a clear that ends the frame is applied to
both targets. The composite then scales the scene texture to the display as it draws, so no
separate upscale pass exists. A window resize changes the display scale and drops every
surface, like a resolution change.
Checked: captures at render scale 0.5 and 2 in a 1280x720 window (HUD and markers sharp, scene
soft or sharp, stencil-masked minimap right); a live run at 640x360 (title, menu, save list,
gameplay HUD, pause menu) and switching to 2560x1440 while paused. Not checked: shops, dialogue
windows, movies with subtitles, loading screens, and 2D that is depth-tested (it would stay at
the render size). 3D-looking UI is unaffected by design.

### Shadow quality

PC Settings > Shadow Quality (`shadow_quality` in settings.ini: 0 Original, 1 High, 2 Very High,
3 Ultra; `NIER_SHADOW_SCALE=<1-4>` overrides) sizes the shadow map: 1024, 2048, 3072 or 4096
texels a side. The blocky character shadows were never a screen-resolution matter. Each frame the
game renders one 1024x1024 depth map of the casters from the light (commands into target 2D0/1040,
square viewport, ending in a depth copy to 07A7D000), then a half-size mask pass (target B8/640,
640x360; pixel shaders 1F81BF6889FC24EC and D0CE46D136802B42) reads that map with a hand-written
filter, nine taps one texel apart blended by the position inside a texel, and the result
(07E7D000) is laid over the scene. The map covers a wide area, so Nier fills about 5 by 15
texels: his shadow is a few squares at any render size.
`ShadowPasses` (renderer_d3d12.cpp) finds the shadow passes: a square depth copy of at least
256 texels, and the commands before it back to the previous copy (rectangles and draws through
a square viewport). They run at `gShadowTwelfths` instead of the render scale, so their depth
target and the resolved depth texture are that much larger; nothing else about them changes.
The filter's idea of the map comes from pixel shader constants the game sets: 257.z = 1024,
257.w = 1/1024, 258 = (0, 1, 2, 3) texels. For a draw that binds a shadow-scaled square depth
texture and has those constants, `ExecuteDraw` restates them for the real size (without that the
taps skip texels and the shadow streaks). Four times is the most: the map's surface is 1040 by
about 2500 guest pixels, and a texture side can't pass 16384.
Checked on captures with `NIER_SHOW_SURFACE=07E7D000` (the mask alone: squares at 1x, Nier's
outline at 4x), in the final picture of four scenes, and in a live run at 4x. Not measured: the
cost. Not done: the mask itself is still half the render size, so its edges soften rather than
sharpen at high render sizes; wide shadows (trees, clouds) come from elsewhere and are unchanged.

### Frame captures (the fastest way to work on a graphics bug)

F12 in the game (or `NIER_CAPTURE_AT=<frame>` in a test run) saves that frame's renderer commands
to `recomp/captures/frame_f<N>.nfrm` next to the draw log and the picture: every draw's shaders
(with their microcode), raw vertex data, constants, state and decoded textures, and the resolves
(framecapture.cpp; about 90 MB a frame, not committed). `tools/replay_frame.ps1 <capture>` renders
it without the game in about two seconds and writes a PNG beside it (or `-Out`); the picture
matches the game's. `-Cull 'mode,ccw'` forces face culling (captures made before culling
existed, version 1, hold no cull state). `tools/nfrm.py` reads a capture: lists commands,
prints a draw's vertex streams, histograms a bit field, returns constants. The shaders are translated again from the microcode, so translator changes,
`-PsDebug '<hash>=<expr>'` and `-Patch '<hash>|<find>|<replace>'` apply. `-Zoom 'z,x,y'` magnifies
the scene z times around the screen point x,y (0-1); it is a real close-up, not an enlarged
picture: the viewport of depth-tested draws into full-width targets is scaled, while full-screen
passes, the HUD and shadow passes are left alone. `-Skip '3,10-14'` leaves commands out and
`-Stop n` drops every draw from command n on; `recomp/replay.log` lists the commands by index
with their shaders and textures. `-Show` keeps the window open. Limits: one frozen frame, so
nothing that depends on game logic or timing; textures that the previous frame resolved are
approximated by replaying the frame repeatedly (40 times by default); texture decoding is not
redone (the capture holds decoded textures).

### Recordings (10 seconds of frames)

F9 in the game (or `NIER_RECORD_AT=<frame>[,<seconds>]` in a test run) records every frame for 10
seconds to `recomp/captures/seq_f<N>.nseq`, for faults that only show in motion and for stutter
(framecapture.cpp). Shaders, textures and data blocks (vertex data, indices, constants in
256-float pieces) are stored once however many frames use them: about 650 MB for 10 seconds at
The Aerie. Beside it a `.txt` has one line per frame: milliseconds since the frame before, new
textures (count, KB), new shaders, new data KB, and the renderer's waits in that frame (shader
compiles, pipeline creation, pipeline waits), then `frames: N`. Recording costs a little time
per frame (it hashes the frame's data), so judge stutter from the pattern, not the absolute times.
`tools/replay_frame.ps1 seq_f<N>.nseq` plays it once at 60 frames a second and saves every
`-Every`'th frame (default 6; 1 for all) as .ppm into `<capture>_frames/` or `-Out`; `-Show`
loops it in a window; `-Surface <address>`, `-PsDebug`, `-Patch`, `-Zoom`, `-Cull` work as for a
single capture, and `recomp/replay.log` lists each recorded frame's interval.
A playtest now logs to `recomp/playtest.log`; test runs keep `recomp/boot.log` (they used to share
the file when run at the same time). Test runs and replays made while the user plays also compete
with their game for the CPU and the graphics card: avoid them while a stutter is being judged.

### Playtest issues

Saves: the user's playthrough saves live in `save/` (backup in `save_backups/`). Test runs from
`tools/run_boot.ps1` (anything but `-Playtest`) set `NIER_SAVE_DIR=recomp/test_save`, a fresh copy
of `save/` made every run, so they can load the playthrough (Game 1) without writing to it. Test
routes load Game 1 by default rather than start a new game, unless the user says otherwise.

From the user's playthrough (open unless marked):
- Blocky shadows: see "Shadow quality" (a setting; awaiting the user's check).
- FIXED (confirmed by the user in play): square patches on the sheep's
  legs and beards, Yonah's dress and hair, Nier's waist cloth, AND the dust that filled the screen
  in the cutscene after the Lost Shrine boss. One cause: no face culling. Cloth, fur and hair are
  modelled double-sided as two coincident layers, every vertex twice at the same position with
  exactly opposite normals (877 of 1,234 seam pairs on Yonah's dress), and the game culls back
  faces (PA_SU_SC_MODE_CNTL, register 0x2205: bit 0 cull front, bit 1 cull back, bit 2 clockwise
  is front). The renderer drew with culling off, so the inward layer fought the outward one at
  equal depth and won in patches; the dust and light-ray effects likewise showed the faces that
  should be hidden. Now `RendererDraw::cull` / `frontCounterClockwise` (gpu.cpp BuildDraw,
  pipeline state in renderer_d3d12.cpp); a draw with both faces culled is skipped; rectangles
  are never culled; `NIER_NO_CULL=1` turns it off. In the village frame 163 draws cull back
  faces, 92 don't cull, none cull front. Found with a frame capture: showing `input.t[0]` (the
  interpolated normal) with -PsDebug gave a checker, and tools/nfrm.py showed the paired vertices.
  Dead ends on the way, all ruled out: NaN in shader 46155F8651382D13 (the select translation
  was still wrong for NaN and is fixed; its guard is now `precise`), IEEE strictness, a precise
  vertex position, broken mip levels, the 4x4 texture in slot 10 of Yonah's dress, handedness.
- Test tools added for this: `run_boot.ps1 -Record <file>` / `-Replay <file>` (NIER_RECORD_INPUT,
  NIER_REPLAY_INPUT: the pad state by frame; scripted presses still apply during a replay) and
  NIER_AUTO_MOVE=<frame>,<frames>,<lx>,<ly>[;...] for the left stick. Test runs ignore real
  controllers (NIER_IGNORE_PADS=1, set by run_boot.ps1 unless -Playtest or -Record). `recomp/recordings/shrine_roof.txt` (not committed)
  loads Game 2 (Lost Shrine: Roof) and walks through the boss door to the altar where Yonah
  lies, about 5,000 frames; Examine there only says a force blocks the way.
- DLC ("The World of Recycled Vessel"): works, confirmed by the user after getting Grimoire Weiss.
- Sparkles on Nier (single bright pixels on skin, hair and armour, e.g. capture f7338 in his
  house): NOT a bug; the user checked reference footage of the original and it shows them too.
  They are specular highlights of the character shaders at normal size (gone when zoomed in;
  the same count with and without face culling; they vanish when D461B8752A20D67B and
  EDAC9A9A457EFED6 are painted flat).
- Mouse on the menus other than the settings pages (title, pause tabs, save list, shops): the
  user reports the cursor stuck at the screen edge there while the game has focus. That is the
  camera capture (hidden, confined cursor), not a fault: no pointer exists for those menus yet.
  It needs to know when such a menu is on screen, and three cheap signals don't work: the legend
  texts "Select" 11100000 / "Confirm" 11100100 are looked up once when a menu opens (frame 854
  save list, 3300 pause menu in the load route), not every frame; a paused frame still draws the
  whole 3D scene (316 depth-tested draws against 201 in play); nothing marks a menu closing.
  Next step: log the guest call chain at those legend lookups to find each menu's object and its
  per-frame handler (as sub_8241C020 is for the settings pages), then hook that.
- Shadows "still flicker" at higher Shadow Quality (user, at 3840x2160 with shadow maps x3 / x4):
  NOT reproduced. Standing still (Game 3) the shadow mask is identical frame to frame (955 +/- 1
  dark pixels over 10 frames); in a 3-second recording of running, the mask follows the stride
  smoothly with no dropouts. Guess, untested: the nine-tap filter is one texel wide, so at a
  larger map the edge is harder and its stair steps crawl as the light's projection moves; a
  wider filter or a blur of the mask would hide it. Needs an F9 recording from the user.
- Micro stutter at The Aerie (Game 1): reproduced. Walking for 10 seconds with nothing else
  running, 6 to 7 of about 593 frames take two refreshes (while recording; 4 of 604 under the
  profiler), none with new textures, shaders or pipelines. The scene is about 670 draws a
  frame; the command-processing thread works 10.6 ms of each 16.7 ms frame on average and
  18.1 ms in the late ones, so it has little headroom. Its time (NIER_PROFILE): OnDraw 56%, of
  which texture hashing in DecodeTexture 14% (XXH3 over small textures on every use),
  AssemblePrimitives 14% (index lists rebuilt per draw), copying vertex data 9%, byte-swapping
  it 9%, allocation 7%. The game's main thread is 100% busy in sub_82579D50 <- sub_82573758 <-
  sub_8257AB38 (looks like a spin wait in the D3D library; not examined). Next: cut the command
  thread's work (memoise small texture hashes per frame, a bulk path for plain triangle lists,
  share vertex data between a mesh's passes) and see what makes the late frames heavier.
- "Magic orbs show a square outline" (user, possibly since the resolution change): no capture
  of it exists yet (the newest on disk, f8452 / f8519, show none). The user will capture it in
  the Aerie boss fight.
- DLC notice on Continue ("The quest "Recycled World" is now available." and where to find it:
  info-window messages 2481 / 2482, two pages to confirm on every load) is skipped: sub_8219B0D0
  shows it through sub_823C5140 while a flag at its object +0x20C00 is set, then clears the flag;
  the hook `SkipDlcNotice` at 0x8219B300 clears the flag and jumps past (xam.cpp;
  NIER_DLC_NOTICE=1 keeps the notice). The load route in loadsave_args.txt still works; its
  extra A presses now land after the save has loaded.
- "Press START to Skip" in cutscenes: text 11202000 now returns "Press <ICON=13> to Skip"
  (confirmed by the user). `<ICON=n>` in game text draws cell n of
  the button atlas (0 A, 1 B, 2 X, 3 Y, 4-7 LT RT LB RB, 8 L, 9 R, 11 R3, 12 Back, 13 Start,
  16-19 d-pad right left up down); the tutorials use it.
- PC settings menu: phase 1 done (see "PC Settings page"). Still to come, by phase: mouse input and
  rebinding (controllers and input-aware glyphs are done, see "Controllers and button glyphs"); graphics quality
  (resolution scale, shadow resolution, ambient occlusion); a Vulkan renderer (then DX12 / Vulkan).
- Fixed (confirmed by the user): missing colour in the 3D scene (house grey, open world
  washed out; HUD and 2D fine). The final composite (ps A386CE74148E6F9C) converts to HSV, adjusts,
  and converts back inside a `setp_ne_push` branch. We translated the predicate push as
  `a.w <test> && b.w == 0`; Xenia's ucode.h has `a.w == 0 && b.w <test>` (and the result uses the x
  components the same way). With a = 0 the branch was never taken, leaving only the value channel.
- Fixed: the storage messages at the title screen are skipped (see "Storage messages" below).
- Fixed: "Select Device" removed from Options / pause Game Settings and the save list (see below).
- Change "console" to "PC" in the remaining save texts ("Saving game data. Please don't turn off
  your console." etc.). The English message table is in guest memory (keys `INFO_SYS_MES_*_SID`,
  `INFO_WINDOW_SAVE_LOAD_*`, around 0x85803400 in one run); `NIER_SCAN_TEXT=<text>` (at a
  `NIER_PROFILE` exit) finds strings as bytes or UTF-16.
- Fixed (confirmed by the user): village fog with the Game 2 save. The camera-attached mist
  emitter (effect 0x220, ~330 faint sprites) is normally drawn straight into the scene; in some
  states the game draws it into a half-resolution layer (EDRAM 0x2D0, 640x360) and composites it.
  D3D clears that layer with a depth fill: a depth-only rectangle (always pass, depth write)
  through a 4x MSAA depth surface at the same EDRAM base as the colour surface, which overwrites
  the colour pixels. Our colour and depth surfaces are separate, so the layer was never wiped and
  the mist accumulated frame on frame. gpu.cpp now turns such a fill (mode 5, rect list, depth base
  == colour base) into a clear of the colour target with that base and width, value = 24.8 depth
  read as 0xAARRGGBB. Ruled out on the way: DLC, 60 fps, game clock, emitter state (identical in
  Game 1 and Game 2). Debug aids added: `NIER_BREAK_DUMP=rN@offset:bytes` (follow a pointer),
  `NIER_BREAK_IF=<offset>:<hex>` + `NIER_BREAK_W8=<offset>:<value>` (poke matching objects),
  `run_boot.ps1 -SaveSlot N` (test copy keeps only that save; route in recomp/loadsave_args.txt:
  A every 200 frames from 760 handles Continue, two DLC notice pages, select, confirm).
- Quit to Desktop: replace the title menu's "Downloadable Content" (store only). Its label is a
  texture (not the text table), so it needs texture replacement (also wanted for button glyphs);
  the entry is greyed out without Xbox Live.
- Fixed: music silent after the intro; crash at frame 42445 (switch index); fullscreen option.

### Storage messages

The title screen showed "Checking system data.", "Loading system data. Please don't turn off your
console.", "System data loaded." and, before the save list, "Checking storage device. Please don't
turn off your console." (about 4.5 s in all). They go through the system message box at 0x826920F0
(+4 type, +8 state, 0 = idle, +12 text, +16 seconds left; animated by sub_82184090): open
animation, a minimum time (1.0 s, 1.3 s from the constant at 0x8200EA64), close animation, and the
save state machines (sub_8213D208 check, sub_8213D488 load) wait for the box to go idle. The
message call sub_8213BE50 (virtual, reached only through the indirect call table; r4 mode, r5
message, f1 seconds) is wrapped in xam.cpp to drop messages 0 and 7 (mode 2) and 8 (mode 0); the
box stays idle and the saves load at once (save list at frame ~800 instead of ~980 on the scripted
route). `NIER_STORAGE_MESSAGES=1` keeps them. `NIER_POKE32=<address>=<value>,...` overwrites image
words at load (experiments).

### Controllers and button glyphs

Input goes through SDL3 (input.cpp; `tools/get_sdl3.ps1` fetches the official VC devel package into
`tools/SDL3`, hash-checked, not committed; `SDL3.dll` is copied beside the exe). An SDL thread owns the
devices; every connected controller (Xbox, DualShock 4, DualSense, Switch Pro, USB or Bluetooth)
drives player one, merged, with rumble renewed every 0.7 s (SDL rumble has a duration); the
touchpad click doubles as Back. The keyboard still acts as player one's pad. `InputLastStyle()`
tracks the device used last (Xbox / PS4 / PS5 / Nintendo / keyboard).

Glyphs: the game draws all button icons from one atlas (128x192 DXT5, base 13A1D000 on the save
list; 4x6 grid of 32-px cells: A B X Y / LT RT LB RB / L R L3 R3 / Back Start D-pad Guide / D-pad
right left up down / L left-right, L up-down, the game emblem, empty). glyphs.cpp recognises it by
shape and content (empty last cell, opaque A cell) at the end of `DecodeTexture` and hands the
renderer one of our atlases instead (256x384 RGBA, drawn by `tools/make_glyphs.ps1` into
`recomp/build/glyphs/*.rgba` at build time; the emblem cell is copied from the game's atlas):
xbox (Series), ps4, ps5, steam (Steam Deck / Steam Controller: Valve vendor id 0x28DE or SDL's
STEAM type), keyboard, chosen by the last device used. `NIER_GLYPHS=<set>|original`
forces one. PC Settings > Button Icons (`button_icons` in settings.ini: 0 Auto, 1 Xbox Series,
2 PlayStation 4, 3 PlayStation 5, 4 Steam Deck, 5 Xbox 360 = the game's own) picks the controller
set; keyboard and mouse always switch to the keyboard set on their own. Help texts longer than
about 60 characters run off the help box: break them with a newline (the box holds three lines).
Still to do: DualSense extras.

Title prompt: "Press the START Button" on the title screen is not text and not part of the movie
(TITLE_NIER_ALL.SFD is the intro that plays before the title; its length in frames varies run to
run, so tests skip it with one Start press, e.g. `-AutoPress 1750:10`, and capture ~400 frames
later). It is two DXT3 images of the title layout (2d	itle), one colour with the shape in the
alpha: the letters (255x14) and their glow (265x26), centred on the same point; the game fades
and pulses them. `GlyphAtlasFor` recognises them by format and size and returns "Press the
(icon) Button" ("Key" on keyboard; GDI+, keyglyphs.cpp `DrawPromptMask`, in the image's own
colour) with the Start icon of the icon set in use (atlas cell column 1, row 3, cropped to what is drawn and scaled to the prompt's
height), at twice the size: Options on PS4 / PS5, the menu button on Xbox Series / Steam, the
keycap of the key bound to Pause Menu on keyboard; the original images with the Xbox 360 icons.
The letters go in both images, the icon only in the glow image (the letters image is 14 pixels
high), so the icon pulses with the glow. Two text-table strings "Press the START Button" /
"...button" (ids 1112500 and 1500400, found by text in `PcSettingsText`) are reworded instead
("Press the OPTIONS Button", "Press the MENU Button", "Press the Esc Key"); where the game shows
those is not known.
The table (r3 of sub_823CB8C8) holds { id, string } pairs: up to 2935 at +0 (count at +23480) and
up to 64 at +23484 (count at +23996).

### Key bindings

PC Settings has "Key Bindings: Buttons" and "Key Bindings: Movement" (12 rows each: a page holds
24 records). Each control has one key (input.cpp `kActions`, saved as `key_<id>` in settings.ini).
A row is a two-way record whose value text is the key's name; pressing Left / Right changes it,
the game then asks for its value text (sub_82416C20), which starts a capture ("Press a key...").
`PcSettingsTick` (hook at the menu handler's entry, every frame) takes the next key pressed once
all keys are released, binds it (swapping with a control that already had it), saves and lays the
page out again; Esc or moving the cursor cancels. Keys don't act as the pad during a capture.
Our action rows (item ids 70-72) are handled by `PcSettingsAction` at 0x8241C778.
`NIER_FAKE_KEY=<frame>:<vk>` presses a key for tests. The keyboard glyph set is drawn at run time from
the bindings (keyglyphs.cpp, GDI+): a keycap per control with the key's short label, rebuilt when a
key is rebound; NIER_DUMP_GLYPHS=1 writes it to captures/keyboard_atlas.rgba. Rows are named by
action (Jump / Confirm (A), Attack (X), Ability Slot (LB), Map (Back), Reset Camera (R3), Item /
1H Sword / 2H Sword / Spear Menu on the D-pad); L3 does nothing in the game and is hidden. Mouse: raw
mouse movement (WM_INPUT in present.cpp, while the window is focused and Mouse Sensitivity > 0;
the cursor is hidden and clipped to the window) becomes right-stick deflection in input.cpp
(speed-based, lifted over the dead zone; untested by hand so the scale may need tuning). Mouse
buttons are bindable like keys (VK 1, 2, 4, 5, 6) and have their own icons; defaults: Attack =
left, Heavy Attack = right, Reset Camera = middle. Menu pointer (first
screens: the settings pages, kinds 4-7): PcSettingsTick calls MenuPointer each frame the page is
open; it keeps "pointer mode" on (cursor shown, no camera, mouse buttons not game controls),
maps the cursor to a row (rows 38 px apart from y 172, x 240-905 in 1280x720 space, 9 visible),
writes the cursor row (+3576) and lays the page out again on hover, and injects pad presses for
clicks (A on action rows, d-pad left/right by click side on values, B on right click, d-pad up/down
on the wheel). NIER_FAKE_MOUSE=<frame>,<x>,<y>[,<click frame>] simulates it. Still to do for the
pointer: top Options page, title menu, save list, pause tabs, shops, dialogue choices.

### Direct mouse camera (in progress)

Goal: turn the camera by the mouse delta instead of emulating the right stick. Found so far:
- Global camera at 0x826B40A0, updated by sub_821A6C10 <- sub_821A7D00(camera, dt) <- sub_82193538 /
  821935C8 / 821963C8 (manager 0x82691E70) <- sub_82191D60. It holds only results: rotation rows at
  +0xE0 / +0xF0 / +0x100, eye at +0x110, and a copy at +0x120.. (+0x150 eye).
- Heap object 0x432DB0A0 (vtable 0x8200F3F8, effects module) has the same matrices and eye (+0xD0..);
  also derived.
- Holding the right stick fully for 60 frames turns exactly 1.6 rad (0.02667 rad per frame at 60 fps);
  initial pitch is 0.3 rad. No plain yaw / pitch float found yet in 0x82691E70-0x826B4500 or the
  globals 0x82680000-0x826D0000, so the angles live in a controller object elsewhere (heap).
- Pad: XamInputGetState fills 0x85F4FCAC (rx at +12), read by sub_8258CE90 <- sub_8258D400 <-
  sub_8258D658 (the XInput wrapper) from the main loop sub_82496D00; the game's pad object at
  0x827CE7A0 (sub_8210A3B0) showed no stick floats in its first 8 KB.
- Tools added for this: NIER_AUTO_STICK=<frame>,<frames>,<rx>,<ry>, NIER_SCAN_F32=<value>,<tol>,
  NIER_LOG_PAD=1.
Next: follow sub_8210A3B0 (it calls 0x8258DB38 / DB90 / DBA8 with floats) to where the processed
right stick is stored, watch reads of that, and find the yaw / pitch update in the camera code.

### Downloadable content

The user's DLC ("15 Nightmares", "Extra outfit and battle quest pack": `media/DLC01.cpk` plus
`name.txt`) lives in `dlc/535107E8/00000002/<package>` as a console stores it (git-ignored;
`NIER_DLC_DIR` elsewhere). dlc.cpp reads STFS packages (consecutive files only so far) and xam.cpp
lists them for marketplace content (type 2) and, when opened, unpacks one once into `<package>.d/`
and serves it read-only with a full license. `tools/stfs.py` lists or extracts a package.
The game lists DLC at boot (sub_82116548 keeps items that pass sub_8255B970 in its manager, count
+23352, entries of 328 bytes from +2376) but mounts one only on request: sub_82116B48 / 82116BB0
queue it (from sub_8211E658 and sub_8211EB70, called by sub_82117400 and the manager thread
sub_8211BD00); sub_82116670 then opens it and reads name.txt. In the story it is offered at Nier's
house after getting Grimoire Weiss (a second sparkle; Grimoire mentions "the world of the recycled
vessel"), so it is untested until the playthrough gets there. The title menu's "Downloadable
Content" only opens the store (greyed out without Xbox Live); it is to become Quit to Desktop.

### PC Settings page

`settings.ini` beside the exe (`NIER_SETTINGS_FILE` elsewhere; test runs use
`recomp/test_settings.ini`) holds borderless, frame_rate_60, vsync, master_volume (0-10) and
mute_in_background; `NIER_FULLSCREEN`, `NIER_60FPS`, `NIER_30FPS` override it. It is edited in game:
"Select Device" at the bottom of Game Settings (title Options and pause menu) is relabelled "PC
Settings" and opens our page in place of the device selector (pcsettings.cpp; hooks in
nier_hooks.toml):
- Text: our text ids (0x7E000000+) are answered at the text lookup sub_823CB8C8; a record's value
  text ("Borderless", "60 fps") at sub_82416C20.
- Open (action item 27 in the input handler sub_8241C020, case at 0x8241C828): the Game Settings
  records, record count (+5264), first record in view (+3572) and cursor row (+3576) are saved, ours
  written (item ids 60+, which the game's config copy sub_82415C48 and action switch ignore), and the
  page laid out again with the calls the builder makes after its records (824197A8, 824178D0,
  82416168, 82419120, 82416038), without its config copy, so pending Game Settings changes survive.
- Return / B (sub_8241BB60): on our page, the values are read back, saved and applied, and the Game
  Settings records restored.
Display Mode posts to the window thread; Frame Rate is checked on every flip (the 60 fps hook is
always installed); VSync off presents with tearing allowed when DXGI supports it; volume and
background mute scale the final mix with a short ramp. Alt+Enter / F11 also update the setting.
`NIER_WATCH_READ=<address>,<bytes>,<from frame>` catches reads (it found the text lookup).

### "Select Device" removed (PC port)

Mid-asm hooks now exist: `recomp/config/nier_hooks.toml` (appended last by `make_config.ps1`,
since `[[midasm_hook]]` tables would capture keys after them), implemented in xam.cpp.
- Options / pause Game Settings: the builder sub_8241A188 fills 24 records of 56 bytes at +3916
  (+0 type, 0 = unused; +40 item id; +44 label text id; +48 help text id), counts the used ones into
  +5264, then lays them out. `HideSelectDeviceOption` (at 0x8241B7DC, between the two) removed the
  record labelled "Select Device"; superseded by the PC Settings page (below), which relabels it.
- Save list (load and save screens; refreshed by sub_82427978): the 4th cursor stop is the device
  row. `DisableSelectDeviceRow` clears its enable byte (+8367; sub_824274E8 skips disabled stops)
  and `BlankSelectDeviceRow` sets its label to an empty string. The row's underline still shows.
- Text ids: the loaded text table is {id, string} pairs (e.g. around 0x85831000); 6740000 is Audio
  Output, not Select Device. `NIER_STORAGE_MESSAGES=1` keeps all of this original.
- Scripted route to the save list / Game 1 on the test saves: Start every 60 frames to 600, then
  A at 760 (Continue), 880 (Game 1), 1000 (Yes). The pause menu opens by itself in unfocused test
  runs. Title Options: Start to 600, Down at 760, A at 800, A at 900 (Game Settings); Up wraps to
  the last item.

### Jump-table switches index with 32 bits (fixed)

Playtest crash at frame 42445 in sub_821DB288: access violation at a host address 16 GB from the
code. The function bounds-checks its switch index with `cmplwi` (32-bit), but the generated
`switch (ctx.r11.u64)` used all 64 bits. Its argument r4 was -28 with zero upper bits, so the index
became 0x1_0000_004A, passed the guest check, and clang's jump table (no default range check) was
read far out of bounds. The hardware forms the target from the low 32 bits (`rlwinm`), so
XenonRecomp now emits `switch (rN.u32)`. All 248 tables changed; `tools/xenonrecomp-nier.patch`
is updated.

### Fullscreen

Borderless fullscreen on the window's monitor: Alt+Enter or F11 toggles it, `NIER_FULLSCREEN=1`
starts in it (the cursor is hidden over the game). `tools/run_boot.ps1 -Playtest` runs for the
user: sound, primary monitor, fullscreen.

### Music silent after the intro (fixed)

Sound effects and voices played, but the game's music (CRI ADX stems streamed from
`AUDIO/NIER_BGM.CPK` inside `layer1.cpk`, e.g. `Synth/000_tokyo_choir_cmp_wav.aax`) was silent.
The stems were read and decoded at real-time rate, and the game set the layer AISAC controls
(`layer00`..`layer03`, through sub_824E85F8) correctly; forcing all of them to 1.0 changed nothing.
The cause was `XMsgInProcessCall` for the XMP app (0xFA), message 0x7001B
(`XMPGetPlaybackController {client, controller*, locked*}`): the stub returned success without
writing the outputs, so the game read stale memory as "the user's own soundtrack is playing" and
muted its music. It now writes 0 to both (the title controls playback, as Xenia does), and
`XMPGetStatus` (0x70009) reports idle. `NIER_AUDIO_DUMP=<frame>,<seconds>` records the mix to
`captures/audio.wav` for checks like this.
