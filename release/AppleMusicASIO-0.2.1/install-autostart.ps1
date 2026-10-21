$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$watcher = Join-Path $root 'watch-bridge.ps1'
if (-not (Test-Path $watcher)) {
    throw "watch-bridge.ps1 not found next to this script"
}

$startup = [Environment]::GetFolderPath('Startup')
$shortcutPath = Join-Path $startup 'AppleMusicASIO Watcher.lnk'
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = (Get-Command powershell.exe).Source
$shortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$watcher`""
$shortcut.WorkingDirectory = $root
$shortcut.WindowStyle = 7
$shortcut.Description = 'Inject the Apple Music ASIO bridge when AMPLibraryAgent starts'
$shortcut.Save()

Write-Host "Created $shortcutPath"
Write-Host "Remove that shortcut to disable autostart."
