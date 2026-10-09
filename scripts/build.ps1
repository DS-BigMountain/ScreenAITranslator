[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateSet('Visual Studio 18 2026', 'Visual Studio 17 2022')][string]$Generator = 'Visual Studio 18 2026',
    [switch]$Package,
    [switch]$Preview,
    [switch]$BootstrapInno,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [string]$InnoCompiler
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
function Invoke-Checked {
    param([string]$Program, [string[]]$ToolArguments)
    & $Program @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE." }
}
& (Join-Path $PSScriptRoot 'Get-Ocr.ps1') -BuildRoot $buildRoot
if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot 'Get-WinUI.ps1') -BuildRoot $buildRoot
    Invoke-Checked 'cmake' @('-S', $projectRoot, '-B', $buildRoot, '-G', $Generator, '-A', 'x64')
    Invoke-Checked 'cmake' @('--build', $buildRoot, '--config', $Configuration, '--parallel', '1')
}
if (!$SkipTests) { Invoke-Checked 'ctest' @('--test-dir', $buildRoot, '-C', $Configuration, '--output-on-failure') }
$appBinary = Join-Path $buildRoot "$Configuration/ScreenAITranslator.exe"
if (!(Test-Path -LiteralPath $appBinary)) { throw "Application binary missing: $appBinary" }
$appVersion = (Get-Item -LiteralPath $appBinary).VersionInfo.ProductVersion
if ($appVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$') { throw 'Application version is missing or invalid.' }
$suffix = if ($Preview) { '-WinUI3-preview' } else { '' }
$distributionRoot = Join-Path $projectRoot "__release_packages__/$appVersion"
if ($Preview) { $distributionRoot = Join-Path $distributionRoot 'WinUI3-preview' }
$stageRoot = Join-Path $buildRoot "package-stage/$Configuration$suffix"
# 暂存目录仅包含可再发行文件；构建、测试及诊断文件不进入交付包。
if (Test-Path -LiteralPath $stageRoot) {
    $resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
    $allowedStage = [IO.Path]::GetFullPath((Join-Path $buildRoot 'package-stage')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolvedStage.StartsWith($allowedStage, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging directory.' }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageRoot, $distributionRoot, (Join-Path $stageRoot 'licenses') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $buildRoot 'ocr-runtime') | Copy-Item -Destination $stageRoot -Recurse -Force
Copy-Item -LiteralPath $appBinary -Destination (Join-Path $stageRoot 'ScreenAITranslator.exe')
Copy-Item -LiteralPath (Join-Path $projectRoot 'CHANGELOG.md') -Destination (Join-Path $stageRoot '更新说明.md')
Get-ChildItem -LiteralPath (Join-Path $buildRoot 'winui-runtime') | Copy-Item -Destination $stageRoot -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $stageRoot 'licenses/nlohmann-json-LICENSE.txt')
foreach ($name in @('Foundation.1.8.260803002', 'InteractiveExperiences.1.8.260708001', 'WinUI.1.8.260803003')) {
    Copy-Item -LiteralPath (Join-Path $buildRoot "_deps/packages/Microsoft.WindowsAppSDK.$name/license.txt") -Destination (Join-Path $stageRoot "licenses/WindowsAppSDK-$name.txt")
}
foreach ($name in @('Microsoft.UI.Xaml.dll', 'Microsoft.WindowsAppRuntime.dll', 'resources.pri', 'vcruntime140.dll', 'msvcp140.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $stageRoot $name))) { throw "Required runtime file missing: $name" }
}
$binaryHash = (Get-FileHash -LiteralPath $appBinary -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath (Join-Path $stageRoot 'ScreenAITranslator.exe') -Algorithm SHA256).Hash -ne $binaryHash) { throw 'Application copy integrity verification failed.' }
foreach ($binary in Get-ChildItem -LiteralPath $stageRoot -File | Where-Object Extension -In '.exe', '.dll') {
    $signature = Get-AuthenticodeSignature -LiteralPath $binary.FullName
    if ($binary.Name -eq 'ScreenAITranslator.exe') {
        if ($signature.Status -notin @('Valid', 'NotSigned')) { throw "Application signature invalid: $($signature.Status)" }
    } elseif ($signature.Status -ne 'Valid') { throw "Runtime signature invalid: $($binary.Name): $($signature.Status)" }
}
$portableZipPath = Join-Path $distributionRoot "ScreenAITranslator-$appVersion$suffix-portable.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (Test-Path -LiteralPath $portableZipPath) { Remove-Item -LiteralPath $portableZipPath -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $portableZipPath, [IO.Compression.CompressionLevel]::Optimal, $false)
$archive = [IO.Compression.ZipFile]::OpenRead($portableZipPath)
try {
    $files = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -File)
    if ($archive.Entries.Count -ne $files.Count) { throw 'Portable archive file count differs from staging.' }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
        $entry = $archive.GetEntry($relative)
        if (!$entry) { throw "Portable archive entry missing: $relative" }
        $stream = $entry.Open(); $hasher = [Security.Cryptography.SHA256]::Create()
        try { $entryHash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
        finally { $stream.Dispose(); $hasher.Dispose() }
        if ($entryHash -ne (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) { throw "Portable archive integrity verification failed: $relative" }
    }
} finally { $archive.Dispose() }
Write-Host "Portable archive verified: $portableZipPath"
if ($Package) {
    if ($Configuration -ne 'Release') { throw 'Installer packages must use a Release build.' }
    if (!$InnoCompiler) {
        $localCompiler = Join-Path $projectRoot 'build-tools/inno-6.7.3/ISCC.exe'
        $installedCompiler = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $localCompiler) { $InnoCompiler = $localCompiler }
        elseif ($installedCompiler) { $InnoCompiler = $installedCompiler.Source }
        elseif ($BootstrapInno) { $InnoCompiler = & (Join-Path $PSScriptRoot 'Get-InnoSetup.ps1') }
        else { throw 'Inno Setup compiler not found. Use -BootstrapInno or -InnoCompiler <absolute ISCC.exe path>.' }
    }
    $setupName = "ScreenAITranslator-Setup-$appVersion$suffix"
    Invoke-Checked $InnoCompiler @('/Qp', "/DAppVersion=$appVersion", "/DAppStage=$stageRoot", "/DPackageOutput=$distributionRoot", "/DPackageName=$setupName", (Join-Path $projectRoot 'installer/setup.iss'))
    $setupPath = Join-Path $distributionRoot "$setupName.exe"
    if (!(Test-Path -LiteralPath $setupPath)) { throw 'Installer compiler did not produce the expected setup EXE.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $setupPath
    if ($signature.Status -notin @('Valid', 'NotSigned')) { throw "Installer signature verification failed: $($signature.Status)" }
    Write-Host "Installer ready: $setupPath"
}
