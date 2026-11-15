# Fetches AMD's FidelityFX Super Resolution 1 shader headers (MIT licensed) into tools\FidelityFX\fsr1.
# The runtime's Upscaling setting compiles its FSR 1 passes from them (recomp/runtime/renderer_d3d12.cpp);
# compile_ppc.ps1 copies them beside nier.exe. tools\FidelityFX is not committed.
$target = Join-Path $PSScriptRoot 'FidelityFX\fsr1'
$files = @(
    @{ name = 'ffx_a.h'; path = 'ffx-fsr/ffx_a.h'; sha256 = 'f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb' },
    @{ name = 'ffx_fsr1.h'; path = 'ffx-fsr/ffx_fsr1.h'; sha256 = '93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5' },
    @{ name = 'license.txt'; path = 'license.txt'; sha256 = 'db089274ce766da70f5b7d791029c3486f9f9e27c8c79c652689603d3192e802' }
)
New-Item -ItemType Directory -Force $target | Out-Null
foreach ($file in $files) {
    $out = Join-Path $target $file.name
    if (-not (Test-Path $out)) {
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/master/$($file.path)" -OutFile $out -UseBasicParsing
    }
    $hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $file.sha256) { Remove-Item $out; throw "FSR 1 download hash mismatch for $($file.name): $hash" }
}
"FSR 1 headers present in $target"
