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
materials. Remaining visible fault: the cinematic letterbox bars are translucent grey instead of
solid.

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
