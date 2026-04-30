#Requires -RunAsAdministrator
# This OBS install only scans C:\Program Files\obs-studio\obs-plugins\64bit\.
$src = "C:\Users\griff\dev\obs-audio-cue\build\Release\obs-audio-cue.dll"
$dst = "C:\Program Files\obs-studio\obs-plugins\64bit\obs-audio-cue.dll"

# OBS holds the DLL open while running.
$obs = Get-Process obs64 -ErrorAction SilentlyContinue
if ($obs) {
    Write-Host "Killing obs64.exe..."
    Stop-Process -Name obs64 -Force
    Start-Sleep -Milliseconds 500
}

Copy-Item -Path $src -Destination $dst -Force
Write-Host "Installed: $dst"
