# ============================================================
# build-bundle.ps1 - Build Workx setup.exe (WiX Burn Bootstrapper)
#
# The setup.exe embeds the Workx icon and shows a Chinese
# bootstrapper UI; it chains the workx-<version>-x64.msi package.
#
# Prerequisite: build-msi.ps1 has been run (dist/workx-<ver>-x64.msi exists)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File packaging\build-bundle.ps1
# ============================================================
param(
    [string]$OutDir = "$PSScriptRoot\..\dist",
    [string]$WixBin = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Wxs = Join-Path $PSScriptRoot "bundle.wxs"
$IconPath = Join-Path $Root "src\icon.ico"

# ---------- 1. Locate WiX Toolset ----------
if (-not $WixBin) {
    $candidates = @(
        "C:\Program Files (x86)\WiX Toolset v3.14\bin",
        "C:\Program Files (x86)\WiX Toolset v3.13\bin",
        "C:\Program Files (x86)\WiX Toolset v3.12\bin",
        "C:\Program Files (x86)\WiX Toolset v3.11\bin"
    )
    foreach ($c in $candidates) {
        if (Test-Path "$c\candle.exe") { $WixBin = $c; break }
    }
}
if (-not $WixBin -or -not (Test-Path "$WixBin\candle.exe")) {
    throw "WiX Toolset not found. Install via: winget install WiXToolset.WiXToolset"
}
$candle = Join-Path $WixBin "candle.exe"
$light  = Join-Path $WixBin "light.exe"
Write-Host "[1/4] WiX Toolset: $WixBin"

# ---------- 2. Read version ----------
$verCmake = Get-Content (Join-Path $Root "cmake\version.cmake") -Raw -Encoding UTF8
if ($verCmake -match 'set\(WORKX_VERSION_MAJOR\s+(\d+)\)') { $major = $Matches[1] }
if ($verCmake -match 'set\(WORKX_VERSION_MINOR\s+(\d+)\)') { $minor = $Matches[1] }
if ($verCmake -match 'set\(WORKX_VERSION_PATCH\s+(\d+)\)') { $patch = $Matches[1] }
if (-not $major -or -not $minor -or -not $patch) { throw "Cannot parse version from cmake/version.cmake" }
$version = "$major.$minor.$patch"
$msiName = "workx-$version-x64.msi"
$msiPath = Join-Path $OutDir $msiName
if (-not (Test-Path $msiPath)) {
    throw "MSI not found at $msiPath. Run build-msi.ps1 first."
}
Write-Host "[2/4] Version: $version (MSI: $msiName)"

# ---------- 3. Compile bundle.wxs ----------
$wixObjDir = Join-Path $PSScriptRoot "wixobj"
if (-not (Test-Path $wixObjDir)) { New-Item -ItemType Directory -Path $wixObjDir -Force | Out-Null }
$candleArgs = @(
    $Wxs,
    "-ext", "WixBalExtension",
    "-dWorkxVersion=$version",
    "-dIconPath=$IconPath",
    "-dMsiPath=$msiPath",
    "-out",
    (Join-Path $wixObjDir "")
)
& $candle @candleArgs
if ($LASTEXITCODE -ne 0) { throw "candle.exe failed (exit $LASTEXITCODE)" }
Write-Host "[3/4] bundle.wixobj compiled"

# ---------- 4. Link with light.exe ----------
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$exePath = Join-Path $OutDir "setup.exe"
$lightArgs = @(
    (Join-Path $wixObjDir "bundle.wixobj"),
    "-ext", "WixBalExtension",
    "-ext", "WixUtilExtension",
    "-b", $PSScriptRoot,
    "-b", $OutDir,
    "-o", $exePath
)
& $light @lightArgs
if ($LASTEXITCODE -ne 0) { throw "light.exe failed (exit $LASTEXITCODE)" }
$sizeMB = [math]::Round((Get-Item $exePath).Length / 1MB, 1)
Write-Host "[4/4] setup.exe built: $exePath ($sizeMB MB)"

# Verify EXE icon resource exists (group icon)
Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon($exePath)
if ($icon) {
    Write-Host "  Icon OK: $($icon.Width)x$($icon.Height)"
} else {
    Write-Host "  [WARN] no icon extracted from exe"
}

Write-Host ""
Write-Host "=============================================="
Write-Host "  Setup bundle built: $exePath"
Write-Host "  Version: $version | Size: $sizeMB MB"
Write-Host "  Contains: $msiName"
Write-Host "=============================================="
