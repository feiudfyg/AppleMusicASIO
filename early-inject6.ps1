$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$injector = Join-Path $root 'bin\x64\Release\dev6\AMAudioBridgeInjector.exe'
$bridge = Join-Path $root 'bin\x64\Release\dev6\AMAudioBridge.dll'
$log = Join-Path $root 'bin\x64\Release\early-inject6.log'

"waiting for AMPLibraryAgent $(Get-Date -Format o)" | Out-File $log
for ($i = 0; $i -lt 1000; $i++) {
    $p = Get-Process AMPLibraryAgent -ErrorAction SilentlyContinue
    if ($p) {
        "found pid $($p.Id)" | Out-File $log -Append
        & $injector --dll $bridge --pid $p.Id *>> $log
        "done $(Get-Date -Format o)" | Out-File $log -Append
        break
    }
    Start-Sleep -Milliseconds 20
}
