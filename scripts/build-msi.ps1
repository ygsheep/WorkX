# ============================================================
# build-msi.ps1 - Build Workx MSI installer using WiX Toolset v3
#
# Steps:
#   1. Locate WiX Toolset (candle.exe / light.exe / heat.exe)
#   2. Read version from cmake/version.cmake
#   3. Stage build output (exe + dlls + tools/) into scripts/staging
#      (adds VC runtime DLLs as private DLLs if available on this machine)
#   4. Harvest staged files with heat.exe -> components.wxs
#   5. Compile with candle.exe
#   6. Link with light.exe -> dist/workx-<version>-x64.msi
#   7. Verify MSI structure via Windows Installer COM
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build-msi.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build-msi.ps1 -BuildDir build\bin\Release
# ============================================================
param(
    [string]$BuildDir = "$PSScriptRoot\..\build\bin\Release",
    [string]$OutDir   = "$PSScriptRoot\..\dist",
    [string]$WixBin   = ""
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$StagingDir = Join-Path $PSScriptRoot "staging"
$ComponentsWxs = Join-Path $PSScriptRoot "components.wxs"
$Wxs = Join-Path $PSScriptRoot "workx.wxs"
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
$heat   = Join-Path $WixBin "heat.exe"
Write-Host "[1/7] WiX Toolset: $WixBin"

# ---------- 2. Read version ----------
$verCmake = Get-Content (Join-Path $Root "cmake\version.cmake") -Raw -Encoding UTF8
$version = ""
if ($verCmake -match 'set\(WORKX_VERSION_(?:MAJOR|MINOR|PATCH)\s+(\d+)\)') { }
if ($verCmake -match 'set\(WORKX_VERSION_MAJOR\s+(\d+)\)') { $major = $Matches[1] }
if ($verCmake -match 'set\(WORKX_VERSION_MINOR\s+(\d+)\)') { $minor = $Matches[1] }
if ($verCmake -match 'set\(WORKX_VERSION_PATCH\s+(\d+)\)') { $patch = $Matches[1] }
if (-not $major -or -not $minor -or -not $patch) { throw "Cannot parse version from cmake/version.cmake" }
$version = "$major.$minor.$patch"
Write-Host "[2/7] Version: $version"

# ---------- 3. Stage build output ----------
if (-not (Test-Path "$BuildDir\workx.exe")) {
    throw "Build output not found at $BuildDir (expected workx.exe). Build Release first."
}
if (Test-Path $StagingDir) { Remove-Item $StagingDir -Recurse -Force }
New-Item -ItemType Directory -Path $StagingDir -Force | Out-Null

# exe + dlls (skip .pdb, .lib, .exp)
Get-ChildItem $BuildDir -File | Where-Object {
    $_.Extension -in @('.exe', '.dll')
} | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $StagingDir $_.Name)
    Write-Host "  staged: $($_.Name)"
}

# tools/ subdirectory (bundled rg / jq)
$toolsSrc = Join-Path $BuildDir "tools"
if (Test-Path $toolsSrc) {
    New-Item -ItemType Directory -Path (Join-Path $StagingDir "tools") -Force | Out-Null
    Get-ChildItem $toolsSrc -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path (Join-Path $StagingDir "tools") $_.Name)
        Write-Host "  staged: tools\$($_.Name)"
    }
}

# VC runtime DLLs as private DLLs (ensure target machines can run without VC redist)
$vcDlls = @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
            "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_codecvt_ids.dll",
            "msvcp140_atomic_wait.dll")
foreach ($dll in $vcDlls) {
    $sysDll = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path $sysDll) {
        Copy-Item $sysDll (Join-Path $StagingDir $dll)
        Write-Host "  staged: $dll (VC runtime)"
    }
}
Write-Host "[3/7] Staging done: $StagingDir"

# ---------- 4. Harvest with heat.exe ----------
& $heat dir $StagingDir `
    -cg WorkxComponents `
    -dr INSTALLDIR `
    -srd `
    -gg `
    -sfrag `
    -sw5150 `
    -out $ComponentsWxs
if ($LASTEXITCODE -ne 0) { throw "heat.exe failed (exit $LASTEXITCODE)" }

# heat 3.x 无 -arch 参数：给所有组件注入 Win64="yes"（64 位安装目录必须）
$heatContent = Get-Content $ComponentsWxs -Raw
$heatContent = $heatContent -replace '<Component ', '<Component Win64="yes" '
Set-Content -Path $ComponentsWxs -Value $heatContent -Encoding UTF8
Write-Host "[4/7] components.wxs generated (Win64 marked)"

# ---------- 5. Compile with candle.exe ----------
$wixObjDir = Join-Path $PSScriptRoot "wixobj"
if (-not (Test-Path $wixObjDir)) { New-Item -ItemType Directory -Path $wixObjDir -Force | Out-Null }
$candleArgs = @(
    $Wxs, $ComponentsWxs,
    "-dWorkxVersion=$version",
    "-dIconPath=$IconPath",
    "-out",
    (Join-Path $wixObjDir "")
)
& $candle @candleArgs
if ($LASTEXITCODE -ne 0) { throw "candle.exe failed (exit $LASTEXITCODE)" }
Write-Host "[5/7] .wixobj compiled"

# ---------- 6. Link with light.exe ----------
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$msiPath = Join-Path $OutDir "workx-$version-x64.msi"

# 中文 UI 本地化文件（WiX 自带，位于 SDK\wixui）
# 优先从当前 WixBin 推断（CI 解压目录适用），再回退到标准安装路径
$wixLang = ""
$langCands = @()
if ($WixBin) {
    $langCands += (Join-Path $WixBin "SDK\wixui\WixUI_zh-CN.wxl")
}
$langCands += @(
    "C:\Program Files (x86)\WiX Toolset v3.14\SDK\wixui\WixUI_zh-CN.wxl",
    "C:\Program Files (x86)\WiX Toolset v3.13\SDK\wixui\WixUI_zh-CN.wxl"
)
foreach ($cand in $langCands) {
    if (Test-Path $cand) { $wixLang = $cand; break }
}
if (-not $wixLang) {
    Write-Warning "WixUI_zh-CN.wxl not found; falling back to English UI"
}

$lightArgs = @(
    (Join-Path $wixObjDir "workx.wixobj"),
    (Join-Path $wixObjDir "components.wixobj"),
    "-b", $StagingDir,
    "-b", $PSScriptRoot,
    "-ext", "WixUIExtension",
    "-o", $msiPath,
    # light 默认在 MSI 旁生成 .wixpdb 调试符号；重定向到构建目录，避免混入发布
    "-pdbout", (Join-Path $wixObjDir "workx-$version-x64.wixpdb")
)
if ($wixLang) {
    $lightArgs += @("-cultures:zh-CN", "-loc", $wixLang)
}
& $light @lightArgs
if ($LASTEXITCODE -ne 0) { throw "light.exe failed (exit $LASTEXITCODE)" }
$sizeMB = [math]::Round((Get-Item $msiPath).Length / 1MB, 1)
Write-Host "[6/7] MSI built: $msiPath ($sizeMB MB)"

# ---------- 7. Verify via Windows Installer COM ----------
Write-Host "[7/7] Verifying MSI..."
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase($msiPath, 0)

function Query-Msi($db, [string]$sql, [int]$maxCols = 8) {
    # MSI 记录对象是缓冲复用的，Fetch 后必须立即提取字段值
    $view = $db.OpenView($sql)
    $view.Execute()
    $rows = New-Object System.Collections.ArrayList
    while ($null -ne ($rec = $view.Fetch())) {
        $vals = New-Object System.Collections.ArrayList
        for ($i = 1; $i -le $maxCols; $i++) {
            try {
                $v = $rec.StringData($i)
                if ($null -ne $v) { [void]$vals.Add($v) }
            } catch { break }
        }
        [void]$rows.Add($vals.ToArray())
    }
    return $rows.ToArray()
}

# MSI 只读 COM 模式下 SELECT * 最稳定；列索引用字面量访问
$si = $db.SummaryInformation(0)
$subject = $si.Property(3)   # Subject = 产品描述
$author  = $si.Property(4)   # Author  = 制造商
Write-Host "  Summary: Subject='$subject' Author='$author'"

# File 表列序: File(1), Component_(2), FileName(3), ...
$files = Query-Msi $db 'SELECT * FROM `File`'
Write-Host "  File count: $($files.Count)"
$fileNames = @()
foreach ($f in $files) {
    # MSI COM Record 被 Fetch() 复用时会偶发返回空/null 行，跳过以保稳健
    if ($null -eq $f -or $f.Count -lt 3) { continue }
    $fn = $f[2]
    if ($fn) { $fileNames += $fn; Write-Host "    File: $fn" }
}

# Environment 表列序: Id(1), Name(2), Value(3), Component_(4)
$envRows = Query-Msi $db 'SELECT * FROM `Environment`'
Write-Host "  Environment entries:"
foreach ($e in $envRows) {
    if ($null -eq $e -or $e.Count -lt 4) { continue }
    Write-Host "    Name=$($e[1]) Value=$($e[2]) Component=$($e[3])"
}

# Validation summary
$ok = $true
if ($subject -notlike '*Workx*') {
    $ok = $false; Write-Host "  [FAIL] Summary Subject does not mention Workx"
}
if (-not ($fileNames | Where-Object { $_ -match 'workx.*\.exe' })) {
    $ok = $false; Write-Host "  [FAIL] workx.exe not in File table"
}
$pathEntry = $envRows | Where-Object {
    $null -ne $_ -and $_.Count -ge 3 -and $_[1] -match 'PATH' -and $_[2] -match '\[INSTALLDIR\]'
}
if (-not $pathEntry) {
    $ok = $false; Write-Host "  [FAIL] no Environment (PATH += INSTALLDIR) entry"
}
if ($ok) {
    Write-Host ""
    Write-Host "=============================================="
    Write-Host "  MSI VERIFIED OK: $msiPath"
    Write-Host "  Version: $version | Size: $sizeMB MB"
    Write-Host "=============================================="
} else {
    Write-Host "MSI verification reported problems above."
    exit 1
}
