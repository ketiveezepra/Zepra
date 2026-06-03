<#
.SYNOPSIS
    Zepra Browser Uninstaller for Windows
.DESCRIPTION
    Removes Zepra Browser, shortcuts, and registry entries.
.NOTES
    Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
#>

#Requires -Version 5.1

param(
    [switch]$Silent
)

$Script:AppName       = "Zepra Browser"
$Script:UninstallGuid = "{7E2F9A4B-3D1C-4E8F-B5A6-9C0D2E1F3A4B}"
$Script:InstallDir    = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Elevation check
function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    $arguments = "-ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Definition)`""
    if ($Silent) { $arguments += " -Silent" }
    Start-Process powershell.exe -ArgumentList $arguments -Verb RunAs
    exit
}

# Confirmation
if (-not $Silent) {
    Add-Type -AssemblyName PresentationFramework
    $result = [System.Windows.MessageBox]::Show(
        "Are you sure you want to uninstall $($Script:AppName)?`n`nThis will remove all program files from:`n$($Script:InstallDir)",
        "Uninstall $Script:AppName",
        [System.Windows.MessageBoxButton]::YesNo,
        [System.Windows.MessageBoxImage]::Question)

    if ($result -ne [System.Windows.MessageBoxResult]::Yes) {
        Write-Host "Uninstall cancelled." -ForegroundColor Yellow
        exit 0
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Uninstalling $Script:AppName" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Kill running processes
Write-Host "[1/5] Stopping running processes..." -ForegroundColor White
Get-Process -Name "zepra_browser" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "zepra-repl" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# Step 2: Remove desktop shortcut
Write-Host "[2/5] Removing shortcuts..." -ForegroundColor White
$desktopPath = [Environment]::GetFolderPath("CommonDesktopDirectory")
$desktopLink = "$desktopPath\Zepra Browser (Beta).lnk"
if (Test-Path $desktopLink) {
    Remove-Item $desktopLink -Force
    Write-Host "       Removed: desktop shortcut" -ForegroundColor DarkGray
}

# Step 3: Remove Start Menu entries
$startMenuPath = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\KetiveeAI"
if (Test-Path $startMenuPath) {
    Remove-Item $startMenuPath -Recurse -Force
    Write-Host "       Removed: Start Menu entries" -ForegroundColor DarkGray
}

# Step 4: Remove registry entries
Write-Host "[3/5] Removing registry entries..." -ForegroundColor White
$regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($Script:UninstallGuid)"
if (Test-Path $regPath) {
    Remove-Item $regPath -Force
    Write-Host "       Removed: Add/Remove Programs entry" -ForegroundColor DarkGray
}

# Step 5: Remove program files
Write-Host "[4/5] Removing program files..." -ForegroundColor White
# Safety check — don't delete system directories
if ($Script:InstallDir -match "KetiveeAI|ZepraBrowser|zepra") {
    # Remove everything except the uninstaller itself (we're running from it)
    Get-ChildItem -Path $Script:InstallDir -Exclude "Uninstall-ZepraBrowser.ps1" |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "       Removed: program files" -ForegroundColor DarkGray
} else {
    Write-Host "       SKIP: Install directory doesn't look safe to delete" -ForegroundColor Yellow
}

# Step 6: Schedule self-deletion
Write-Host "[5/5] Cleaning up..." -ForegroundColor White
$cleanupCmd = "Start-Sleep -Seconds 2; Remove-Item '$Script:InstallDir' -Recurse -Force -ErrorAction SilentlyContinue"
Start-Process powershell.exe -ArgumentList "-WindowStyle Hidden -Command $cleanupCmd" -WindowStyle Hidden

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  Uninstall Complete!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ""

if (-not $Silent) {
    [System.Windows.MessageBox]::Show(
        "$Script:AppName has been successfully uninstalled.",
        "Uninstall Complete",
        [System.Windows.MessageBoxButton]::OK,
        [System.Windows.MessageBoxImage]::Information) | Out-Null
}
