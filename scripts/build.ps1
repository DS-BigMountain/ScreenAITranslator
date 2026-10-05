[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateSet('Visual Studio 18 2026', 'Visual Studio 17 2022')][string]$Generator = 'Visual Studio 18 2026',
    [switch]$Package,
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

if (!$SkipBuild) {
    Invoke-Checked 'cmake' @('-S', $projectRoot, '-B', $buildRoot, '-G', $Generator, '-A', 'x64')
    Invoke-Checked 'cmake' @('--build', $buildRoot, '--config', $Configuration, '--parallel')
}
if (!$SkipTests) {
    Invoke-Checked 'ctest' @('--test-dir', $buildRoot, '-C', $Configuration, '--output-on-failure')
}
$appBinary = Join-Path $buildRoot "$Configuration/ScreenAITranslator.exe"
if (!(Test-Path -LiteralPath $appBinary)) { throw "Application binary missing: $appBinary" }
$appVersion = (Get-Item -LiteralPath $appBinary).VersionInfo.ProductVersion
if ($appVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$') { throw 'Application version is missing or invalid.' }
$distributionRoot = Join-Path $projectRoot "__release_packages__/$appVersion"
$portableName = "ScreenAITranslator-$appVersion.exe"
New-Item -ItemType Directory -Force -Path (Join-Path $distributionRoot 'licenses') | Out-Null
Copy-Item -LiteralPath $appBinary -Destination (Join-Path $distributionRoot $portableName) -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $distributionRoot 'licenses/nlohmann-json-LICENSE.txt') -Force
$binaryHash = (Get-FileHash -LiteralPath $appBinary -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath (Join-Path $distributionRoot $portableName) -Algorithm SHA256).Hash -ne $binaryHash) { throw 'Portable binary integrity verification failed.' }
Write-Host "Application ready: $(Join-Path $distributionRoot $portableName)"

$portableZipName = "ScreenAITranslator-$appVersion-portable.zip"
$portableZipPath = Join-Path $distributionRoot $portableZipName
Compress-Archive -LiteralPath (Join-Path $distributionRoot $portableName), (Join-Path $distributionRoot 'licenses') -DestinationPath $portableZipPath -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($portableZipPath)
try {
    foreach ($entryName in @($portableName, 'licenses/nlohmann-json-LICENSE.txt')) {
        $entry = $archive.GetEntry($entryName)
        if (!$entry) { throw "Portable archive entry missing: $entryName" }
        $stream = $entry.Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $entryHash = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
        finally { $stream.Dispose(); $hasher.Dispose() }
        if ($entryHash -ne (Get-FileHash -LiteralPath (Join-Path $distributionRoot $entryName) -Algorithm SHA256).Hash) {
            throw "Portable archive integrity verification failed: $entryName"
        }
    }
} finally { $archive.Dispose() }
Write-Host "Portable archive ready: $portableZipPath"

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
    if (!(Test-Path -LiteralPath $InnoCompiler)) { throw "Compiler not found: $InnoCompiler" }
    Invoke-Checked $InnoCompiler @('/Qp', "/DAppVersion=$appVersion", "/DAppBinary=$appBinary", (Join-Path $projectRoot 'installer/setup.iss'))
    $setupName = "ScreenAITranslator-Setup-$appVersion.exe"
    $setupPath = Join-Path $distributionRoot $setupName
    if (!(Test-Path -LiteralPath $setupPath)) { throw 'Installer compiler did not produce the expected setup EXE.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $setupPath
    if ($signature.Status -notin @('Valid', 'NotSigned')) { throw "Installer signature verification failed: $($signature.Status)" }
    Write-Host "Installer ready: $setupPath"

}

