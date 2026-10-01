# BioShock VR (left-handed edition) - installer.
# Run Install.bat (double-click). Finds BioShock Remastered, copies the mod in,
# backs up any other xinput1_3.dll it would replace, and asks for your hand setup.
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

function Pick-Folder {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "Select your BioShock Remastered folder"
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        return $null
    } catch {
        return (Read-Host "Paste the path to your BioShock Remastered folder")
    }
}

$version = "?"
$vf = Join-Path $PSScriptRoot "VERSION.txt"
if (Test-Path $vf) { $version = (Get-Content $vf -Raw).Trim() }
Write-Host ""
Write-Host "BioShock VR - left-handed edition $version" -ForegroundColor Cyan
Write-Host "Installer for BioShock Remastered" -ForegroundColor Cyan
Write-Host ""

$files = @("xinput1_3.dll", "bioshockvr.dll", "bvr_steamvr32.dll", "openvr_api.dll")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $PSScriptRoot $f))) {
        throw "$f is missing next to the installer. Unzip the whole release first, then run Install.bat from the unzipped folder."
    }
}

# 1. The game folder.
$game = Resolve-BioshockFolder $GamePath
if (-not $game) {
    $found = Find-BioshockFolder
    if ($found) {
        Write-Host "Found BioShock Remastered:"
        Write-Host "  $found"
        if (Ask "Install here?" $true) { $game = $found }
    } else {
        Write-Host "Couldn't find BioShock Remastered in your Steam libraries."
    }
}
while (-not $game) {
    Write-Host "Pick the BioShock Remastered folder (the one with Build\Final inside)."
    $picked = Pick-Folder
    if (-not $picked) { Write-Host "Cancelled - nothing was installed."; exit 1 }
    $game = Resolve-BioshockFolder $picked
    if (-not $game) { Write-Host "BioshockHD.exe isn't in that folder or its Build\Final. Try again." -ForegroundColor Yellow }
}

# 2. The game must be closed (its DLLs are locked while it runs).
while (Get-Process -Name "BioshockHD" -ErrorAction SilentlyContinue) {
    Write-Host "BioShock Remastered is running. Close it, then press Enter." -ForegroundColor Yellow
    Read-Host | Out-Null
}

# 3. Back up another mod's xinput1_3.dll (e.g. the head-tracking mod) once.
$existing = Join-Path $game "xinput1_3.dll"
$backup = Join-Path $game "xinput1_3.dll.bvr-backup"
$ours = Test-Path (Join-Path $game "bioshockvr.dll")
if ((Test-Path $existing) -and -not $ours -and -not (Test-Path $backup)) {
    Copy-Item $existing $backup
    Write-Host "Another mod's xinput1_3.dll was there (often the head-tracking mod). It's saved as"
    Write-Host "xinput1_3.dll.bvr-backup - the two mods can't run together. Uninstall.bat puts it back."
}

# 4. Copy the mod.
foreach ($f in $files) { Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $game $f) -Force }
Set-Content -Path (Join-Path $game "bioshockvr-installed.txt") -Encoding ASCII -Value @(
    "BioShock VR left-handed edition $version",
    "installed $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    "files: $($files -join ', ')")
Write-Host ""
Write-Host "Installed into $game" -ForegroundColor Green

# 5. Hand setup (saved where the mod keeps its settings).
$data = Join-Path $env:LOCALAPPDATA "BioshockVR"
$hand = Join-Path $data "handedness.ini"
Write-Host ""
if (Test-Path $hand) {
    Write-Host "Keeping your existing hand setup (change it any time in F10 -> Input)."
} else {
    $left = Ask "Are you left-handed? (gun in your left hand, plasmids in your right)" $false
    $swap = $false
    if ($left) { $swap = Ask "Walk with the RIGHT stick and turn with the left?" $false }
    New-Item -ItemType Directory -Path $data -Force | Out-Null
    Set-Content -Path $hand -Encoding ASCII -Value @(
        "# BioShock VR - hand setup (saved automatically)",
        "leftHanded=$([int]$left)",
        "swapSticks=$([int]$swap)",
        "mirrorViewmodel=$([int]$left)")
    if ($left) { Write-Host "Left-handed set up, with a real left hand on the gun." }
    else { Write-Host "Right-handed set up." }
}

Write-Host ""
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  1. Start your VR runtime (Virtual Desktop with VDXR, Quest Link, or SteamVR)."
Write-Host "  2. Launch BioShock Remastered from Steam. VR starts by itself."
Write-Host "  3. First time only: press F10 -> VR camera -> Render resolution, pick 2560 x 2560,"
Write-Host "     press 'Write to Bioshock.ini' and restart the game (a square image is sharper"
Write-Host "     and faster in a headset)."
Write-Host "  README.txt has the F10 guide. Uninstall.bat removes the mod."
Write-Host ""
