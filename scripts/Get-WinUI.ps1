[CmdletBinding()]
param([string]$BuildRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build'))
$ErrorActionPreference = 'Stop'
$deps = Join-Path $BuildRoot '_deps'
$packages = Join-Path $deps 'packages'
$nuget = Join-Path $deps 'nuget.exe'
New-Item -ItemType Directory -Force -Path $deps | Out-Null
if (!(Test-Path -LiteralPath $nuget)) {
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe' -OutFile $nuget
}
$signature = Get-AuthenticodeSignature -LiteralPath $nuget
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw 'NuGet signature verification failed.'
}
foreach ($dependency in @(@('Microsoft.WindowsAppSDK.WinUI', '1.8.260803003'), @('Microsoft.Windows.CppWinRT', '3.0.260818.1'))) {
    if (!(Test-Path -LiteralPath (Join-Path $packages "$($dependency[0]).$($dependency[1])"))) {
        & $nuget install $dependency[0] -Version $dependency[1] -OutputDirectory $packages -NonInteractive -Verbosity quiet -Source 'https://api.nuget.org/v3/index.json'
        if ($LASTEXITCODE) { throw 'Windows App SDK restore failed.' }
    }
}
$projection = Join-Path $deps 'winui-projection'
if (!(Test-Path -LiteralPath (Join-Path $projection 'complete.stamp'))) {
    & "$packages/Microsoft.Windows.CppWinRT.3.0.260818.1/bin/cppwinrt.exe" -input sdk `
        -input "$packages/Microsoft.WindowsAppSDK.WinUI.1.8.260803003/metadata" `
        -input "$packages/Microsoft.Web.WebView2.1.0.3179.45/lib/Microsoft.Web.WebView2.Core.winmd" `
        -input "$packages/Microsoft.WindowsAppSDK.InteractiveExperiences.1.8.260708001/metadata/10.0.18362.0" `
        -input "$packages/Microsoft.WindowsAppSDK.Foundation.1.8.260803002/metadata" -output $projection
    if ($LASTEXITCODE) { throw 'C++/WinRT projection generation failed.' }
    Set-Content -LiteralPath (Join-Path $projection 'complete.stamp') -Value 'WinUI 1.8.260803003; CppWinRT 3.0.260818.1'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
[xml]$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'resources/app.manifest') -Raw
$files = @{}
foreach ($package in @('Microsoft.WindowsAppSDK.Foundation.1.8.260803002', 'Microsoft.WindowsAppSDK.InteractiveExperiences.1.8.260708001', 'Microsoft.WindowsAppSDK.WinUI.1.8.260803003')) {
    [xml]$fragment = Get-Content -LiteralPath "$packages/$package/runtimes-framework/package.appxfragment" -Raw
    foreach ($server in $fragment.SelectNodes('//*[local-name()="InProcessServer"]')) {
        $name = [string]$server.Path
        if (!$files.ContainsKey($name)) {
            $file = $manifest.CreateElement('file', 'urn:schemas-microsoft-com:asm.v1')
            $file.SetAttribute('name', $name)
            $manifest.DocumentElement.AppendChild($file) | Out-Null
            $files[$name] = $file
        }
        foreach ($type in $server.ActivatableClass) {
            $element = $manifest.CreateElement('activatableClass', 'urn:schemas-microsoft-com:winrt.v1')
            $element.SetAttribute('name', [string]$type.ActivatableClassId)
            $element.SetAttribute('threadingModel', [string]$type.ThreadingModel)
            $files[$name].AppendChild($element) | Out-Null
        }
    }
}
$manifest.Save((Join-Path $deps 'winui-app.manifest'))
