[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$installRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build/test-output/install-smoke'))
if (!$installRoot.StartsWith($projectRoot + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test directory.' }
$appVersion = (Get-Item -LiteralPath (Join-Path $projectRoot 'build/Release/ScreenAITranslator.exe')).VersionInfo.ProductVersion
if ($appVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$') { throw 'Application version is missing or invalid.' }
$setup = Join-Path $projectRoot "__release_packages__/$appVersion/ScreenAITranslator-Setup-$appVersion.exe"
$runKey = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Run'
$uninstallKey = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/{2B5E7B5E-E962-42F0-A42D-6A5843E925FA}_is1'
if (Get-Process -Name 'ScreenAITranslator*' -ErrorAction SilentlyContinue) { throw 'ScreenAITranslator is running; isolated installer test refused.' }
if ((Get-ItemProperty -LiteralPath $runKey -Name ScreenAITranslator -ErrorAction SilentlyContinue) -or (Test-Path -LiteralPath $uninstallKey)) {
    throw 'An existing ScreenAITranslator installation or startup entry is present; isolated installer test refused.'
}
if (Test-Path -LiteralPath $installRoot) { throw 'Test directory already exists; inspect it before rerunning.' }
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'build/test-output') | Out-Null
$uninstaller = Join-Path $installRoot 'unins000.exe'
try {
    $installArgs = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/NOICONS','/TASKS="startup"',"/DIR=`"$installRoot`"", "/LOG=`"$(Join-Path $projectRoot 'build/test-output/installer-install.log')`"")
    $process = Start-Process -FilePath $setup -ArgumentList $installArgs -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Install failed: $($process.ExitCode)" }
    $installed = Join-Path $installRoot 'ScreenAITranslator.exe'
    if (!(Test-Path -LiteralPath $installed) -or !(Test-Path -LiteralPath $uninstaller)) { throw 'Installer did not lay down required files.' }
    if ((Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath (Join-Path $projectRoot 'build/Release/ScreenAITranslator.exe')).Hash) { throw 'Installed EXE differs from verified build.' }
    $startup = (Get-ItemProperty -LiteralPath $runKey -Name ScreenAITranslator).ScreenAITranslator
    if ($startup -ne "`"$installed`" --background") { throw 'Startup command is incorrect.' }
    if (!(Test-Path -LiteralPath (Join-Path $installRoot 'licenses/nlohmann-json-LICENSE.txt'))) { throw 'Dependency license not installed.' }
    Write-Output 'Installer: PASS (current-user install, exact binary, license, quoted background startup command)'
} finally {
    if (Test-Path -LiteralPath $uninstaller) {
        $uninstallArgs = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART', "/LOG=`"$(Join-Path $projectRoot 'build/test-output/installer-uninstall.log')`"")
        $uninstallProcess = Start-Process -FilePath $uninstaller -ArgumentList $uninstallArgs -WindowStyle Hidden -Wait -PassThru
        if ($uninstallProcess.ExitCode -ne 0) { throw "Uninstall failed: $($uninstallProcess.ExitCode)" }
    }
}
if ((Get-ItemProperty -LiteralPath $runKey -Name ScreenAITranslator -ErrorAction SilentlyContinue) -or (Test-Path -LiteralPath $uninstallKey) -or (Test-Path -LiteralPath (Join-Path $installRoot 'ScreenAITranslator.exe'))) { throw 'Installer cleanup failed.' }
Write-Output 'Uninstaller: PASS (test binary, startup and uninstall registration removed)'
