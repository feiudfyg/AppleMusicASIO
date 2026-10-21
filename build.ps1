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
    '/nologo', '/std:c11', '/O2', '/MT', '/W3', '/utf-8', '/guard:cf',
    '/I', 'src', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
    '/DUNICODE', '/D_UNICODE'
)

function Build-Target([string]$name, [string[]]$sources, [string]$linkArgs) {
    $obj = Join-Path $objRoot $name
    New-Item -ItemType Directory -Force $obj | Out-Null
    $clArgs = $common + @('/Fo' + $obj + '\') + $sources + @('/link') + ($linkArgs -split ' ')
    $cmd = "`"$vcvars`" >nul && cl " + ($clArgs -join ' ')
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
}

$dllSources = @(
    'src\bridge.c', 'src\log.c', 'src\config.c', 'src\format.c', 'src\ring.c',
    'src\asio_host.c', 'src\sink_asio.c', 'src\sink_wasapi.c', 'src\stream.c',
    'src\hooks.c', 'src\guids.c'
)
Build-Target 'bridge' $dllSources ('/DLL /OUT:' + (Join-Path $out 'AMAudioBridge.dll') +
    ' ole32.lib uuid.lib advapi32.lib propsys.lib shell32.lib')

$probeSources = @('src\probe.c', 'src\format.c', 'src\guids.c')
Build-Target 'probe' $probeSources ('/OUT:' + (Join-Path $out 'AMAudioProbe.exe') +
    ' ole32.lib uuid.lib')

Build-Target 'asiotool' @('src\asiotool.c') ('/OUT:' + (Join-Path $out 'AMAsioTool.exe') +
    ' ole32.lib uuid.lib')

Build-Target 'injector' @('src\injector.c') ('/OUT:' + (Join-Path $out 'AMAudioBridgeInjector.exe') +
    ' ole32.lib uuid.lib advapi32.lib')

Copy-Item (Join-Path $root 'AMAudioBridge.ini') (Join-Path $out 'AMAudioBridge.ini') -Force
Write-Host "built $out"
