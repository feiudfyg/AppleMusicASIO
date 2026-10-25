param([string]$OutputDir = 'bin\x64\Release\comshim')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent (Split-Path -Parent $root)
$out = Join-Path $repo $OutputDir
$obj = Join-Path $repo (Join-Path 'obj' $OutputDir)
New-Item -ItemType Directory -Force $out | Out-Null
New-Item -ItemType Directory -Force $obj | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path $vcvars)) { throw "vcvars64.bat not found" }

$common = @(
    '/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4', '/utf-8', '/guard:cf',
    '/I', $root, '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
    '/DUNICODE', '/D_UNICODE'
)

function Build-Target([string]$name, [string[]]$sources, [string[]]$defines, [string]$link) {
    $dir = Join-Path $obj $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    $args = $common + $defines + @('/Fo' + $dir + '\') + $sources + @('/link') + ($link -split ' ')
    $cmd = "`"$vcvars`" >nul && cl " + (($args | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ')
    Write-Host "building $name"
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "build failed: $name" }
}

$src = Join-Path $root 'forwarder.cpp'
$commonSrc = @($src, (Join-Path $root 'comshim_common.h'))

Build-Target 'TestServer' @((Join-Path $root 'test_server.cpp')) @() (
    '/SUBSYSTEM:CONSOLE /OUT:' + (Join-Path $out 'TestServer.exe') + ' ole32.lib advapi32.lib')
Build-Target 'TestClient' @((Join-Path $root 'test_client.cpp')) @() (
    '/SUBSYSTEM:CONSOLE /OUT:' + (Join-Path $out 'TestClient.exe') + ' ole32.lib advapi32.lib')

Build-Target 'TestHandler' @($src) @('/DBUILD_HANDLER') (
    '/DLL /OUT:' + (Join-Path $out 'TestHandler.dll') +
    ' /EXPORT:DllGetClassObject /EXPORT:DllCanUnloadNow ole32.lib advapi32.lib')
Build-Target 'TestShim' @($src) @('/DBUILD_SHIM') (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'TestShim.exe') + ' ole32.lib advapi32.lib')

Build-Target 'AppleHandler' @($src) @('/DBUILD_HANDLER', '/DTARGET_APPLE') (
    '/DLL /OUT:' + (Join-Path $out 'AppleHandler.dll') +
    ' /EXPORT:DllGetClassObject /EXPORT:DllCanUnloadNow ole32.lib advapi32.lib')
Build-Target 'AppleShim' @($src) @('/DBUILD_SHIM', '/DTARGET_APPLE') (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'AppleShim.exe') + ' ole32.lib advapi32.lib')

Build-Target 'AMExeProbe' @((Join-Path $root 'probe.cpp')) @() (
    '/DLL /OUT:' + (Join-Path $out 'AMExeProbe.dll') + ' ole32.lib advapi32.lib')
Build-Target 'PackagedProbe' @((Join-Path $root 'packaged_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'PackagedProbe.exe') + ' ole32.lib advapi32.lib')
Build-Target 'ProxyForward' @((Join-Path $root 'proxy_forward.cpp')) @() (
    '/DLL /OUT:' + (Join-Path $out 'ProxyForward.dll') +
    ' /EXPORT:DllGetClassObject /EXPORT:DllCanUnloadNow ole32.lib advapi32.lib')
Build-Target 'AgentActivationProbe' @((Join-Path $root 'agent_activation_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'AgentActivationProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'AgentStartProbe' @((Join-Path $root 'agent_start_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'AgentStartProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'RegOverrideProbe' @((Join-Path $root 'reg_override_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'RegOverrideProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'ClientProbe' @((Join-Path $root 'client_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'ClientProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'RealRestoreProbe' @((Join-Path $root 'real_restore_probe.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'RealRestoreProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'FactoryProbe' @((Join-Path $root 'real_restore_probe.cpp')) @('/DTEST_FACTORY') (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'FactoryProbe.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'ProxyClient' @((Join-Path $root 'proxy_client.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'ProxyClient.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'ProxyShim' @((Join-Path $root 'proxy_shim.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'ProxyShim.exe') +
    ' ole32.lib advapi32.lib shell32.lib')
Build-Target 'EarlyProxy' @((Join-Path $root 'early_proxy.cpp')) @() (
    '/DLL /OUT:' + (Join-Path $out 'EarlyProxy.dll') +
    ' /EXPORT:DllGetClassObject /EXPORT:DllCanUnloadNow ole32.lib advapi32.lib')
Build-Target 'PreStartHelper' @((Join-Path $root 'prestart_helper.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'PreStartHelper.exe') +
    ' ole32.lib advapi32.lib')
Build-Target 'PreStartShim' @((Join-Path $root 'prestart_shim.cpp')) @() (
    '/SUBSYSTEM:WINDOWS /OUT:' + (Join-Path $out 'PreStartShim.exe') +
    ' ole32.lib advapi32.lib')

Write-Host "built $out"
