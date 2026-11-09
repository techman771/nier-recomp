# Dot-source this to get CMake, Ninja, clang-cl and the MSVC/SDK paths in the current shell:
#   . "C:\Projects\Nier Recomp\tools\devshell.ps1"
$vs = 'C:\Program Files\Microsoft Visual Studio\18\Community'
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg'
$env:Path = (Join-Path $vs 'VC\Tools\Llvm\x64\bin') + ';' + $env:Path
