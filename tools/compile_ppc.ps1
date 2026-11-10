# Builds recomp/build/nier.exe from the recompiled code and the runtime. Load devshell.ps1 first.
# The runtime is optimized too (it emulates the GPU's command processor, which bounds gameplay speed);
# -RuntimeOpt /Od gives a debuggable runtime. Symbols (/Z7) are kept either way.
param([string]$Opt = '/O2', [string]$RuntimeOpt = '/O2')
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'recomp\build'
New-Item -ItemType Directory -Force $build | Out-Null

python -I (Join-Path $PSScriptRoot 'gen_stubs.py') (Join-Path $root 'recomp\out') (Join-Path $root 'recomp\runtime')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sources = Get-ChildItem (Join-Path $root 'recomp\out') -Filter *.cpp | Select-Object -ExpandProperty Name
$runtime = Get-ChildItem (Join-Path $root 'recomp\runtime') -Filter *.cpp | Select-Object -ExpandProperty Name
$x = '../../tools/XenonRecomp'
$common = "/nologo /c /std:c++20 /MT /arch:AVX -msse4.1 -fno-strict-aliasing -Wno-everything -I$x/thirdparty/simde -I../out"
$lib = '../../tools/build'
$libs = "$lib/XenonUtils/XenonUtils.lib $lib/thirdparty/disasm/disasm.lib $lib/thirdparty/fmt/fmt.lib $lib/thirdparty/xxHash/cmake_unofficial/xxhash.lib"
$ninja = @(
    "rule cxx",
    "  command = clang-cl $common $Opt /EHs-c- /GS- `$in /Fo:`$out",
    "  description = `$in",
    "",
    "rule rt",
    "  command = clang-cl $common $RuntimeOpt /Z7 /EHsc -I../runtime -I$x/XenonUtils -I$x/thirdparty -I$x/thirdparty/disasm `$in /Fo:`$out",
    "  description = `$in",
    "",
    "rule link",
    "  command = lld-link /nologo /OUT:`$out /SUBSYSTEM:CONSOLE /STACK:0x10000000,0x100000 @`$out.rsp $libs gdi32.lib user32.lib winmm.lib ole32.lib xinput.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib dbghelp.lib /DEBUG:FULL",
    "  rspfile = `$out.rsp",
    "  rspfile_content = `$in",
    "  description = linking `$out",
    ""
)
$objects = @()
foreach ($s in $sources) { $ninja += "build $s.obj: cxx ../out/$s"; $objects += "$s.obj" }
# Every runtime header is an implicit dependency (no compiler dependency tracking here), so a header
# change rebuilds all runtime files.
$headers = (Get-ChildItem (Join-Path $root 'recomp\runtime') -Filter *.h | ForEach-Object { "../runtime/$($_.Name)" }) -join ' '
foreach ($s in $runtime) { $ninja += "build rt_$s.obj: rt ../runtime/$s | $headers"; $objects += "rt_$s.obj" }
$ninja += "build nier.exe: link $($objects -join ' ')"
Set-Content -Encoding ascii (Join-Path $build 'build.ninja') $ninja
ninja -C $build -k 0
exit $LASTEXITCODE
