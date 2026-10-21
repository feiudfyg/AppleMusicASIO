param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$pfn = 'AppleInc.AppleMusicWin_nzyj5cx40ttqa'
$log = Join-Path $PSScriptRoot 'AMAudioBridgePreferences.log'

function Note([string]$text) {
    $line = "$(Get-Date -Format o) $text"
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8 -ErrorAction SilentlyContinue
    if (-not $Quiet) { Write-Host $line }
}

try {
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $link = Join-Path $env:APPDATA 'Apple Computer\Preferences'
    $relative = "AppData\Local\Packages\$pfn\LocalCache\Roaming\Apple Computer\Preferences"

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalCache\Roaming\Apple Computer\Preferences")
    )
    $localCache = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalCache"
    if (Test-Path -LiteralPath $localCache) {
        $cacheItem = Get-Item -LiteralPath $localCache -Force
        if ($cacheItem.LinkType -eq 'Junction' -and $cacheItem.Target) {
            $resolved = @($cacheItem.Target)[0]
            if ($resolved) {
                $candidates = @((Join-Path $resolved 'Roaming\Apple Computer\Preferences')) + $candidates
            }
        }
    }
    foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
        if ($drive.Root) {
            $candidates += (Join-Path $drive.Root "WpSystem\$sid\$relative")
        }
    }

    $target = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $target) {
        $packageRoots = @(
            (Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalCache\Roaming\Apple Computer")
        )
        foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
            if ($drive.Root) {
                $packageRoots += (Join-Path $drive.Root "WpSystem\$sid\AppData\Local\Packages\$pfn\LocalCache\Roaming\Apple Computer")
            }
        }
        $packageRoot = $packageRoots | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if (-not $packageRoot) {
            Note "Apple Music package preferences not found; launch Apple Music once, then rerun."
            exit 1
        }
        $target = Join-Path $packageRoot 'Preferences'
        New-Item -ItemType Directory -Force -Path $target | Out-Null
    }

    New-Item -ItemType Directory -Force -Path (Split-Path $link -Parent) | Out-Null

    $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($item -and $item.LinkType -eq 'Junction') {
        $current = @($item.Target)[0]
        if ($current -and ($current.TrimEnd('\') -ieq $target.TrimEnd('\'))) {
            Note "OK: $link -> $target"
            exit 0
        }
        Note "Replacing stale junction: $current"
        cmd.exe /c rmdir "$link" | Out-Null
    }
    elseif ($item) {
        Note "Existing Preferences directory is real; merging into $target"
        Copy-Item -Path (Join-Path $link '*') -Destination $target -Recurse -Force -ErrorAction SilentlyContinue
        $backup = "$link.bak-$(Get-Date -Format yyyyMMddHHmmss)"
        Rename-Item -LiteralPath $link -NewName (Split-Path $backup -Leaf) -ErrorAction Stop
        Note "Backed up old directory to $backup"
    }

    $output = cmd.exe /c mklink /J "$link" "$target" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Note "junction creation failed: $output"
        exit 1
    }
    $check = Get-Item -LiteralPath $link -Force
    Note "Created junction: $link -> $($check.Target)"
    exit 0
}
catch {
    Note "fix failed: $($_.Exception.Message)"
    exit 1
}
