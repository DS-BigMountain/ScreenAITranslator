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
Get-Verified "$base/rec/korean_PP-OCRv5_rec_mobile.onnx" "$runtime/models/rec-ko.onnx" 'CD6E2EA50F6943CA7271EB8C56A877A5A90720B7047FE9C41A2E541A25773C9B'
# Strongest-available upgrade set. Each file is optional at runtime: the application
# prefers it when present and falls back to the mobile weights above when staged without it.
# Optional stronger detector. The 88 MB server weights split large headline lines into
# per-word fragments and add roughly 3.2 s per page on CPU, while mobile detection keeps
# byte-identical line boxes for the layout baseline. Placing det-server.onnx in
# models/ enables it without a rebuild. Source and integrity for manual staging:
# https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/det/ch_PP-OCRv5_det_server.onnx
# SHA256 0F8846B1D4BBA223A2A2F9D9B44022FBC22CC019051A602B41A7FDA9667E4CAD
# Japanese/Chinese/English recognizer: PP-OCRv6 small is a unified 50-language model
# (kana and CJK included) that runs close to mobile speed on CPU.
$v6 = 'https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6'
Get-Verified "$v6/rec/PP-OCRv6_rec_small.onnx" "$runtime/models/rec-ja-server.onnx" '6F327246B50388F3C176AE304BD95767EA6DC0C9AE92153EF8CBE210B3C14884'
# Text-line orientation (0/180 degrees): corrects upside-down crops before recognition.
Get-Verified "$base/cls/ch_PP-LCNet_x1_0_textline_ori_cls_server.onnx" "$runtime/models/cls.onnx" '7D3C02EF6C7DA8AE08B4347CC7695B2081AAE68C325D64375724ECF39C99E743'
Copy-Item -LiteralPath "$cache/ort/runtimes/win-x64/native/onnxruntime.dll" -Destination $runtime -Force
Copy-Item -LiteralPath "$cache/ort/LICENSE" -Destination "$runtime/licenses/ONNXRuntime-LICENSE.txt" -Force
Copy-Item -LiteralPath "$cache/ort/ThirdPartyNotices.txt" -Destination "$runtime/licenses/ONNXRuntime-ThirdPartyNotices.txt" -Force
Copy-Item -LiteralPath "$PSScriptRoot/../third_party/ocr/PaddleOCR-LICENSE.txt", "$PSScriptRoot/../third_party/ocr/RapidOCR-LICENSE.txt", "$PSScriptRoot/../third_party/ocr/NOTICE.txt" -Destination "$runtime/licenses" -Force
$signature = Get-AuthenticodeSignature -LiteralPath "$runtime/onnxruntime.dll"
if ($signature.Status -ne 'Valid') { throw "ONNX Runtime signature verification failed: $($signature.Status)" }
Write-Host 'OCR models verified; ONNX Runtime signature valid.'

