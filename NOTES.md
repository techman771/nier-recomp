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
Command thread, second round (with the frame rate unlocked it is what limits the game: 99.5% busy
at The Aerie). Measured with the 240 cap, VSync off, the load script's frame numbers times four
(fully uncapped, scripted presses are over before a menu reacts), NIER_PROFILE=11000,10:
107 fps before. Its time went to CopySwapped 23% (vertex data), texture hashing 21%,
AssemblePrimitives 19% with its zero-filled result, and about 20% in building and moving the draw
record (emplace, memcpy, memset).
- Command lists are recycled: each frame's list used to be a new allocation, so every draw's
  10 KB record landed on untouched pages; the render thread now empties a finished frame's list and
  hands it back (`RendererTakeCommandList`). Draws are built in place at the end of the list
  (`#define draw (*building)` in BuildDraw, taken off again by `Pending` on an early return; the
  depth-fill clear is inserted before it) and `RendererDraw() {}` leaves `constants`
  uninitialized. Building in place alone gained nothing; with recycling: 140 fps.
- Texture hashing: block-compressed textures (never edited in place, only replaced whole) get a
  sampled check per frame, 64 bytes at 48 places, and a full hash only when it changes or every
  600-855 frames; plain formats (glyph sheets are edited a glyph at a time) are still hashed in
  full once a frame. NIER_HASH_FULL=1 for the old way. (Guest physical memory is file-mapped
  views, so write watching isn't available.)
- Triangle strips, most of the geometry, are drawn as strips (`RendererDraw::strip`, the restart
  marker 0xFFFFFFFF being D3D12's strip cut value, set in every pipeline) instead of being expanded
  to lists; the vertex range scan skips the markers. Captures record the flag (NFRM 5, NSEQ 4;
  older ones still load). 170 fps, the picture unchanged at frame 3000 of Game 3.
- AVX2: `compile_ppc.ps1` builds with `/arch:AVX2` (AVX2, BMI, FMA; `-CpuArch /arch:AVX` for the old
  target) and `-ffp-contract=off`, so that the recompiled code's `a * b + c` (how fmadd is emitted)
  and everything else compute exactly as in the AVX build that was tested, not fused. main.cpp
  refuses to start on a processor without AVX2, with a message box. The opcode tests pass (1663
  checks). 185 fps in the same measurement (170 before; the copy-and-swap loop is the main gainer).
- User after the three changes above: "so far no broken visuals".
- Left: CopySwapped 29% of what remains (vertex data copied and swapped for every draw, then
  copied again by the render thread into the upload arena; swapping in the vertex shader and
  writing straight into a per-frame upload buffer would remove most of it), XXH3 14%, heap 7%.

Frame rate caps (asked for by the user after Uncapped gave no trouble: the Steam Deck OLED is 90 Hz,
and some prefer a cap to VSync): Frame Rate is now 30, 60, 90, 120, 144, 240 fps and Uncapped
(setting `frame_rate`, the index 0-6; the old `frame_rate_60` is still read: 0, 1, and anything
higher as Uncapped; NIER_FRAME_RATE=<index> for tests). 30 and 60 keep the fixed 60 Hz vblank. The
rates above are Uncapped with a limiter (`FrameRateCap`): the vblank follows each queued flip, no
sooner than 1/cap after the one before (or one display refresh with VSync on, whichever is longer),
the earliest moment stepping on by exactly that gap so the average is exact. Measured on Game 3
with the counter on (the user asked for the counter to be on when testing caps): the 90 cap gave
90.0 fps and "90 fps" on the counter; the 120 cap 115 fps on the 120 Hz display. Not tried: 144,
240, the menu row.

Frame Rate above 60, as first built (setting `frame_rate_60`: 0 30 fps cap, 1 60, 2 Uncapped, experimental). The
user tried 120, 240 and Unlocked in a playtest and asked for 120 and 240 to be dropped; fixed rates
above 60 now exist only through NIER_GUEST_HZ. What follows describes all of them, except that
Uncapped no longer follows the display's refreshes with VSync on: the user found that it behaved
like double-buffered vsync (a late frame cost a whole refresh), where a G-Sync display shows a late
frame when it is ready. Uncapped now always raises the vblank when a flip is queued, no sooner after the last than one
display refresh with VSync on (a limiter; presenting alone didn't hold it: a menu ran at 665 fps on
a 120 Hz display, with half the frames dropped) or 1 ms with it off. What paces the game is its vblank, which the runtime raises itself
(`VblankThread`); `GuestRefreshHz()` gives its rate (NIER_GUEST_HZ=<rate>, or -1 for unlocked, for
tests). 120 and 240 simply raise it; where the display's rate is a multiple, the vblank follows the
display's refreshes as at 60 and each frame is held for that many. Unlocked: with VSync on, a vblank
for every refresh of whatever display the window is on (so 144 or 165 Hz displays get their own
rate); with VSync off, a vblank as soon as the game queues a flip (`gVblankKick`), at least 1 ms
apart (left free, a menu ran at 2,700 frames a second). The video mode still reports 60 Hz to the
game. Speed check (Game 1's opening camera cuts, 60 against 120 Hz with the scripted presses' frame
numbers doubled): the shots pass at the same real-time pace, so the game is timed by the clock, not
by frames. At The Aerie the game doesn't reach 120: 97 fps in the opening shots, about 70 in the
ones after, every frame still one vblank long; the vblank thread itself only gets that many through
(the interrupt it delivers appears to wait for the frame's work), with the command thread at about
8.7 ms a frame. Not tried: 240, Unlocked without VSync, the menu row, anything but that scene, and
whether physics or scripted events misbehave at high rates.

Frame rate counter (PC Settings > Frame Rate Counter, `show_fps`; NIER_SHOW_FPS=1 forces it on):
`DrawFpsCounter` counts the game frames presented and twice a second redraws "NN fps" on a dark
plate (`DrawSmallLabel` in osk.cpp, GDI+), shown in the top left corner of the picture through the
overlay pipeline. Checked in a test run ("60 fps"); the menu row hasn't been looked at.

Dark picture at 1600x900 (user's capture seq_f32624: dark while at 900p, normal after switching
to 720p; the darkness is in the recorded constants, i.e. the game's own exposure). Reproduced with
no upscaler: mean picture brightness 37 at scale 1.25 against 84 at 1. The game reads the end of
its luminance reduction back (512x512 ... 32x2) to set the exposure; at a fractional scale the
smallest surfaces have a fractional viewport (32x2 -> 40x3 with 2.5 rows drawn), part of the last
row is never written and the average is too low. `DataPasses` now finds every colour copy that
doesn't have the screen's shape (and isn't the picture) with the commands leading to it, and those
passes run at `DataTwelfths()`, the whole multiple of the game's size at or below the render scale
(4K, 3x, is unchanged; 900p and 1080p use 1x). The chain has to go together: its surfaces are
levels of one texture (running only the surfaces of 64 pixels and less at 1x blew the exposure out
at 1.5x and 3x). Brightness at frame 3000 of Game 3 now: 83.7, 83.6, 83.8, 83.9, 83.8, 83.3, 83.7
at 0.5, 0.667, 1, 1.25, 1.5, 2.5 and 3x. Awaiting the user's check. The blur buffers depth of
field and bloom use are part of that chain, so at fractional scales they are now the game's size.

Variable refresh rate (G-Sync / FreeSync): nothing to add for it to engage. It needs a flip-model
swap chain presenting with sync interval 0 and DXGI_PRESENT_ALLOW_TEARING in a window covering the
display, which is what VSync Off already does (FLIP_DISCARD, the tearing flag when DXGI supports
it), and with VSync off the game's vblank runs from its own timer (or follows each flip, in
Unlocked) instead of the display's refreshes. So: VSync Off + Borderless + a Frame Rate at or
below the display's maximum. Whether it engages can't be read back from DXGI; the display's own
refresh readout shows it. Possible refinement: in Unlocked without VSync, hold the rate a few
frames under the display's maximum so it stays inside the variable range instead of tearing above
it.

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

Upscaling (PC Settings > Upscaling, item 69, `upscaling` in settings.ini: 0 Off, 1 FSR 1;
NIER_UPSCALING=<0|1> overrides): what happens when the render resolution is below the display's.
- Off: the 2D pass's first draws sample the render-sized scene with the game's own bilinear filter.
- FSR 1 (AMD's spatial upscaler): before a draw of the 2D pass, each render-sized resolved surface
  it samples that is at least 1024x576 in guest pixels (the scene; seen: 06A52000) is enlarged to
  display size by EASU and sharpened by RCAS (`Upscaled`, `PrepareUpscaledTextures`), and
  `WriteTextureDescriptor` binds the result in its place; redone only when the surface has been
  copied to since. Constants as FsrEasuCon / FsrRcasCon; sharpness 0.2 stops
  (NIER_FSR_SHARPNESS=<stops>, 0 sharpest). Alpha is filtered plainly. The shaders are AMD's
  `ffx_a.h` and `ffx_fsr1.h` (MIT), not committed: `tools/get_fsr1.ps1` fetches them (hash-checked)
  into `tools/FidelityFX/fsr1`, `compile_ppc.ps1` copies them to `recomp/build/fsr1`, and the
  runtime compiles the two passes from those files at first use (`[fsr1] ready`); without the files
  the setting does nothing. Checked at 640x360 -> 1280x720 (Game 3): clearly crisper edges, HUD
  unaffected. The menu row itself hasn't been looked at yet.
- FSR 1 confirmed by the user ("looks good"), menu row included.
- Temporal upscalers (FSR 3.1 first, then DLSS, XeSS, FSR 4): groundwork, nothing built yet.
  A gameplay frame (frame_f8519.nfrm, 188 commands) runs: 0-14 shadow map (-> 07A7D000
  1024x1024); 17-29 a half-size pass (-> 07E7D000 640x360); 31-101 the scene; 102-106 copies
  of it (149BD000 colour, 072E2000, 06A52000); 107-131 post-processing (depth of field
  C42C4BC0 / AE4637CC, the luminance chain 512 -> 32x2, tone mapping); 135-138 more
  depth-tested draws; 144-157 bloom; 160 on the 2D pass.
  - Camera: every scene draw (71 draws, 10 vertex shaders) carries the same vertex constants
    c5-c8, the view-projection matrix by rows (x, y, z, w; z = w - 0.094 here, so depth is 0 at
    the near plane and approaches 1 far away), and c19 = the camera position. Motion vectors
    for the static world can therefore be computed from the depth buffer and this matrix of the
    current and the previous frame; moving and skinned objects would need more (their own
    previous transforms) and will ghost until then.
  - Jitter: add `jitter * position.w` to the clip position at the end of the translated vertex
    shaders, from a system constant the renderer sets only for scene draws (depth-tested, main
    target, not a shadow pass); full-screen passes stay unjittered.
  - Where to upscale: the simple place is where FSR 1 sits (the finished scene before the 2D
    pass), feeding the upscaler tone-mapped colour; the better one is after command 102, which
    means running post-processing at display size and giving the later depth-tested draws a
    display-sized depth buffer. Start with the simple place.
- FSR 3.1 built (Upscaling value 2, "FSR 3.1"; NIER_UPSCALING=2). User, at 720p down to 360p on a
  1440p window: "Looks good, shimmering and trailing with very low res but that's expected":
  - SDK: AMD FSR SDK 2.4.0 (`tools/get_fsr_sdk.ps1`: API headers and the signed
    `amd_fidelityfx_loader_dx12.dll` 39 KB and `amd_fidelityfx_upscaler_dx12.dll` 57.7 MB,
    hash-checked, into `tools/FidelityFX/sdk`, not committed; `compile_ppc.ps1` adds the include
    path and copies the DLLs beside nier.exe). The runtime loads the loader DLL by name and
    calls ffxCreateContext / ffxQuery / ffxDispatch; without it the setting falls back to FSR 1.
    The upscaler DLL reported "3.1.6" here; on Radeon cards that support it the same DLL
    provides FSR 4 (untested).
  - Jitter (`TemporalBeginFrame`): Halton(2,3), 8 x area-ratio phases, as the SDK recommends;
    passed to draws as NDC offsets in `systemExtra.yz` for depth-tested draws outside the
    shadow and 2D passes, and added at the end of the translated vertex shaders only when the
    vertex's w isn't exactly 1. That last test matters: depth of field (AE4637CC), the shadow
    mask (53D2B139) and soft particles (70D071F1) are full-screen quads with the depth test
    on, and jittering them slid their picture against the scene (doubled image, and the
    upscaler then never sharpened).
  - Camera: the perspective matrix in c5-c8 carried by the most scene draws of the frame (a
    vote; single objects carry their own combined matrix in the same registers and pass the
    same shape test, so the first one found is wrong).
  - Depth: the frame's latest full-size depth copy (`gSceneDepthBase`, here 14D55000), not
    inverted, infinite far plane; near and vertical field of view come from the matrix (0.092,
    55 degrees here).
  - Motion vectors: a full-screen pass into R16G16F from that depth and
    previous-camera x inverse(current-camera), in screen fractions (camera movement only).
  - Dispatch in `TemporalUpscaled`, at FSR 1's place: colour = the finished render-sized scene
    (flags: non-linear colour space, sRGB), output a display-sized UAV texture bound in place
    of the surface; sharpening 0.3 (NIER_FSR3_SHARPNESS). The SDK leaves its own descriptor
    heaps on the command list: ours are set again after the dispatch.
  - Result at 640x360 -> 1280x720 (Game 3): while walking, ropes, banners and railings come
    out close to a native picture; standing still the scenery is detailed and the idling
    character slightly soft (it moves without motion vectors of its own).
  - Debugging: NIER_FSR3_DEBUG=1 (the SDK's debug overlay, and a log line every 300 frames),
    NIER_FSR3_BYPASS=1 (jittered frames straight to FSR 1), NIER_JITTER_SCALE=<x>[,<y>]
    (scales the drawn jitter only; large values shake the window, so not on a visible run).
  - Per-object motion vectors for skinned geometry (`ObjectMotion`, after the camera pass, into
    the same texture; NIER_NO_OBJECT_MOTION=1 turns it off; awaiting the user's playtest).
    Skinning vertex shaders read their bone matrices from a texture (t16, one row) and the
    shared camera from c5-c8, so the bones are in world space. `ExecuteDraw` notes each opaque
    scene draw whose vertex shader declares textures (`gSkinnedDraws`, with the arena blocks
    it used); at the end of the frame its constants and those textures are kept
    (`RememberSkinnedDraws`), keyed by shaders, vertex/index counts and order among equals. The
    next frame, each paired draw is repeated with a second form of its vertex shader
    (`PrepareVariant`: the translated main body twice, the copy reading `cP[]` and `tPN` in
    slot (N+16)%32, i.e. last frame's inputs) and a pixel shader that writes the difference of
    the two clip positions where the fragment is the visible surface (depth copy in slot 31,
    2% of 1 - depth allowed). Last frame's texture contents live in their own host textures
    (`gDecodedBefore`). Game 3: 7 of 7 skinned draws paired; at 360p -> 720p the running
    character keeps its shape where the camera-only vectors left it speckled and dissolving.
    Limits: the second shader form compiles on the render thread the first time a skinning
    shader is seen (a hitch); two identical enemies that swap draw order get each other's
    history for a frame; rigid moving objects (their own matrix in constants) and particles
    still have camera-only vectors.
  - Not done: motion vectors for rigid moving objects and particles; upscaling before the game's
    post-processing; use at native resolution (anti-aliasing only); the half-size and particle
    passes are jittered by the same rule but weren't checked separately. The vertex shader
    change gives every vertex shader new HLSL, so the DXBC cache refills once.

Pre-compilation, second look (after 581 shaders captured in the playthrough so far):
- The archives and the executable hold standard shader containers (`tools/shader_containers.py`):
  magic 102A1100 pixel / 102A1101 vertex, then virtual size, physical size, 0, constant table
  offset, definition table offset, shader record offset; the record starts with the microcode's
  offset inside the physical part (0x40: after the float literals) and its size. 557 distinct
  pixel and 331 distinct vertex shaders are stored that way (about 1,800 copies of each kind).
  But of the 331 pixel shaders captured only 32 are among them byte for byte and 23 with a few
  words changed; of 250 vertex shaders none identically (19 with a few words changed). The rest
  are the headerless physical parts described above (their headers are elsewhere) or generated.
- What a first-run hitch is made of (Game 1 from boot with NIER_SHADER_SALT=1, 130 pipelines):
  the driver's CreateGraphicsPipelineState takes 2,553 ms in total (59 over 5 ms, worst 106 ms)
  and the render thread waits 881 ms for pipelines in 20 waits; it never waits for an FXC compile
  directly. A pipeline needs the patched vertex shader, the pixel shader and the render state,
  none of which the files give by themselves, so compiling stored pixel shaders ahead would
  remove only the smaller, parallel part of each hitch.
- What would remove them: a pipeline list recorded while playing (vertex shader, pixel shader,
  render state), replayed on a background thread at boot. For the player who recorded it that
  only guards against the driver's cache being reset. For other players the list would have to
  say where each shader's microcode is in their own copy of the game (file, offset, length,
  hash) plus the few vertex fetch words D3D patches in; shaders stored nowhere (about a sixth
  in the first sample) would still compile at first use. The user approved shipping it.
- Built (pipeline list):
  - Recording: `RendererPreparePipeline` appends every new combination the game uses to
    `recomp/pipelines_seen.txt` (kept across runs, test runs included; not committed):
    `P <vs hash> <ps hash> <blend> <write mask> <depth control> <stencil masks> <flags>`
    (flags: 1 rect list, 2 colour, 4 depth bound, 8/16 cull, 32 counter-clockwise front).
  - `tools/pipeline_list.cpp` (build: `clang++ -O2 -std=c++20 tools\pipeline_list.cpp -o
    toolsuild\pipeline_list.exe` in the dev shell; run: `pipeline_list recomp\pipelines_seen.txt
    recomp\shaders recomp\config\pipeline_list.txt Nier recomp\captures\image.bin
    media\layer1.cpk media\layer2.cpk`, about 10 s) looks for each used shader's microcode
    (`recomp/shaders/*.bin`) in the image and the archives, allowing up to a third of the words
    to differ, and writes `recomp/config/pipeline_list.txt` (committed): `S <vs|ps> <hash>
    <source> <offset> <dwords> [index:value,...]` (source `image`: offset is the guest address;
    the index:value pairs are the words to patch in) and the `P` lines whose shaders were found.
  - `recomp/runtime/prewarm.cpp` (`PrewarmStart`, after LoadGame): a thread waits for the
    renderer, reads each listed shader from the player's files, patches it, checks the hash,
    translates it and queues the compile and the pipelines (`RendererPrewarmShader/Pipeline`).
    Those jobs go to the back of the worker queues; whatever the game asks for goes to the front
    and pulls its own queued job forward. NIER_NO_PREWARM=1 turns it off. Log: `[prewarm]`.
  - First result (boot to The Aerie, 131 pipelines, 94 shaders): 65 shaders placed (vertex: 1
    as captured, 30 patched, 8 not found; pixel: 29 as captured, 5 patched, 21 not found; 412
    patched words in all), 84 pipelines made ahead. With the DXBC cache emptied (the driver's
    cache was warm, so this measures only the FXC part): render thread waits for pipelines
    545 ms in 15 waits without the list, 207 ms in 9 with it. A run with the driver's cache
    cold as well hasn't been measured since (NIER_SHADER_SALT gives only one other variant).
  - The scene and character material family (`8263A851`, `D461B875`, ...) was not placed at first: those
    shaders are inside "lzo" containers. Most model files in the archives (*.MDP; found by the
    magic at any 0x800 boundary, 333 in the two archives) are stored that way: `lzo\0`, slot size
    at +8 (0x20000), block count at +12, unpacked size at +16; block b starts at container + slot
    * b (block 0: 0x20 further on) with its unpacked offset, unpacked size and packed size
    (big-endian), then LZO1X data, or the bytes as they are when the two sizes are equal
    (`recomp/runtime/lzo1x.h`, our own decoder). The *.MDV beside each is CRILAYLA-compressed by
    the archive itself and holds no microcode. `tools/pipeline_list` unpacks every container and
    searches it; a shader found there gets a `Z` line (container offset in the archive, offset in
    the unpacked data) and prewarm.cpp unpacks the block it is in. Result: 559 of 559 recorded
    pipelines and all 451 shaders placed (before: 199 of 353), the tool takes about 11 s.
  - The user's first play of the opening with the list at 353 recorded 206 more combinations:
    the opening had not been recorded on this machine, and it stuttered from compiles.
  - The game's start waits for the list (`PrewarmWait`, prewarm.cpp, called after `PrewarmStart`):
    before, the list was built in the background and a fresh machine reached the opening first.
    While it waits extra shader and pipeline workers run (`RendererPrewarmBoost`: nothing else
    needs the processor), and after 0.7 s a "Preparing shaders" picture with a bar is shown
    (`DrawProgressScreen`, osk.cpp, as the overlay over empty frames). Here: 0.4 s with warm
    caches (nothing shown), 1.7 s with the game's shader cache emptied and the driver's partly
    warm. `NIER_NO_PREWARM_WAIT=1` starts at once as before; `NIER_PREWARM_SHOW=<ms>` keeps the
    picture up that long to look at it. It gives up if no pipeline finishes for 30 s.
  - Shaders nobody has drawn yet: `tools/pipeline_list` also lists every pixel shader stored in the
    archives' model and effect files (`EnumerateStored`; 612 beyond the 254 recorded, so about 866
    in all). A file pair X.MDV / X.MDP (models; X.EFV / X.EFP effects) holds the shader headers in
    the V file (CRILAYLA-compressed by the archive for models, plain for effects; found by the
    magic 102A1100 / 102A1101) and the code in the P file. The code parts of a group of headers
    follow one another in header order, each rounded up to 32 bytes; a model has one group at the
    end of its unpacked data starting on a 4 KB boundary, an effect file several groups. Each
    header's code is looked for right after the one before, and when that does not read as
    microcode (`PlausibleMicrocode`: control flow first, only exec/alloc kinds, blocks start right
    after it and reach the end) the file is searched. All 5,291 pixel shader headers are placed
    that way, 264 of the recorded pixel shaders come out among them, and all 612 new ones
    translate. The archive's file table (`CPK `/`TOC ` @UTF tables) gives the pairs.
  - The game compiles those once: shader lines no P line uses are skipped when
    `recomp/shadercache/prepared_<hash of the list>.txt` exists, and compiled (behind the
    "Preparing shaders" picture) when it does not; `NIER_PREWARM_ALL=1` forces it. With the
    shader cache emptied: 1,063 shaders and 559 pipelines in 3.5 s here, 0.3 s on the next start.
    What this saves when an unrecorded place is first drawn is the pixel shader's compile; the
    vertex shader (patched per vertex layout, so unknown ahead) and the driver's pipeline build
    still happen then. Not measured in play.
  - An earlier count of "2,285 pixel / 1,030 vertex shaders stored" (tools/shader_containers.py over
    the unpacked data) was wrong: in a V file the code does not follow the header, so the script
    read whatever came next as code and counted it.
  - The list covers only what has been recorded since this was built; it grows as the user
    plays (regenerate and commit now and then).

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

On-screen keyboard (`recomp/runtime/osk.cpp`), for XamShowKeyboardUI: the game asks for the hero's
name with it (New Game > difficulty > keyboard > "Set your name as ...?", whose cursor starts on No;
answering No shows "Please enter another name." and the keyboard again). Title "Enter a name",
description "Please enter your name.", 10 characters at most. It is drawn with GDI+ into a 1920x1080
image whenever something changes and shown by the renderer as an alpha-blended overlay over the
scanout (`RendererSetOverlay`, `DrawOverlay`; the game keeps presenting frames meanwhile).
Controller: d-pad / left stick move (with repeat), A types, X deletes, Y space, LB shift, Start
accepts, B cancels; keyboard: WM_CHAR types, Enter accepts, Esc cancels, arrows move; mouse: click a
key (`OskChar/OskKey/OskClick` from the window procedure). Shift is one-shot and on for the first
letter. Only ASCII 32-126 is typed. While it is open the game gets an idle pad and key bindings are
off (`InputSetOverlay`), and after it closes input stays held until the buttons are released, or the
closing press would answer the game's next question. The hint line follows the last device used.
Two things the old Windows dialog path also got wrong: the request must be marked pending in the
overlapped block (first word and +0x18 = 997) before the thread starts, or the game takes it as
done at once with no name; and `run_boot.ps1` always preset NIER_PLAYER_NAME, so a playtest never
showed any keyboard. Now a playtest, or `-AskName`, shows it; other test runs still preset "Nier".
NIER_KEYBOARD_DIALOG=1 uses the old dialog. Test route (with saves present the title cursor starts
on Continue): Start every 60 frames to 600, `700:1` (Up), `800:1000` (New Game), `1150:1000`
(Normal), `1400:1000`; the keyboard is up by frame 1550. Checked by script: typing, moving, Shift,
accepting ("Set your name as Qw?" follows). The user tried it in a playtest ("all good"; the log
shows a name returned and a cancel).

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
- Blocky shadows: see "Shadow quality" (a setting; the user is happy with the result).
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
  CLOSED by the user ("we can ignore the shadows, they are pretty good now"). Never reproduced. Standing still (Game 3) the shadow mask is identical frame to frame (955 +/- 1
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
  Done since (awaiting the user's playtest): (1) command thread: only data textures (one row
  or a float format) are hashed on every use, the rest once a frame; plain triangle lists are
  taken as read and strips written through a pointer; vertex and index data are copied and
  byte-swapped in one pass into a buffer that isn't zero-filled (`ByteBuffer`). Its work per
  frame went from 10.6 to about 7.3 ms (OnDraw 56% -> 37% of the thread), late guest frames
  under the profiler from 4 of 604 to 0 of 609. The main thread's sub_82579D50 is the D3D
  library's spin wait for the GPU, not work. (2) Presentation: the user's display is 240 Hz,
  and a 60 fps frame presented at the next refresh stayed up 3, 4 or 5 refreshes depending on
  when it was ready. On a display at 120, 180 or 240 Hz each game frame is now presented with
  that sync interval (2-4), or 1 when frames are queued so no delay builds up; the display's
  rate is re-read every 120 frames (`[present] display at ...` in the log;
  NIER_PRESENT_INTERVAL=<n> forces the count, 1 turns it off). Nothing can be done this way
  for 144 or 165 Hz (60 doesn't divide them). (3) The guest vblank is 60 Hz exactly instead of
  59.94, so the game doesn't fall a frame behind the display every 17 seconds. Checked at
  120 Hz on monitor 2: 609 of 609 frames on time. IDXGISwapChain::GetFrameStatistics gave no
  usable on-screen durations in the windowed flip model, so evenness is judged by eye.
  Test runs are noisy when the user is using the machine (one run showed 40 late frames).
  User after (1)-(3): "frame pacing is much better, there are still some tiny stutters on the
  aerie". (4) Two free-running 60 Hz clocks (the vblank thread's timer and the display) still
  drift, and each crossing shows one frame too long or too briefly. The vblank thread now waits
  on the display itself: `RendererWaitDisplayVblank` (IDXGIOutput::WaitForVBlank on the swap
  chain's containing output, once per refresh in the 60 Hz period) when vsync is on and the
  display's rate is 60, 120, 180 or 240 Hz; otherwise, or if the wait returns at once or fails,
  the timer as before (NIER_NO_VBLANK_LOCK=1 forces the timer). Log: `[present] the game's
  vblank follows the display`. Test run at 120 Hz with the user's 4K playtest running beside
  it: 604 of 607 frames on time. Awaiting the user's verdict. The user plays at 3840x2160
  internal with shadow maps x3, so GPU time is the next suspect if stutters remain (F9).
- Stutter in the cutscene after leaving the library, and "the light does not come in smoothly"
  when the home appears after the prologue (user, 60 Hz display, recordings seq_f56943 = home,
  seq_f63744 / seq_f64736 = the cutscene). OPEN, two changes awaiting the user's playtest.
  What the recordings show: (a) the cutscene has about 20 frames of 33.3 ms in 10 s with no new
  shaders, pipelines or textures, render thread 11-13 ms and GPU 6 ms: frames the game itself
  was late with, not the renderer. (b) In the home the game's exposure value (the clear colour
  it resolves to 0767A000: pixel constant c256 of the D0C7E9595FC160B9 draw into the 80-wide
  target) is run down to a floor of 0.042 during the fade to black, stays there for 2 s while
  the fade lifts (only the candle flames show), then climbs x3.6 a frame for six frames to 114
  and falls back to 6 over twenty: a white flash. Found on the way (NIER_LOG_WRITEBACK=1): on a
  display at the game's own rate the frame queue, once full after a load, never drained (the
  "show queued frames for one refresh" rule only helped at 120-240 Hz), so the game ran 4-5
  frames ahead of the picture: input lag, exposure set from a measurement that old, and the
  game paced by the renderer's pops instead of the vblank (in a test run every frame took 33 ms
  for a while after the load). Changed: (1) a frame presented while others wait, at interval 1,
  is presented at interval 0 (NIER_KEEP_QUEUE=1: as before); the game is now 1 frame ahead in
  561 of 586 frames. (2) Read-back resolves are written to guest memory before Present instead
  of after (NIER_LATE_WRITEBACK=1: as before). NOT shown to fix the flash: loading Game 3, the
  exposure ramp (0.547 x1.25 a frame to 1.66, then back to 0.91) is identical to three digits
  with the game 1 or 5 frames ahead, so that overshoot is the game's own behaviour, and the
  home's may be too (not reproducible without playing the prologue again). Test runs in a
  window on the 60 Hz display still show 59.3-59.6 fps in Game 3 with old and new alike (3-4
  late frames in 10 s; the playtest's light scenes logged 60.0): not looked into.
  User, after playing the new build and checking reference footage of the console version: "it
  look to be correct", so the light's behaviour is taken to be the game's own. Which of the two
  that covers, and whether the cutscene stutter is gone, is not yet confirmed.
  `NIER_REPLAY_CONSTANTS=<ps hash>,<first>,<count>[,<target width>][;...]` lists those
  constants for every frame of a recording in replay.log.
- Pipelines not ready in time no longer hold the frame for scene geometry (awaiting the user's
  playtest). Pipelines were already built on workers, started by the GPU emulation a frame or
  more ahead; the render thread waited when one was still not there (PipelineFor). Now a draw
  that tests against a depth buffer, is not a rectangle list and has more than 6 vertices is
  left out until its pipeline is ready (MayDeferDraw): the object appears late, the frame is on
  time. Everything else still waits: full-screen and 2D quads (3-6 vertices), whose absence
  would make the whole picture wrong. Frame replays wait for everything
  (RendererSetSynchronousPipelines). NIER_SYNC_PIPELINES=1: as before. Log: `[pipeline] vs ..
  ps ..: ready after N ms, M draws left out meanwhile`, and for every wait over 5 ms that
  remains `[pipeline] ..: the frame waited N ms (state, vertices, target)`, so a playtest log
  now says which slow frames were pipelines. Also: a job somebody waits for goes to the front
  of its queue (urgent jobs are taken newest first, and in a burst the awaited one lay under
  the rest: seen as a 2.3 s wait with NIER_PIPELINE_DELAY). Tests, loading Game 3 with
  NIER_NO_PREWARM=1: with NIER_PIPELINE_DELAY=150 (every pipeline 150 ms slower) 13 pipelines
  and 496 draws left out, picture whole once they were in (frames 1500/1800/2400 looked at);
  the level's first frame still took 2.4 s there because its ten or so post-processing quads
  each waited their 150 ms. With a cold shader cache and NIER_SHADER_SALT=1 (real compile
  times): waiting as before 4 waits, 304 ms, slowest frame 226 ms; leaving out 7 waits (all
  quads), 206 ms, slowest 187 ms, 10 pipelines and 125 draws left out for at most 172 ms. So
  the gain without the pipeline list is modest; with the list, post-processing is built at
  start-up and what is left to meet in play is scene geometry, which this covers. Risks, not
  seen: something drawn once into a surface that is kept (it would stay missing); a quad with
  more than 6 vertices that a later pass depends on. Not covered: the motion-vector form of a
  vertex shader (FSR 3 only) is still compiled and its pipeline built on the render thread.
- The stutter in the cutscene after leaving the library, looked at again with the recordings
  (seq_f63744, seq_f64736; seq_f57737 from the same morning is as heavy). CAUSE FOUND, not
  fixed. All thirteen recordings side by side: the ones made at home on 14-15 November have 0-2
  frames of 33 ms in 600; the three heavy cutscene recordings of 16 November have 4, 7 and 19,
  the light home scene of the same session 1. In seq_f64736 the late frames come in runs with
  a fixed spacing (65149, 65153, 65157, 65161: every fourth frame, right after a camera cut
  that takes the frame's data from 733 to 831 KB), which is what a stage needing a little more
  than one refresh per frame looks like, not the virtual display's scattered late vblanks.
  Replayed at the user's settings (resolution_scale 8 = 3840x2160 internal, shadow_quality 3 =
  shadow maps x4, `NIER_SETTINGS_FILE` pointed at a copy of recomp\build\settings.ini) with
  NIER_LOG_PACING=1, which now also gives the wait for the GPU after Present: recording the
  frame takes 4.7-5.8 ms and the GPU 8-11.7 ms (average 10.5, up to 15.5), and the render
  thread does the two one after the other, so a frame costs it 15.6 ms on average and more
  than 16.7 ms in 131 of 576 frames; from frame 65141 on the average is 17 ms. The other
  cutscene recordings: 12.8 ms (27 frames over) and 14.8 ms (73 over); the home scene 6.4 ms
  (2 over). Shadow maps at x1 take 1.5 ms off, not enough. Playing over Parsec adds the
  stream's own use of the GPU on top (not measured). So the renderer is the limit there: the
  render thread waits for the GPU to finish a frame (WaitForGpu after Present, and before it
  for the read-backs) before it starts recording the next, where the two could overlap (the
  limit would be the larger of the two, 11 ms, instead of their sum). That needs per-frame
  resources in two sets (command allocator, upload arena, retired list, read-back buffers) and
  the read-backs delivered a frame later. The replay's own time between presents (33 ms at any
  resolution, even 640x360) is the cost of reading the recording, and says nothing.
  FIXED another way (awaiting the user's playtest): the frame goes to the GPU in pieces while
  the rest of it is still being recorded (ExecuteGuestCommands: every 0.25 ms of recording the
  command list is closed, executed and reset; `NIER_SUBMIT_MS=<ms>`, 0 = the whole frame at
  once as before). Every draw sets all of its own state, so a list can end between any two
  commands and only the descriptor heaps are set again; one set of per-frame resources stays
  enough, because the frame is still finished before the next begins. Replayed at the user's
  settings, ms a frame (recording + waiting for the GPU), whole then in pieces: seq_f64736
  14.8 -> 9.4 (frames over 16.7 ms: 70 -> 5 of 576), seq_f63744 11.9 -> 8.5 (16 -> 1),
  seq_f57737 13.3 -> 9.1 (36 -> 2), the home scene 6.2 -> 5.1. Recording itself gets 0.3-0.9
  ms longer (about twenty submissions a frame). Pictures: ten frames of seq_f64736 rendered
  both ways are byte for byte the same. Live: loading Game 3, 60 fps, no crash. Not tried:
  the cutscene itself in play, and a GPU other than this one.
- The display-locked vblank ran the game too fast on a monitor whose rate Windows misreports
  (found in a test window on the user's second monitor at home: reported 120 Hz, a vblank every
  6.94 ms = 144 Hz, so "every 2 refreshes" made 72 frames a second; the same with the old
  submission, so not from the change above). FIXED: RendererWaitDisplayVblank compares the
  middle one of every 31 waits with the period expected and, more than 4% off, leaves the
  vblank to the timer for as long as the display's mode and the divider stay the same. Log:
  `[present] the display reported as 120 Hz gives a vblank every 6.94 ms where 8.33 was
  expected`. After it: 60.0 fps there. The middle value, so that a virtual display's
  occasional missed refresh does not count.
  Playtest at home after it (50,400 frames, primary 3840x2160 at 240 Hz, fullscreen). With
  VSync off (the setting left from the Parsec session) frames 1-42,000 logged 60.0 in every
  window but two. At frame 42,000 the user turned VSync on: "each frame is held for 4
  refreshes", the vblank followed the display, and the game ran at 58.5; then the guard
  reported "a vblank every 4.59 ms where 4.17 was expected" (four waits took 18.4 ms), went to
  the timer, and it got worse: 51, 39 and 46 fps for three windows, 857 frames skipped from
  view, the user's recording seq_f43609 (386 frames in 10 s, a 100 ms stall about every tenth
  frame, a light scene, no renderer waits). Measured on its own afterwards (a small program
  timing 480 WaitForVBlank on each output, no game running) that monitor ticks at 240.1 Hz and
  the second one at 144.0 (reported 120). So in the fullscreen game with VSync on the
  refreshes did not come at 240 Hz: most likely a variable refresh rate, where the display's
  refreshes follow the game's presents and are no clock (the user has since confirmed that
  G-SYNC is on for that monitor). Two things were wrong: the display-locked vblank on such a
  display (the guard caught that), and what the guard fell back to, the timer with frames
  still held for 4 refreshes each. Changed: the guard now needs three readings in a row more
  than 4% off, and once it has tripped frames are presented at the next refresh
  (gDisplayTicksOff) instead of being held for a count of them. Tested only on the second
  monitor (60.0 fps, the guard trips, no skips after the load); NOT tested on the primary with
  VSync on, which is where it went wrong. The other recording of that session, seq_f48136
  (a heavy scene, 752 KB of data a frame, VSync off again), has 591 of 596 frames at 16.7 ms,
  and the log there shows the pieces at work: render thread 7.7-7.9 ms a frame (recording
  3.8-5.1, waiting for the GPU 2.6-3.7).
  The playtest after the change did not try it: the game started at 60 with VSync on for the
  first 900 frames (menus), then the user set Frame Rate to uncapped and played 87,600 frames
  that way, most windows at 240 fps, 100-190 in heavy places, no crash, no pipeline waits, no
  F9. In the heavy places the render thread takes 7.4 ms (recording 5.4, GPU 1.8) while the
  game delivers a frame every 10 ms, so there the limit is the game or the GPU emulation
  thread, not the renderer. (The "skipped from view" count read 600 of 600 in that mode: every
  uncapped frame is presented at once and none is lost; it now counts only at a locked rate.)
  So 60 fps with VSync on, on the G-SYNC monitor, is still NOT verified.
  CORRECTION after the next playtest (60 fps, VSync on, 240 Hz primary: 58.6, 59, 53.9, then
  40 fps for three windows with 442-444 of 600 frames "skipped from view", recording
  seq_f8550, and the guard silent). The user: "might need to revert back to the previous vsync
  behavior". They were right, and the variable refresh rate theory above is NOT established:
  the cause in plain sight is the queue change committed that morning (1ca514c). It was meant
  for a display at the game's own rate, but its test was `waiting && interval == 1`, and on a
  faster display a queued frame has interval 1 precisely because it is queued (frames not
  queued get the multiple, 4 at 240 Hz). So on 240 Hz every queued frame was presented at
  interval 0 and never seen, where before it was shown for one refresh: the catch-up that had
  always worked there. The recordings made at home on 14-15 November, before any of this, have
  0-2 late frames in 600 with VSync on. Changed: frames are dropped only when the multiple is
  1; on faster displays the behaviour is the previous one again. The guard is now only for a
  display that steadily runs at another rate than reported (refreshes counted against the
  real clock a second at a time; two seconds in a row more than 2% off and equal to each other
  within 0.5%): it trips on the second monitor (`reported as 120 Hz refreshes 144.0 times a
  second`, then 60.0 fps and 0 skipped) and leaves a display whose count wanders alone.
  Whether the earlier "4.59 ms" reading on the primary was the display or a product of the
  dropped frames is not known. Awaiting a playtest at 60 fps with VSync on.
  SETTLED by testing on the primary monitor itself (the user: "I think you should test on the
  primary monitor since that's what we are checking"; scratch script primary_test.ps1: muted,
  fullscreen, a copy of the user's settings, the copied Game 3 save; note NIER_MONITOR=primary
  means the SECOND monitor, atoi gives 0: leave the variable unset for the primary). Three
  things were wrong, all of them real:
  (1) the queue change dropping frames on displays faster than the game (above; fixed);
  (2) the G-SYNC display's refreshes are no clock after all: with VSync on, frames held for 4
  refreshes and the vblank following the display, the display was counted at 85.6-88.5
  refreshes a second (155.6 in the user's playtest), four waits took 46 ms, and the game ran at
  35 and then 21 fps. The game's clock followed the display, which refreshes when a frame is
  presented, which happens when the game's clock ticks: nothing in the circle keeps time;
  (3) the guard said "use the timer" only AFTER waiting for the display's four refreshes, so
  the fallback never ran on the timer either (frames 46 ms apart with interval 1).
  Now: a refused display is not waited for at all; the count is taken a quarter of a second at
  a time and two in a row more than 5% off refuse it (so within about half a second of the
  first game frames); then the timer and Present at the next refresh. Result on the primary,
  60 fps, VSync on, 3840x2160 internal: 60.0 fps in every gameplay window, 0 frames skipped,
  and with NIER_LOG_PACING 1608 of 1611 gameplay frames 15.5-17.9 ms after the one before, none
  over 25 ms, no late vblanks. The second monitor (reported 120, really 144) is refused the
  same way and runs at 60.0. A Parsec display's 1-3% of missed refreshes stays under the 5%.
  Not checked: a fixed-refresh 120 or 240 Hz display, where the lock should stay on (none
  here to test with).
  The user's playtest after it (7,200 frames, 60 fps, VSync on): "fix worked but now the
  cutscene reverted back to stuttering", "even though it reports 60fps". The log agrees with
  the second half: the cutscene's windows (frames 5,400-7,200, recording 2.5-4.8 ms) read
  59.8-60.0 fps, slowest frame 8.8-18.2 ms, nothing skipped, nothing queued. So the frames are
  all made and none is late by a refresh; what is uneven must be WHEN each is shown, which the
  log does not record without NIER_LOG_PACING. Suspected, not measured: with the timer and
  Present at the next refresh on a G-SYNC display a frame appears when it is finished, and in
  that cutscene the time to finish one varies by several ms from frame to frame (recording 1
  to 5 ms, GPU 3 to 6), where holding each frame for a count of refreshes used to even that
  out. OPEN: needs a playtest of the cutscene with the trace on.
  Traced playtest (7,909 frames, 60 fps, VSync on throughout): that suspicion was WRONG. In
  the cutscene (frames 4,848-7,248) 582-590 of every 600 frames come within 1 ms of 16.7 ms
  after the one before; what stands out is 12 frames at 33 ms, each alone (5694, 5730, 5764,
  5782, 5913, 6470, 6500, 6530, 6834, 7014, 7087, 7142; plus two at the scene's start with new
  shaders). For each of them the vblank came on time (no late vblank line), nothing was queued,
  and the render thread's own work was as usual (recording 5 ms, GPU 2.5 ms): the frame simply
  reached the renderer one tick late. So it is the game's side, the game thread or the GPU
  emulation's command thread, about once in 200 frames; the rate is about what seq_f48136
  showed with VSync off (5 in 596), so "reverted" may be the same thing noticed again rather
  than something the VSync fix brought back (not established). New for the next pass:
  NIER_LOG_PACING also logs, for every frame over 1.5 vblanks and every 100th other one,
  `[pace] command thread, frame N (late|on time): T ms = W waiting (WAIT_REG_MEM) + I idle (no
  commands from the game) + K working`. In Game 3's gameplay that reads 16.7 = 11-12 waiting +
  0 idle + 4.2-5.6 working: the command thread is never short of commands there and has 11 ms
  to spare. The cutscene sends about four times the data a frame (750-830 KB against 100-200),
  so its working time may be near the whole 16.7 ms; that is the thing to measure.
  Measured (second traced playtest): in the cutscene the command thread works 9.2-9.9 ms a
  frame (most 12.7) and waits 7 in WAIT_REG_MEM, idle 0: not near the limit on a normal frame.
  The late frames are that thread working 17-29 ms. Two kinds. (a) With a dozen new shaders
  logged in the frame before (scene starts; 61 ms at frame 2329): ShaderFor and StoreShader
  wrote each new shader's dumps (recomp\shaders\*.bin and *.hlsl) to disk there and then. (b)
  With nothing new (6024, 6973, 7098; in the first traced session 5694/5730/5764/5782 and then
  6470/6500/6530, the same group about 770 frames later): the periodic full hash of block
  compressed textures in DecodeTexture ("every ten seconds or so regardless"). Its stagger was
  `600 + (key & 255)`, but the key's low bits are the same for nearly every texture, so all the
  textures first seen in one frame, a scene's worth (88 MB in the first frame of seq_f64736),
  came due in the same frame again and again. FIXED, (b) awaiting the user's pass through the
  cutscene: (a) the dumps go to a writer thread (WriteDumpLater); (b) the stagger is a hash of
  the key (600-1111 frames) and at most 2 MB of due textures are hashed in a frame, the rest
  staying due (one larger than that still gets its turn). Loading Game 3 on the primary, the
  command thread's work in the late frames around the load went from 363 ms in all (106, 60,
  six of 19-25, 64) to 236 (52, 57, 36, three of 18-22, 31); what is left there is not looked
  into (texture decoding is the likely part), and it falls on the loading screen.
  Third traced playtest, the cutscene watched twice: NOT fixed. 7 late frames the first time
  (5536, 5595, 6219, 6398, 6460, 6792, 6857), 3 the second (13463, 14238, 14825), none with
  new shaders, each "33.3 ms = 16 waiting + 17-19.5 working" against a normal 7 + 9.5: the
  work ran past the tick by a little and the thread then waited out the next one. So the
  periodic hash was not it, or not all of it. The trace now also gives, per late frame,
  textures converted (count, KB, ms) and KB hashed in full with its ms. At the Game 3 load:
  83 textures, 42 MB converted in 29.6 ms (1.4 GB/s: the per-block TiledOffset + CopySwapped
  loop), 67 textures, 25 MB in 13.3 ms; hashing is cheap (44 MB in 1.6 ms) but 31 MB is hashed
  in full EVERY frame in gameplay there (textures in plain formats over 64 KB: not looked into
  which). Frames 1379-1382 of that load are 18-57 ms of work with nothing converted, so there
  is a third cost not yet named. The sampling profiler (NIER_PROFILE=<frame>,<seconds>) on
  Game 3's gameplay: command thread busy 37%, of the process's samples CopySwapped 8.6%
  (vertex data in BuildDraw), XXH3 5.4%, BuildDraw itself 3.9%, ReadIndices 2.3%; and one guest
  thread is busy 100% of the time in sub_82579D50 / sub_82573758 (a wait loop of the game's).
  Next: a pass through the cutscene with both the trace and the profiler on.
  Fourth traced playtest (the profiler's window, frames 5000-7100, missed the cutscene, which
  came at 6833-9400 this time): 6 late frames in it, one with 22 textures converted (7.6 MB,
  4.7 ms), the other five with nothing converted and 1 ms of hashing, each again "16 waiting +
  16.7-17.6 working". That narrow range, always just past one tick, says the thread is not
  doing 8 ms more work but is HELD until about the next tick somewhere inside what is counted
  as work. The trace now times every kind of packet (`[pace]   packets: 0x64 x1 30.4 ms, ...`
  after a late frame's line; 0x22 draws, 0x27 shader loads, 0x64 the swap) and the hand-over
  of a finished frame (`[pace] frame N: handing it to the renderer took X ms (L for the lock,
  R for room: Q frames were queued)`). At the Game 3 load the late frames are of two kinds:
  draws (52 ms with 42 MB of textures converted) and the swap packet, 14-58 ms, which is
  RendererSubmitFrame waiting for room in the frame queue (3 queued) while the render thread
  is behind with the scene's uploads. Whether the cutscene's late frames are that same wait
  is what the next pass shows.
  Fifth traced playtest: it is NOT the hand-over. The cutscene's nine late frames (7429, 7496,
  7630, 8064, 8169, 8229, 8495, 8561, and 6540 with 22 textures converted) are all draws:
  860-965 draw packets taking 15.1-17.4 ms where the scene's usual frame spends about 9 on
  them; nothing converted, 1 ms hashed, shader loads 0.3 ms, the swap 0.0, no hand-over wait
  anywhere in the scene. So the command thread's draw handling (BuildDraw) is what runs
  long, by 6-8 ms, a few times a minute; why those frames and not their neighbours is not
  known (the "held until the tick" reading above was wrong: the 16.7-17.6 range is just where
  a frame becomes late). Two ideas tried on Game 3's gameplay and TAKEN OUT again, since
  neither changed the thread's 3.9 ms a frame: sharing one vertex copy between a mesh's draws
  in a frame (only 1.1 of 11.5 MB a frame were repeats there; verified correct with a copy
  and compare, 0 mismatches) and cutting the vertex bytes from recycled blocks instead of a
  fresh allocation per draw. What the trace says about that thread in Game 3's gameplay: 375
  draws in 3.4 ms (9 microseconds each, the longest 0.5 ms), of which 0.9 ms is hashing 28.7
  MB of textures in full every frame, and 11.5 MB of vertex data copied. The packets line now
  gives the longest single packet of each kind and is also written for every 100th on-time
  frame, so the next pass through the cutscene shows whether a late frame is one draw taking
  7 ms or all of them taking longer, and what an ordinary frame there looks like.
  OPEN. Still to try: finding what the 28.7 MB hashed every frame is (a fifth of the thread's
  work in gameplay), and the sampling profiler over the cutscene itself.
- Vertex shaders nobody has drawn yet are on the pipeline list too (awaiting the user's
  playtest; nothing in play is expected to look different, only first-use builds get shorter).
  The archives' model and effect files (X.MDV/X.MDP, X.EFV/X.EFP, X.2DV/X.2DP) hold 627
  distinct vertex shaders with blank fetch instructions; play had recorded 199. What the
  console's D3D writes into them, worked out from the recorded ones (tools/pipeline_list.cpp
  VertexForms has the rule in full): the shader's header record has an input table (count at
  record+28, entries at record+40: instruction index in bits 0-11, usage 12-15, usage index
  16-19, a flag 20-23); the fetches are rewritten in the order of the mesh's vertex elements,
  each keeping its input's register; format, sign bits, swizzle, stride and offset come from
  the element's declaration type; fetches inside the 8 dwords a full fetch brought in are mini
  fetches. The meshes' declarations are in the header files (12 bytes an element: offset,
  type, usage/index/0x78; seven types in the whole game: FLOAT3 2A23B9, HEND3N 2A2190,
  FLOAT16_4 1A2360, FLOAT16_2 2C235F, DEC3N 1A2187, UBYTE4N 1A2086, UBYTE4 1A2286), but they
  are not needed: in every recorded case the layout is exactly the shader's own inputs in one
  fixed order (position, weights, indices, normal, texture coordinates, tangents, colours), and
  23 recorded shaders use layouts that are in no file. So the forms are made from the inputs
  alone, with each texture coordinate tried as two and as four half floats and each tangent
  tried present and absent: 2,396 forms, of which 2,216 are new on the list. Check: 180 of the
  182 recorded vertex shaders whose stored copy is in an archive are among the forms, bit for
  bit (the other two are stored outside the V/P file pairs). First start with the new list:
  3,281 shaders, 0 different from the list, 0 untranslated, ready after 10.4 s (once; 0.3 s
  after that); the shader cache grows to 3,284 files, 100 MB; the list from 123 KB to 624 KB.
  Not measured: how many of the new forms the game ever draws, and so how much it saves.
  Scratch tools used for the analysis are not kept in the repository.
- Crash while preparing shaders at start-up (user's playtest, once; ten test starts after it did
  not crash): C0000005 at host address 0x18 in all seven pipeline workers at the same moment,
  in CreatePipeline where it copied a shader's ComPtr<ID3DBlob> out of its future (AddRef on a
  blob whose memory had been freed; found with llvm-symbolizer --inlines on the logged
  PipelineWorker+0x547). Cause: a d3dcompiler blob's reference count is not safe to change from
  several threads at once. Measured with a small program: D3DCreateBlob, then balanced
  AddRef/Release from 8 threads, ends in heap corruption (exit C0000374) every time. The
  workers each took and dropped a reference to the shaders their pipelines share, so now and
  then the count came out short and the blob was freed. Not new with the pipeline skipping; it
  has been possible since pipelines got several workers. Fixed: the blob is held only by its
  entry in gShaderJobs.results, for the whole run, and CreatePipeline and ShaderBlob use plain
  pointers (no reference counting outside the shader worker that made it). Not provable by
  test, since it never reproduced: 16 starts and one loaded game since, none crashed.
- "Noticing some hitching" (user, playtest with a fresh shader cache, 3 minutes). OPEN. The log:
  gameplay windows at 59.1-59.4 fps with the slowest frame under 10 ms and no `[pipeline]`
  lines at all, so neither pipelines nor slow frames: about 4-5 refreshes missed in 10 s. The
  playtest before it (same queue code) logged 60.0 in gameplay. The display both times is
  reported as 60 Hz, and Win32_VideoController shows why: the active display is a "Parsec
  Virtual Display Adapter" at 1600x1200, 60 Hz (the RTX 4090's own output is listed at
  3840x2160, 240 Hz), i.e. the user is on a streamed virtual display, whose vblank is not a
  monitor's. Not yet asked whether both playtests were over Parsec. Measured in windowed test
  runs on that display with the new `NIER_LOG_PACING=1` (one line a frame: time since the last
  present, wait for the swap chain, recording, read-backs, Present, frames queued, interval):
  a frame is 5 ms waiting for the swap chain + 3 recording + 7 waiting for the GPU; in 1200
  gameplay frames 10-13 intervals of two refreshes, each one a frame where the swap chain's
  wait was 22 ms instead of 5, i.e. the frame before it missed its refresh after being
  presented on time. The count is the same with read-backs before or after Present
  (NIER_LATE_WRITEBACK) and with `NIER_QUEUE_CUSHION` 0 or 1 (frames allowed to stay queued,
  default 0); NIER_KEEP_QUEUE=1 gave 149 (33 ms frames for seconds after the load). So in
  these tests the missed refreshes come from the display side, not from our frame times. With
  the queue drained each one shows as a repeated picture and then a skipped one; whether the
  old full queue hid them better in steady play is not measured. `[present] last 600 frames:
  N shown with none queued behind them, M skipped from view` is logged every 600 frames.
  The user confirmed both playtests were over Parsec, so the display did not change between
  them. CAUSE FOUND (windowed tests): NIER_LOG_PACING=1 also logs `[pace] vblank N (frame M):
  X ms after the one before, its handler took Y ms` for a late vblank or a slow handler. With
  no frames queued every long interval is a late vblank: IDXGIOutput::WaitForVBlank on the
  Parsec virtual display returns a refresh late (33 ms) 10 to 33 times in 1200 frames, the
  handler in the game takes 0.0 ms, and the game runs exactly one frame a vblank, so it loses
  that frame. Presents with interval 1 are held a refresh too at times (swap chain wait 22 ms
  with a frame queued). With VSync off (NIER_VSYNC=0, or the setting: the vblank is then the
  timer and frames are presented at once) 0 long intervals and 0 late vblanks in 1200 gameplay
  frames, 60.0 fps in every gameplay window; the timer alone with VSync on
  (NIER_NO_VBLANK_LOCK=1) still had 12. How many refreshes that display misses seems to vary
  from session to session (60.0 in one playtest, 59.2 in the next, same code path). Not done:
  choosing this by itself on such a display (late vblanks counted, or the adapter's name): the
  user will set VSync off by hand over Parsec. Playtest after (12,000 frames): 60 fps in 18 of
  20 windows, 1 frame skipped from view, no crash; the first pipeline left out in real play
  (`vs E57CD156111D2B4C ps B12CB4FE50555DD0: ready after 47 ms, 15 draws left out`). User:
  "The game was smooth overall, some of the hitching might just be latency due to Parsec" (they
  stream from home to their workplace, 13 miles away), so what is left to see there cannot be
  told apart from the stream; smoothness is best judged on the real monitor.
- FIXED (confirmed by the user in play): the Aerie boss's red orbs
  showed their whole quad as a square. Not the resolution work (it shows at 1x with the 2D pass
  off): the renderer had no alpha test. The orbs are 19 quads drawn with the 2D shader (vs
  E51E77673993E73F, ps A87A2B7182F92D9D, an RGBA8 2048x128 sprite strip, blend 000E0706: colour
  by source alpha, alpha by the CONSTANT alpha factor); outside the orb their alpha is almost
  zero, so the colour is untouched but the target's alpha is rewritten over the whole quad, and
  the passes after it that read that alpha show the quad. The console drops those pixels with
  the alpha test. Now: `RendererDraw::alphaFunc` / `alphaRef` from RB_COLORCONTROL (0x2202:
  function bits 0-2, enable bit 3) and RB_ALPHA_REF (0x210E, float), handed to pixel shaders
  in the System constants and applied at the end of every translated pixel shader (so every
  pixel shader recompiles once). The only test seen so far: function 6 (greater or equal),
  reference 2/255. Also new: the blend constant (RB_BLEND_RED..ALPHA, 0x2105-0x2108) is passed
  to the card (`OMSetBlendFactor`); it was left at zero. Captures are now NFRM version 3 /
  NSEQ version 2 (both fields); `replay_frame.ps1 -Alpha '<ps hash>,<function>,<reference>'`
  and `-Blend 'r,g,b,a'` force them on older captures, `-Only <n>` takes one frame of a
  recording as a single capture.
- Depth of field "a bit low res" (user, cutscene with Kaine; recording seq_f17594). The game's
  depth of field is a mip blur: C42C4BC07882DC33 copies the picture to 512x512 (07184000) with
  one sample a texel, C2DA0F3502D52B51 builds each smaller level from the one before (four
  taps; the same chain feeds the exposure read-back), and AE4637CCAACCEBD4 picks a level from
  the depth (log2 of the circle of confusion) and blends it with the sharp picture. Two built-in
  shader improvements (xenos_shader.cpp, by shader hash; NIER_ORIGINAL_EFFECTS=1 leaves them
  out): the first copy averages 4x4 samples over each texel's footprint, and the chain is read
  with a bicubic B-spline filter at the two nearest levels. In the replay the blurred figure's
  edges lose their stair steps and the mean brightness is unchanged (103.1 before and after).
  Awaiting the user's check. Fine texture detail still shows through weakly blurred areas; that
  is the sharp picture in the blend, not the blur.
- FIXED (confirmed by the user in play): the sphere
  around Kaine was a solid bright disc; in the original (the user's screen recording) it is a
  glowing red rim with a see-through centre. Cause: no generated pixel parameter. With
  SQ_PROGRAM_CNTL (0x2180) bit 18 set, the console replaces the interpolator numbered in
  SQ_CONTEXT_MISC (0x2181) bits 8-15 with the pixel's position (xy, whole pixels) and the sign
  bit of x set on a back face. The game's two-sided materials read it as `1 / x >= 0` and flip
  the normal for back faces; the sphere's layers (ps 977F5D4A37D9C39A, 4EEE96B916E520A0, the
  dark shell DCFB0D9513F263B2; no culling, additive) then light only the rim (fresnel to the
  fourth power). Without it every face counted as a front face and the far half of the sphere
  glowed at full strength. Now `RendererDraw::paramGen`, `paramGen` / `systemExtra.x` in the
  System constants, an SV_IsFrontFace input and a prologue in every pixel shader
  (xenos_shader.cpp). Seen live: interpolator 4 (B6D8EB27C0AE69CC), 5 (D461B8752A20D67B, the
  character material: its two-sided parts were lit as front faces too), 6 (46155F8651382D13).
  The point-sprite part of the parameter (zw) is zero. Captures: NFRM version 4 / NSEQ
  version 3; `replay_frame.ps1 -ParamGen '<interpolator>,<ps hash>[,...]'` for older ones.
  To look at a video reference without ffmpeg: copy it beside a one-line HTML page, serve the
  folder with `python -m http.server`, open it in the built-in browser, load the file into a
  blob (the plain server can't seek), set `currentTime` and take screenshots.
- DLC notice on Continue ("The quest "Recycled World" is now available." and where to find it:
  info-window messages 2481 / 2482, two pages to confirm on every load) is skipped: sub_8219B0D0
  shows it through sub_823C5140 while a flag at its object +0x20C00 is set, then clears the flag;
  the hook `SkipDlcNotice` at 0x8219B300 clears the flag and jumps past (xam.cpp;
  NIER_DLC_NOTICE=1 keeps the notice). The load route in loadsave_args.txt still works; its
  extra A presses now land after the save has loaded.
- Quit to Desktop replaces the title menu's fourth entry, "Downloadable Content" (it only opened
  the Xbox Live store, and was selectable only when signed in to Live). The title object's flag
  at +3043 means "offer the entry"; `TitleEnableQuit` at 0x82197498 (its setter, r4 = 1) and
  `TitleKeepQuitEnabled` at 0x8219B0B4 (r11 = 0, so sub_8219AF70 doesn't clear it) keep it on.
  Confirming the entry (sub_82199788, cursor +3020 == 3) reaches `TitleQuitToDesktop` at
  0x82199B20: settings are saved and the process ends, as closing the window does
  (pcsettings.cpp). The label is two DXT3 images (letters 213x14, glow 225x26), redrawn as
  "Quit to Desktop" by `TitleLabelFor` (glyphs.cpp) in the image's own colour and peak alpha. The letters image is too short for descenders (the
  original wording had none; anything below it was lit by the glow alone, so Q and p looked half
  lit): `DrawPromptMask` takes a row band, fits capitals plus descenders at 60% depth into it
  (the text comes out about 7% smaller).
  The user found it dead at boot and working after backing out of New Game (never reproduced
  in a test run, where it worked at boot): the flag is cleared when the title object is set up
  (0x8219ACF8) and the setter is called from one place only (0x8219B1E8, after the game
  re-checks its DLC), so `TitleInitQuit` at 0x8219ACFC now sets it at set-up too. Confirmed
  by the user ("all good").
  NIER_TITLE_DLC=1 keeps the original entry and label. Checked in a test run (entry
  highlights, confirm exits with code 0); not yet tried in a playtest.
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
- Save pages (30 slots): the row is back in use. The game has three save slots (packages
  GAME_DATA0-2) and only ever calls XamShowDeviceSelectorUI from this row, so that call now
  turns to the next of ten pages (`save_page` in settings.ini, wrapping), and the game, taking
  it for a newly chosen device, lists its saves again. Page 1 is the save folder itself, page
  N its subfolder `pageN`; SYSTEM_DATA is shared (`PackageFolder`, `ListContent` in xam.cpp).
  The row is labelled "<  Page N / 10  >" (`BlankSelectDeviceRow` now writes that into a guest
  buffer; `DisableSelectDeviceRow` does nothing). Left and Right on the row turn back and
  forward: the list's input routine tests confirm first (0x82427E7C on; cursor +8360, result
  +8356), and `SavePageTurn` at 0x82427E84 makes a Left or Right press on row 3 count as the
  confirm, noting the direction for the selector call (`InputLastGameState` gives the pad
  state the game last saw, key bindings included). Confirm itself turns forward. Checked on
  the load screen: Down from slot 3 reaches the row; Left 1 -> 10, Right -> 1 -> 2, A -> 3,
  each showing that page's slots. The user turned pages both ways in playtests ("it works").
  Not checked: saving into a slot of another page and loading it back.
  NIER_STORAGE_MESSAGES=1 keeps the row as the game's own.
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

## Handheld test (ROG Xbox Ally X, Radeon 890M), 2026-11-14

- The process is DPI-aware (`SetProcessDpiAwarenessContext`, main.cpp). Before, Windows scaling above
  100% made the game see a smaller screen (1280x720 on the 1080p panel at 150%) and Windows stretched it.
- `[perf]` log line every 600 frames (RenderFrame): frame rate, and the render thread's time a frame split
  into recording, present and waiting for the GPU, with the render and window sizes.
- The FSR loader takes the newest upscaler it finds, and the AMD driver there supplied FSR 4.1.2 for the
  FSR 3.1 setting. The FSR 3.1 setting now asks for the newest 3.x (`TemporalVersion`, ffxOverrideVersion);
  the new FSR 4 setting (`upscaling = 3`) takes a 4.x, or 3.1 when the system has none.
- GPU time by stage: timestamps go into the command list where the stage changes (`StageMark`: shadows,
  scene, post, exposure, upscaling, 2D, present) and the `[perf]` line lists the averages. The passes are
  found for timing even at settings where they need no treatment of their own (`always` on ShadowPasses,
  DataPasses, FindFlatPhase), so the split is the same at every setting. `RendererPerfTake` hands the sums out.
- System load beside the frame rate counter (`NIER_SHOW_LOAD=1`, SampleLoad in renderer_d3d12.cpp): the
  system's CPU and GPU use and the game's RAM and VRAM, sampled twice a second.
