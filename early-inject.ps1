$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$injector = Join-Path $root 'bin\x64\Release\probe2\AMAudioBridgeInjector.exe'
$logger = Join-Path $root 'bin\x64\Release\probe2\AMCoreAudioProbe.dll'
$bridge = Join-Path $root 'bin\x64\Release\dev5\AMAudioBridge.dll'
$log = Join-Path $root 'bin\x64\Release\early-inject2.log'

"waiting for AMPLibraryAgent $(Get-Date -Format o)" | Out-File $log
for ($i = 0; $i -lt 1000; $i++) {
    $p = Get-Process AMPLibraryAgent -ErrorAction SilentlyContinue
    if ($p) {
        "found pid $($p.Id)" | Out-File $log -Append
        & $injector --dll $logger --pid $p.Id --force *>> $log
        & $injector --dll $bridge --pid $p.Id *>> $log
        "done $(Get-Date -Format o)" | Out-File $log -Append
        break
    }
    Start-Sleep -Milliseconds 20
}
