# Finds the BioShock Remastered game folder (the one holding BioshockHD.exe).
# Shared by the release installer and tools\install.ps1.
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).

function Get-SteamLibraries {
    $roots = @()
    foreach ($key in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam",
                       "HKLM:\SOFTWARE\Valve\Steam")) {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($v in @($p.SteamPath, $p.InstallPath)) {
                if ($v) { $roots += ($v -replace '/', '\') }
            }
        } catch { }
    }
    $roots += "${env:ProgramFiles(x86)}\Steam"
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path $r)) { continue }
        $libs += $r
        # Every extra Steam library is listed in libraryfolders.vdf as "path" "X:\\..."
        $vdf = Join-Path $r "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    return $libs | Select-Object -Unique
}

# Returns the folder that contains BioshockHD.exe, or $null.
# Accepts either the game's root folder or Build\Final itself.
function Resolve-BioshockFolder([string]$path) {
    if (-not $path) { return $null }
    $path = $path.Trim('"').TrimEnd('\')
    foreach ($c in @($path, (Join-Path $path "Build\Final"))) {
        if (Test-Path (Join-Path $c "BioshockHD.exe")) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Find-BioshockFolder {
    foreach ($lib in Get-SteamLibraries) {
        $hit = Resolve-BioshockFolder (Join-Path $lib "steamapps\common\BioShock Remastered")
        if ($hit) { return $hit }
    }
    return $null
}
