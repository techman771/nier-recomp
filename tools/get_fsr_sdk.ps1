# Fetches what the runtime needs of AMD's FSR SDK 2.4.0 (MIT licensed) into tools\FidelityFX\sdk:
# the API headers, and the signed loader and upscaler DLLs (FSR 3.1, and FSR 4 on Radeon cards that
# support it). The Upscaling setting's "FSR 3.1" uses them (recomp/runtime/renderer_d3d12.cpp);
# compile_ppc.ps1 copies the DLLs beside nier.exe. tools\FidelityFX is not committed.
$target = Join-Path $PSScriptRoot 'FidelityFX\sdk'
$base = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX'
$files = @(
    @{ path = 'api/include/ffx_api.h' },
    @{ path = 'api/include/ffx_api_types.h' },
    @{ path = 'api/include/ffx_api_loader.h' },
    @{ path = 'api/include/dx12/ffx_api_dx12.h' },
    @{ path = 'upscalers/include/ffx_upscale.h' },
    @{ path = 'signedbin/amd_fidelityfx_loader_dx12.dll'; sha256 = 'e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb' },
    @{ path = 'signedbin/amd_fidelityfx_upscaler_dx12.dll'; sha256 = 'aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace' }
)
foreach ($file in $files) {
    $out = Join-Path $target $file.path
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    if (-not (Test-Path $out)) { Invoke-WebRequest -Uri "$base/$($file.path)" -OutFile $out -UseBasicParsing }
    if ($file.sha256) {
        $hash = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
        if ($hash -ne $file.sha256) { Remove-Item $out; throw "FSR SDK download hash mismatch for $($file.path): $hash" }
    }
}
"FSR SDK files present in $target"
