# Rebuilds XenonRecomp, regenerates the config and code, then runs the audit and opcode tests. Load devshell.ps1 first.
$root = Split-Path $PSScriptRoot -Parent
$t = Join-Path $root 'tools'; $x = "$t\XenonRecomp"; $b = "$t\build"; $o = "$b\xenonscan"; $tests = "$b\opcode_tests"
cmake --build $b --target XenonRecomp | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$t\make_config.ps1" | Select-String 'final tables|table-shaped dispatches'
$log = "$root\recomp\recomp.log"
& "$b\XenonRecomp\XenonRecomp.exe" "$root\recomp\config\nier.toml" "$x\XenonUtils\ppc_context.h" > $log 2>&1
"recomp exit: $LASTEXITCODE"
"unrecognized: " + @(Get-Content $log | Where-Object { $_ -match '^Unrecognized instruction' }).Count
"traps: " + @(Select-String -Path "$root\recomp\out\ppc_recomp.*.cpp" -SimpleMatch '__builtin_debugtrap').Count
python -I "$t\xenonscan\check_functions.py" "$root\recomp\out" "$o\dis.exe" "$root\Nier\default.xex" $o | Select-String 'ERROR markers|ghosts'
"audit exit: $LASTEXITCODE"
python -I "$t\xenonscan\opcode_tests\gen.py" "$root\recomp\out" "$tests\emitted.inc"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
clang-cl /nologo /std:c++20 /O1 /MT /EHsc /arch:AVX2 '-msse4.1' '-ffp-contract=off' -fno-strict-aliasing -Wno-everything "/I$x\thirdparty\simde" "/I$root\recomp\out" "/I$tests" "$t\xenonscan\opcode_tests\harness.cpp" "/Fo:$tests\harness.obj" "/Fe:$tests\harness.exe"
& "$tests\harness.exe"
"opcode tests exit: $LASTEXITCODE"
