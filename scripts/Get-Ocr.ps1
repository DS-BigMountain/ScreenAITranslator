[CmdletBinding()]
param([string]$BuildRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build'))
$ErrorActionPreference = 'Stop'
$cache = Join-Path $BuildRoot '_deps/ocr'
$runtime = Join-Path $BuildRoot 'ocr-runtime'
New-Item -ItemType Directory -Force -Path $cache, "$runtime/models", "$runtime/licenses" | Out-Null
function Get-Verified([string]$Url, [string]$Path, [string]$Hash) {
    if (!(Test-Path -LiteralPath $Path) -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        Invoke-WebRequest -Uri $Url -OutFile "$Path.download"
        if ((Get-FileHash -LiteralPath "$Path.download" -Algorithm SHA256).Hash -ne $Hash) { throw "OCR dependency integrity failure: $Path" }
        Move-Item -LiteralPath "$Path.download" -Destination $Path -Force
    }
}
Get-Verified 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.22.1/microsoft.ml.onnxruntime.1.22.1.nupkg' "$cache/onnxruntime.nupkg" '7B091E012DCFEF2F9CC965F2252041101B1840C748FFBC1C18BA047BD6508B1C'
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (!(Test-Path -LiteralPath "$cache/ort/build/native/include/onnxruntime_cxx_api.h")) {
    [IO.Compression.ZipFile]::ExtractToDirectory("$cache/onnxruntime.nupkg", "$cache/ort")
}
$base = 'https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5'
Get-Verified "$base/det/ch_PP-OCRv5_det_mobile.onnx" "$runtime/models/det.onnx" '4D97C44A20D30A81AAD087D6A396B08F786C4635742AFC391F6621F5C6AE78AE'
Get-Verified "$base/rec/ch_PP-OCRv5_rec_mobile.onnx" "$runtime/models/rec-ja.onnx" '5825FC7EBF84AE7A412BE049820B4D86D77620F204A041697B0494669B1742C5'
Get-Verified "$base/rec/en_PP-OCRv5_rec_mobile.onnx" "$runtime/models/rec-en.onnx" 'C3461ADD59BB4323ECBA96A492AB75E06DDA42467C9E3D0C18DB5D1D21924BE8'
Copy-Item -LiteralPath "$cache/ort/runtimes/win-x64/native/onnxruntime.dll" -Destination $runtime -Force
Copy-Item -LiteralPath "$cache/ort/LICENSE" -Destination "$runtime/licenses/ONNXRuntime-LICENSE.txt" -Force
Copy-Item -LiteralPath "$cache/ort/ThirdPartyNotices.txt" -Destination "$runtime/licenses/ONNXRuntime-ThirdPartyNotices.txt" -Force
Copy-Item -LiteralPath "$PSScriptRoot/../third_party/ocr/PaddleOCR-LICENSE.txt", "$PSScriptRoot/../third_party/ocr/RapidOCR-LICENSE.txt", "$PSScriptRoot/../third_party/ocr/NOTICE.txt" -Destination "$runtime/licenses" -Force
$signature = Get-AuthenticodeSignature -LiteralPath "$runtime/onnxruntime.dll"
if ($signature.Status -ne 'Valid') { throw "ONNX Runtime signature verification failed: $($signature.Status)" }
Write-Host 'OCR models verified; ONNX Runtime signature valid.'
