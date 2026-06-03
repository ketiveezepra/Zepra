<#
.SYNOPSIS
    Package Zepra Browser into a distributable installer bundle for Windows.
.DESCRIPTION
    Collects built binaries, resources, legal docs, and installer scripts
    into a single distributable .zip archive.
.NOTES
    Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
#>

param(
    [string]$BuildDir    = "build-win\bin",
    [string]$OutputDir   = "dist",
    [string]$Channel     = "beta"
)

$ErrorActionPreference = "Stop"

$Version  = "0.1.0"
$FullVer  = "$Version-$Channel"
$PkgName  = "ZepraBrowser-$FullVer-win64"
$RootDir  = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition))

Write-Host ""
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "  Zepra Browser — Windows Package Builder" -ForegroundColor Cyan
Write-Host "  Version: $FullVer" -ForegroundColor DarkCyan
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host ""

# Resolve paths
$binDir      = Join-Path $RootDir $BuildDir
$resDir      = Join-Path $RootDir "resources"
$legalDir    = Join-Path $RootDir "legal"
$installerDir= Join-Path $RootDir "installer\windows"
$outDir      = Join-Path $RootDir $OutputDir
$stageDir    = Join-Path $outDir $PkgName

# Verify build exists
if (-not (Test-Path (Join-Path $binDir "zepra_browser.exe"))) {
    Write-Host "[ERROR] zepra_browser.exe not found in $binDir" -ForegroundColor Red
    Write-Host "        Build the project first: cmake --build build-win" -ForegroundColor Yellow
    exit 1
}

# Clean and create staging directory
Write-Host "[1/7] Creating staging directory..." -ForegroundColor White
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

# Copy executables
Write-Host "[2/7] Copying executables..." -ForegroundColor White
$exes = @("zepra_browser.exe", "zepra-repl.exe", "zepra-dump-bytecode.exe")
foreach ($exe in $exes) {
    $src = Join-Path $binDir $exe
    if (Test-Path $src) {
        Copy-Item $src -Destination $stageDir -Force
        Write-Host "       + $exe" -ForegroundColor DarkGray
    }
}

# Copy resources
Write-Host "[3/7] Copying resources..." -ForegroundColor White
New-Item -ItemType Directory -Path "$stageDir\resources\icons" -Force | Out-Null
New-Item -ItemType Directory -Path "$stageDir\resources\web" -Force | Out-Null
if (Test-Path "$resDir\icons") {
    Copy-Item "$resDir\icons\*" -Destination "$stageDir\resources\icons" -Recurse -Force -ErrorAction SilentlyContinue
}
if (Test-Path "$resDir\web") {
    Copy-Item "$resDir\web\*" -Destination "$stageDir\resources\web" -Recurse -Force -ErrorAction SilentlyContinue
}

# Copy legal documents
Write-Host "[4/7] Copying legal documents..." -ForegroundColor White
New-Item -ItemType Directory -Path "$stageDir\legal" -Force | Out-Null
if (Test-Path $legalDir) {
    Copy-Item "$legalDir\*" -Destination "$stageDir\legal" -Recurse -Force
}
@("LICENSE.md", "RELEASE_NOTES.md", "README.md") | ForEach-Object {
    $f = Join-Path $RootDir $_
    if (Test-Path $f) { Copy-Item $f -Destination $stageDir -Force }
}

# Copy installer scripts
Write-Host "[5/7] Copying installer scripts..." -ForegroundColor White
Copy-Item "$installerDir\Install-ZepraBrowser.ps1" -Destination $stageDir -Force
Copy-Item "$installerDir\Uninstall-ZepraBrowser.ps1" -Destination $stageDir -Force

# Create quick-start launcher
Write-Host "[6/7] Creating launcher..." -ForegroundColor White
$launcherContent = @"
@echo off
echo.
echo  Zepra Browser v$FullVer
echo  ========================
echo.
echo  To INSTALL: Right-click Install-ZepraBrowser.ps1 and select
echo             "Run with PowerShell"
echo.
echo  To RUN directly: Launch zepra_browser.exe
echo.
pause
"@
Set-Content -Path "$stageDir\START_HERE.bat" -Value $launcherContent

# Create .zip archive
Write-Host "[7/7] Creating archive..." -ForegroundColor White
$zipPath = Join-Path $outDir "$PkgName.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -CompressionLevel Optimal
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)

Write-Host ""
Write-Host "=============================================" -ForegroundColor Green
Write-Host "  Package created successfully!" -ForegroundColor Green
Write-Host "=============================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Archive: $zipPath" -ForegroundColor White
Write-Host "  Size:    $zipSize MB" -ForegroundColor White
Write-Host "  Stage:   $stageDir" -ForegroundColor DarkGray
Write-Host ""
