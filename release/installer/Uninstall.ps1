# BioShock VR (left-handed edition) - uninstaller. Run Uninstall.bat.
# Removes the mod's files from the game folder, restores a backed-up
# xinput1_3.dll, and optionally deletes your saved VR settings.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([string]$GamePath = "")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find-bioshock.ps1")

function Ask([string]$question, [bool]$default) {
    $hint = if ($default) { "[Y/n]" } else { "[y/N]" }
    while ($true) {
        $a = (Read-Host "$question $hint").Trim().ToLower()
        if ($a -eq "") { return $default }
        if ($a -in @("y", "yes")) { return $true }
        if ($a -in @("n", "no")) { return $false }
    }
}

$game = Resolve-BioshockFolder $GamePath
if (-not $game) { $game = Find-BioshockFolder }
if (-not $game) { $game = Resolve-BioshockFolder (Read-Host "Paste the path to your BioShock Remastered folder") }
if (-not $game) { Write-Host "BioshockHD.exe not found - nothing removed."; exit 1 }

while (Get-Process -Name "BioshockHD" -ErrorAction SilentlyContinue) {
    Write-Host "BioShock Remastered is running. Close it, then press Enter." -ForegroundColor Yellow
    Read-Host | Out-Null
}

Write-Host "Removing BioShock VR from $game"
foreach ($f in @("xinput1_3.dll", "bioshockvr.dll", "bvr_steamvr32.dll", "openvr_api.dll",
                 "bioshockvr-installed.txt")) {
    $p = Join-Path $game $f
    if (Test-Path $p) { Remove-Item $p -Force }
}
$backup = Join-Path $game "xinput1_3.dll.bvr-backup"
if (Test-Path $backup) {
    Move-Item $backup (Join-Path $game "xinput1_3.dll") -Force
    Write-Host "Restored the xinput1_3.dll that was there before."
}

$data = Join-Path $env:LOCALAPPDATA "BioshockVR"
if ((Test-Path $data) -and (Ask "Also delete your saved VR settings and logs ($data)?" $false)) {
    Remove-Item $data -Recurse -Force
    Write-Host "Settings deleted."
}
Write-Host "Done." -ForegroundColor Green
