param(
    [string]$BuildDir = 'bin\x64\Release\v061',
    [string]$Version = '0.6.1'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root $BuildDir
$releaseRoot = Join-Path $root 'release'
$release = Join-Path $releaseRoot "AppleMusicASIO-$Version"
$zip = "$release.zip"

if (!(Test-Path $build)) { throw "build directory not found: $build" }
if (Test-Path $release) { Remove-Item -Recurse -Force $release }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $release | Out-Null

$files = @(
    'AMAudioBridgeSetup.exe',
    'AMAudioBridge.dll',
    'AMVerifierShim.dll',
    'AMPrefsPersist.dll',
    'AMAudioBridge.ini'
)
foreach ($name in $files) {
    $source = Join-Path $build $name
    if (!(Test-Path $source)) { throw "missing release file: $name" }
    Copy-Item $source (Join-Path $release $name)
}
Copy-Item (Join-Path $root 'README.md') (Join-Path $release 'README.md')
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $release 'LICENSE')

foreach ($textFile in @('README.md', 'AMAudioBridge.ini', 'LICENSE')) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $release $textFile))
    foreach ($value in $bytes) {
        if ($value -gt 127) { throw "non-ASCII release file: $textFile" }
    }
}

Compress-Archive -Path (Join-Path $release '*') -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item $zip).Length
Write-Host "packaged $release"
Write-Host "zip: $zip ($size bytes)"
