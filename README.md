# NieR Gestalt Recompiled

A static recompilation of NieR (Xbox 360, 2010) into a native Windows program. The game's PowerPC
code is translated to C++ ahead of time and linked against a runtime that stands in for the
console: its kernel calls, its GPU (drawn with Direct3D 12), audio, input and storage.

**This repository contains no game files.** You need your own copy of the game, extracted from a
disc you own. Nothing from the game, and nothing generated from it, is distributed here or may be
contributed here (the recompiled sources contain the game's code).

## Status

In development. The game boots, plays and saves; a complete playthrough has not been verified
yet, so expect problems. `NOTES.md` is the working journal of the port: what was found, what was
measured and what is still open. It is long and unedited.

## What the port adds

- Frame rates above the console's 30: 40, 60, 120, 144, 240 fps or uncapped.
- Output resolutions from 720p to 2880p, with FSR 1, FSR 3.1 or DLSS upscaling (Native AA
  through Ultra Performance).
- Higher resolution shadow maps.
- Controllers through SDL3 (Xbox, PlayStation, Steam Deck), with matching button glyphs, and
  keyboard and mouse.
- A PC Settings page in the game's own options menu.
- Runs on the Steam Deck through Proton (see below).

## Requirements

- Windows 10 or 11, a Direct3D 12 graphics card.
- A processor with AVX2 (Intel Haswell, AMD Excavator or Zen, or later).
- To build: Visual Studio 2026 Community (CMake, Ninja, clang-cl), Python 3.

## Building

Building from a clean machine has not been tested end to end; the steps below are the ones used
during development, and `NOTES.md` ("Toolchain", "Recompiler setup") has the details.

1. Clone with the submodule: `git clone --recurse-submodules <this repository>`.
2. Apply `tools/xenonrecomp-nier.patch` to `tools/XenonRecomp` (it is written against upstream
   commit `ddd128b`) and build XenonRecomp into `tools/build`.
3. Put your extracted game in `Nier/` (`default.xex`, `media/`). It is read-only to every tool
   here and ignored by git.
4. In PowerShell, load the toolchain: `. tools\devshell.ps1`
5. Fetch the third-party pieces that are not committed: `tools\get_sdl3.ps1`,
   `tools\get_fsr1.ps1`, `tools\get_fsr_sdk.ps1`.
6. Recompile the game's code: `tools\recompile.ps1`
7. Build the program: `tools\compile_ppc.ps1`. The result is `recomp\build\nier.exe`.

## DLSS

DLSS needs an NVIDIA RTX card and four files of NVIDIA's that are not part of this project and
are not shipped with it: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` (Streamline, MIT
licensed) and `nvngx_dlss.dll` (the DLSS model, under NVIDIA's own licence). To get them, run
`get_dlss.cmd` in the game's folder, beside `nier.exe`. It downloads NVIDIA's Streamline SDK
2.14.16 from NVIDIA's GitHub releases (276 MB), checks it, keeps those four files and NVIDIA's
licence text (60 MB) and deletes the rest. After that DLSS appears under Options > Game Settings >
PC Settings > Upscaling. Without the files the game runs as before and the entry is not shown.

When building from source, `tools\get_streamline.ps1` fetches the same SDK into `tools\Streamline`
before step 7; a build made without it has no DLSS support at all.

## Steam Deck

Add `nier.exe` to Steam as a non-Steam game, run it with Proton, and use the launch script so the
shader compiler shipped with the build is used and the GPU clock is held steady:

    "/path/to/NieR-Recomp/deck-launch.sh" %command%

`tools/deck-launch.sh` explains what it does. Settings that held up at 60 fps in testing on an
LCD Deck:

| Setting | Value |
| --- | --- |
| Deck refresh rate | 60 Hz |
| Frame Rate | 60 |
| Output Resolution | 720p |
| Upscaling | FSR 3.1, Native |
| Shadows | High |
| GPU clock | 1300 MHz (the script's default; `NIER_GPU_CLOCK=<MHz>` before the script for another) |

The heaviest scenes still show a few late frames. A higher clock did not help: at 1600 MHz the
GPU's work took as long as before, the Deck ran hotter and the CPU was slowed.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) by hedge-dev, the recompiler this
  port is built on (MIT).
- [SDL3](https://libsdl.org/) for controllers (zlib).
- AMD FidelityFX Super Resolution 1 and the FSR SDK (MIT).

## Licence

The code in this repository is licensed under the GNU General Public License, version 3 (see
`LICENSE`). Copyright (c) 2026 NieR Gestalt Recompiled contributors.

NieR is the property of Square Enix. This project is not affiliated with or endorsed by Square
Enix or Microsoft.
