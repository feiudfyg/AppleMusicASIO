param(
    [string]$BridgeDir = ''
)

if (-not $BridgeDir) {
    $repoBuild = Join-Path $PSScriptRoot 'bin\x64\Release'
    if (Test-Path (Join-Path $repoBuild 'AMAudioBridge.dll')) {
        $BridgeDir = $repoBuild
    } else {
        $BridgeDir = $PSScriptRoot
    }
}

$ErrorActionPreference = 'SilentlyContinue'
$injector = Join-Path $BridgeDir 'AMAudioBridgeInjector.exe'
$bridge = Join-Path $BridgeDir 'AMAudioBridge.dll'
$persist = Join-Path $BridgeDir 'AMPrefsPersist.dll'
$log = Join-Path $BridgeDir 'AMAudioBridgeWatch.log'

$mutex = New-Object System.Threading.Mutex($false, 'Local\AMAudioBridgeWatcher')
if (-not $mutex.WaitOne(0)) {
    exit 0
}

"watcher start $(Get-Date -Format o) bridge=$bridge" | Out-File $log -Append
$fixer = Join-Path $PSScriptRoot 'fix-preferences-junction.ps1'
if (Test-Path $fixer) {
    & $fixer -Quiet *>> $log
}
$attempts = @{}

while ($true) {
    $agents = @(Get-Process AMPLibraryAgent -ErrorAction SilentlyContinue)
    $alive = @{}
    foreach ($agent in $agents) {
        $agentPid = $agent.Id
        $alive[$agentPid] = $true
        if (-not $attempts.ContainsKey($agentPid)) {
            $attempts[$agentPid] = 0
        }
        if ($attempts[$agentPid] -ge 5) {
            continue
        }

        $output = (& $injector --dll $bridge --pid $agentPid 2>&1 | Out-String).Trim()
        $code = $LASTEXITCODE
        if (Test-Path $persist) {
            $persistOutput = (& $injector --dll $persist --pid $agentPid --force 2>&1 | Out-String).Trim()
            $output = "$output`n$persistOutput"
        }
        $attempts[$agentPid]++
        "$(Get-Date -Format o) pid=$agentPid attempt=$($attempts[$agentPid]) exit=$code $output" |
            Out-File $log -Append

        if ($output -match 'already loaded|LoadLibraryW') {
            $attempts[$agentPid] = 99
        }
    }

    foreach ($known in @($attempts.Keys)) {
        if (-not $alive.ContainsKey($known)) {
            $attempts.Remove($known)
        }
    }

    Start-Sleep -Milliseconds 500
}
