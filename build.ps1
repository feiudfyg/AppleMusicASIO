param([string]$OutputDir = 'bin\x64\Release')

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw "vswhere not found" }
$vsPath = & $vswhere -latest -products * -property installationPath
if (!$vsPath) { throw "Visual Studio not found" }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path $vcvars)) { throw "vcvars64.bat not found" }

$out = Join-Path $root $OutputDir
$objRoot = Join-Path $root (Join-Path 'obj' $OutputDir)
New-Item -ItemType Directory -Force $out | Out-Null
New-Item -ItemType Directory -Force $objRoot | Out-Null

$common = @(
    '/nologo', '/std:c11', '/MT', '/W3', '/utf-8', '/guard:cf', '/Gy', '/Gw',
    '/I', 'src', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
    '/DUNICODE', '/D_UNICODE'
)

function Assert-Ascii([string[]]$paths) {
    foreach ($path in $paths) {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        foreach ($value in $bytes) {
            if ($value -gt 127) { throw "non-ASCII content: $path" }
        }
    }
}

$asciiFiles = @(
    Get-ChildItem (Join-Path $root 'src') -File -Recurse |
        Where-Object { $_.Extension -eq '.c' -or $_.Extension -eq '.h' } |
        ForEach-Object { $_.FullName }
)
$asciiFiles += (Join-Path $root 'AMAudioBridge.ini')
$asciiFiles += (Join-Path $root 'README.md')
$asciiFiles += (Join-Path $root 'LICENSE')
Assert-Ascii $asciiFiles

function Build-Target([string]$name, [string[]]$sources, [string]$linkArgs,
                     [string[]]$compileArgs = @()) {
    $obj = Join-Path $objRoot $name
    New-Item -ItemType Directory -Force $obj | Out-Null
    $optimize = if ($compileArgs -contains '/O1') { '/O1' } else { '/O2' }
    $extra = @($compileArgs | Where-Object { $_ -ne '/O1' })
    $clArgs = $common + @($optimize) + $extra + @('/Fo' + $obj + '\') + $sources +
              @('/link') + ($linkArgs -split ' ') +
              @('/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO')
    $cmd = "`"$vcvars`" >nul && cl " + ($clArgs -join ' ')
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
}

$dllSources = @(
    'src\bridge.c', 'src\log.c', 'src\config.c', 'src\format.c', 'src\ring.c',
    'src\asio_host.c', 'src\sink_asio.c', 'src\sink_wasapi.c', 'src\stream.c',
    'src\hooks.c', 'src\app_hooks.c', 'src\guids.c', 'src\setup_common.c'
)
Build-Target 'bridge' $dllSources ('/DLL /OUT:' + (Join-Path $out 'AMAudioBridge.dll') +
    ' /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE' +
    ' ole32.lib uuid.lib advapi32.lib propsys.lib shell32.lib')

$probeSources = @('src\probe.c', 'src\format.c', 'src\guids.c')
Build-Target 'probe' $probeSources ('/OUT:' + (Join-Path $out 'AMAudioProbe.exe') +
    ' ole32.lib uuid.lib')

Build-Target 'asiotool' @('src\asiotool.c') ('/OUT:' + (Join-Path $out 'AMAsioTool.exe') +
    ' ole32.lib uuid.lib')

Build-Target 'coreaudio' @('src\coreaudio_probe.c') ('/DLL /OUT:' + (Join-Path $out 'AMCoreAudioProbe.dll') +
    ' ole32.lib uuid.lib')

Build-Target 'prefsprobe' @('src\prefs_probe.c') ('/DLL /OUT:' + (Join-Path $out 'AMPrefsProbe.dll') +
    ' ole32.lib uuid.lib') @('/O1')

Build-Target 'prefspersist' @('src\prefs_probe.c') ('/DLL /OUT:' + (Join-Path $out 'AMPrefsPersist.dll') +
    ' ole32.lib uuid.lib /LTCG') @('/O1', '/GL')

Build-Target 'earlyproxy' @('src\early_proxy.c', 'src\log.c') ('/DLL /OUT:' + (Join-Path $out 'AMEarlyProxy.dll') +
    ' /EXPORT:DllGetClassObject,PRIVATE /EXPORT:DllCanUnloadNow,PRIVATE ole32.lib advapi32.lib /LTCG') @('/O1', '/GL')

Build-Target 'setup' @('src\setup.c', 'src\setup_common.c') ('/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'AMAudioBridgeSetup.exe') +
    ' user32.lib gdi32.lib shell32.lib advapi32.lib /LTCG') @('/O1', '/GL')

Build-Target 'injector' @('src\injector.c') ('/OUT:' + (Join-Path $out 'AMAudioBridgeInjector.exe') +
    ' ole32.lib uuid.lib advapi32.lib')

Copy-Item (Join-Path $root 'AMAudioBridge.ini') (Join-Path $out 'AMAudioBridge.ini') -Force

$binaries = Get-ChildItem $out -File | Where-Object {
    $_.Extension -eq '.exe' -or $_.Extension -eq '.dll'
}
foreach ($binary in $binaries) {
    $cmd = "`"$vcvars`" >nul && dumpbin /nologo /dependents `"$($binary.FullName)`""
    $output = cmd.exe /c $cmd 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dependency check failed: $($binary.Name)" }
    $text = $output -join "`n"
    if ($text -match 'VCRUNTIME|MSVCP|ucrtbase|api-ms-win-crt') {
        throw "dynamic C runtime dependency: $($binary.Name)"
    }
}
Write-Host "ASCII clean; static runtime; built $out"
