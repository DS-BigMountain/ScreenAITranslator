[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $projectRoot 'build-tools/inno-6.7.3'
$compilerPath = Join-Path $toolRoot 'ISCC.exe'
if (Test-Path -LiteralPath $compilerPath) {
    Write-Output $compilerPath
    return
}

# Pinned immutable official release; digest from its GitHub release asset metadata.
$downloadUrl = 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe'
$expectedSha256 = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
$downloadRoot = Join-Path $projectRoot 'build-tools/downloads'
New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
$downloadPath = Join-Path $downloadRoot 'innosetup-6.7.3.exe'
if (!(Test-Path -LiteralPath $downloadPath)) {
    Write-Host 'Downloading the official Inno Setup 6.7.3 compiler.'
    Invoke-WebRequest -Uri $downloadUrl -OutFile $downloadPath -UseBasicParsing
}
$actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedSha256) { throw 'Inno Setup checksum verification failed. The downloaded file was not executed.' }
$signature = Get-AuthenticodeSignature -LiteralPath $downloadPath
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Pyrsys B\.V\.') {
    throw "Inno Setup publisher verification failed ($($signature.Status)). The downloaded file was not executed."
}

# Official /PORTABLE=1 mode suppresses uninstall registration; no icons or file associations.
$installArguments = @('/PORTABLE=1', '/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS', '/TASKS=""', "/DIR=`"$toolRoot`"", "/LOG=`"$(Join-Path $downloadRoot 'inno-portable-setup.log')`"")
$process = Start-Process -FilePath $downloadPath -ArgumentList $installArguments -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $compilerPath)) {
    throw "Portable Inno Setup extraction failed (exit $($process.ExitCode)); see build-tools/downloads/inno-portable-setup.log."
}
Write-Host "Portable compiler ready: $compilerPath"
Write-Output $compilerPath
