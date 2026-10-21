@echo off
setlocal
set "ROOT=%~dp0"
start "" /min powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%ROOT%watch-bridge.ps1"
start "" explorer.exe "shell:AppsFolder\AppleInc.AppleMusicWin_nzyj5cx40ttqa!App"
endlocal
