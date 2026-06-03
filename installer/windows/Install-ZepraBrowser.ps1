<#
.SYNOPSIS
    Zepra Browser v0.1.0-beta Installer for Windows
.DESCRIPTION
    Professional GUI installer with EULA acceptance, beta warning,
    and full install/uninstall support.
.NOTES
    Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
    Licensed under KPL-2.0.
#>

#Requires -Version 5.1

param(
    [switch]$Silent,
    [string]$InstallPath = "$env:ProgramFiles\KetiveeAI\ZepraBrowser"
)

# ============================================================================
# Constants
# ============================================================================
$Script:AppName        = "Zepra Browser"
$Script:AppVersion     = "0.1.0-beta"
$Script:Publisher      = "KetiveeAI"
$Script:Website        = "https://zepra.ketivee.com"
$Script:UninstallGuid  = "{7E2F9A4B-3D1C-4E8F-B5A6-9C0D2E1F3A4B}"
$Script:DefaultPath    = $InstallPath
$Script:ScriptDir      = Split-Path -Parent $MyInvocation.MyCommand.Definition

# ============================================================================
# Elevation Check
# ============================================================================
function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host "Requesting administrator privileges..." -ForegroundColor Yellow
    $arguments = "-ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Definition)`""
    if ($Silent) { $arguments += " -Silent" }
    if ($InstallPath -ne "$env:ProgramFiles\KetiveeAI\ZepraBrowser") {
        $arguments += " -InstallPath `"$InstallPath`""
    }
    Start-Process powershell.exe -ArgumentList $arguments -Verb RunAs
    exit
}

# ============================================================================
# EULA Text
# ============================================================================
$Script:EulaText = @"
KETIVEEAI - END USER LICENSE AGREEMENT (EULA)
Zepra Browser v0.1.0-beta | Effective Date: May 2026

================================================================
BETA SOFTWARE NOTICE
================================================================

This is a PRE-RELEASE BETA VERSION of Zepra Browser. It is
provided for testing and evaluation purposes. This software
may contain bugs, incomplete features, and may not perform
as expected. Do not rely on this software for critical tasks.

================================================================

1. ACCEPTANCE OF TERMS

By downloading, installing, copying, or otherwise using Zepra
Browser ("Software"), you ("User") agree to be bound by the
terms of this Agreement. If you do not agree, do not install.

2. LICENSE GRANT

KetiveeAI grants you a limited, non-exclusive, revocable
license to install and use the Software for personal,
educational, research, or commercial purposes under KPL-2.0.

3. BETA DISCLAIMER

THIS SOFTWARE IS A BETA RELEASE. You acknowledge that:
  - The Software is not feature-complete
  - It may contain errors, defects, and vulnerabilities
  - It may cause crashes or data loss
  - It should not be used for sensitive activities

4. PRIVACY

  - Anonymous crash reports may be collected
  - NO browsing history, passwords, or tracking data collected
  - The Software connects to ketivee.com for search/updates

5. WARRANTY DISCLAIMER

THE SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND.
KETIVEEAI DOES NOT WARRANT THAT THE SOFTWARE WILL BE ERROR-FREE.

6. LIMITATION OF LIABILITY

KETIVEEAI SHALL NOT BE LIABLE FOR ANY DAMAGES ARISING FROM USE
OF THE SOFTWARE. TOTAL LIABILITY SHALL NOT EXCEED ZERO DOLLARS.

7. GOVERNING LAW

This Agreement is governed by the laws of India.

================================================================
Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
Built in India. Independent by design.
"@

# ============================================================================
# WPF GUI Installer
# ============================================================================
function Show-InstallerGUI {
    Add-Type -AssemblyName PresentationFramework
    Add-Type -AssemblyName PresentationCore
    Add-Type -AssemblyName WindowsBase
    Add-Type -AssemblyName System.Windows.Forms

    $xaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Zepra Browser Setup" Height="580" Width="720"
        WindowStartupLocation="CenterScreen" ResizeMode="NoResize"
        Background="#0D0D1A" Foreground="White"
        FontFamily="Segoe UI">
    <Window.Resources>
        <Style x:Key="BetaBanner" TargetType="Border">
            <Setter Property="Background">
                <Setter.Value>
                    <LinearGradientBrush StartPoint="0,0" EndPoint="1,0">
                        <GradientStop Color="#FF6B1A" Offset="0"/>
                        <GradientStop Color="#FF3D00" Offset="1"/>
                    </LinearGradientBrush>
                </Setter.Value>
            </Setter>
            <Setter Property="Padding" Value="10,6"/>
            <Setter Property="CornerRadius" Value="0"/>
        </Style>
        <Style x:Key="AccentButton" TargetType="Button">
            <Setter Property="Background" Value="#6C3AED"/>
            <Setter Property="Foreground" Value="White"/>
            <Setter Property="FontSize" Value="14"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="Padding" Value="24,10"/>
            <Setter Property="BorderThickness" Value="0"/>
            <Setter Property="Cursor" Value="Hand"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border Background="{TemplateBinding Background}"
                                CornerRadius="6" Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center"
                                            VerticalAlignment="Center"/>
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#7C4DFF"/>
                </Trigger>
                <Trigger Property="IsEnabled" Value="False">
                    <Setter Property="Background" Value="#333355"/>
                    <Setter Property="Foreground" Value="#666688"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        <Style x:Key="SecondaryButton" TargetType="Button">
            <Setter Property="Background" Value="#1A1A2E"/>
            <Setter Property="Foreground" Value="#AAAACC"/>
            <Setter Property="FontSize" Value="13"/>
            <Setter Property="Padding" Value="20,8"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="BorderBrush" Value="#333355"/>
            <Setter Property="Cursor" Value="Hand"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="6" Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center"
                                            VerticalAlignment="Center"/>
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#252545"/>
                </Trigger>
            </Style.Triggers>
        </Style>
    </Window.Resources>

    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
            <RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>

        <!-- Beta Warning Banner -->
        <Border Grid.Row="0" Style="{StaticResource BetaBanner}">
            <TextBlock Text="⚠  BETA RELEASE — This is pre-release software for testing purposes"
                       Foreground="White" FontSize="12" FontWeight="SemiBold"
                       HorizontalAlignment="Center"/>
        </Border>

        <!-- Content Pages -->
        <TabControl x:Name="Pages" Grid.Row="1" Background="Transparent"
                    BorderThickness="0" Margin="0"
                    ItemContainerStyle="{x:Null}">
            <TabControl.ItemContainerStyle>
                <Style TargetType="TabItem">
                    <Setter Property="Visibility" Value="Collapsed"/>
                </Style>
            </TabControl.ItemContainerStyle>

            <!-- Page 0: Welcome -->
            <TabItem>
                <Grid Margin="40,20">
                    <StackPanel VerticalAlignment="Center">
                        <TextBlock Text="🌐" FontSize="64" HorizontalAlignment="Center"
                                   Margin="0,0,0,16"/>
                        <TextBlock Text="Zepra Browser" FontSize="36" FontWeight="Bold"
                                   HorizontalAlignment="Center" Foreground="#B794F6"/>
                        <TextBlock Text="v0.1.0-beta" FontSize="16"
                                   HorizontalAlignment="Center" Foreground="#888"
                                   Margin="0,4,0,20"/>
                        <Border Background="#1A1A2E" CornerRadius="8" Padding="20"
                                Margin="40,0" BorderBrush="#252545" BorderThickness="1">
                            <StackPanel>
                                <TextBlock TextWrapping="Wrap" Foreground="#CCCCEE"
                                           FontSize="13" LineHeight="22" TextAlignment="Center">
                                    Welcome to the first public beta of Zepra Browser —
                                    an independent web browser built from scratch by KetiveeAI.
                                    <LineBreak/><LineBreak/>
                                    This installer will guide you through the setup process.
                                    You will need to accept the End User License Agreement
                                    before proceeding.
                                </TextBlock>
                            </StackPanel>
                        </Border>
                        <TextBlock Text="Copyright © 2025-2026 KetiveeAI. Built in India."
                                   FontSize="11" Foreground="#555577"
                                   HorizontalAlignment="Center" Margin="0,24,0,0"/>
                    </StackPanel>
                </Grid>
            </TabItem>

            <!-- Page 1: EULA -->
            <TabItem>
                <Grid Margin="40,20">
                    <Grid.RowDefinitions>
                        <RowDefinition Height="Auto"/>
                        <RowDefinition Height="*"/>
                        <RowDefinition Height="Auto"/>
                    </Grid.RowDefinitions>
                    <TextBlock Grid.Row="0" Text="End User License Agreement"
                               FontSize="22" FontWeight="Bold" Foreground="#B794F6"
                               Margin="0,0,0,12"/>
                    <Border Grid.Row="1" Background="#111128" CornerRadius="6"
                            BorderBrush="#252545" BorderThickness="1">
                        <ScrollViewer VerticalScrollBarVisibility="Auto" Margin="12">
                            <TextBlock x:Name="EulaTextBlock" TextWrapping="Wrap"
                                       Foreground="#AAAACC" FontSize="12"
                                       FontFamily="Consolas" LineHeight="18"/>
                        </ScrollViewer>
                    </Border>
                    <CheckBox x:Name="AcceptEula" Grid.Row="2"
                              Content="  I have read and accept the End User License Agreement"
                              Foreground="#CCCCEE" FontSize="13"
                              Margin="0,12,0,0"/>
                </Grid>
            </TabItem>

            <!-- Page 2: Install Location -->
            <TabItem>
                <Grid Margin="40,20">
                    <StackPanel VerticalAlignment="Center">
                        <TextBlock Text="Installation Location" FontSize="22"
                                   FontWeight="Bold" Foreground="#B794F6"
                                   Margin="0,0,0,20"/>
                        <TextBlock Text="Choose where to install Zepra Browser:"
                                   Foreground="#AAAACC" FontSize="13" Margin="0,0,0,12"/>
                        <Grid Margin="0,0,0,24">
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="*"/>
                                <ColumnDefinition Width="Auto"/>
                            </Grid.ColumnDefinitions>
                            <TextBox x:Name="InstallPathBox" Grid.Column="0"
                                     Background="#111128" Foreground="#CCCCEE"
                                     BorderBrush="#333355" FontSize="13"
                                     Padding="10,8" VerticalContentAlignment="Center"/>
                            <Button x:Name="BrowseBtn" Grid.Column="1"
                                    Content="Browse..." Style="{StaticResource SecondaryButton}"
                                    Margin="8,0,0,0"/>
                        </Grid>
                        <TextBlock Text="Options:" Foreground="#AAAACC" FontSize="14"
                                   FontWeight="SemiBold" Margin="0,0,0,8"/>
                        <CheckBox x:Name="DesktopShortcut" IsChecked="True"
                                  Content="  Create desktop shortcut"
                                  Foreground="#CCCCEE" FontSize="13" Margin="0,4"/>
                        <CheckBox x:Name="StartMenuEntry" IsChecked="True"
                                  Content="  Create Start Menu entry"
                                  Foreground="#CCCCEE" FontSize="13" Margin="0,4"/>
                        <Border Background="#1C1020" CornerRadius="6" Padding="14"
                                Margin="0,20,0,0" BorderBrush="#3D2040" BorderThickness="1">
                            <TextBlock TextWrapping="Wrap" Foreground="#CC8899" FontSize="12">
                                📦 Estimated disk space required: ~200 MB
                            </TextBlock>
                        </Border>
                    </StackPanel>
                </Grid>
            </TabItem>

            <!-- Page 3: Installing -->
            <TabItem>
                <Grid Margin="40,20">
                    <StackPanel VerticalAlignment="Center">
                        <TextBlock Text="Installing..." FontSize="22"
                                   FontWeight="Bold" Foreground="#B794F6"
                                   Margin="0,0,0,20"/>
                        <ProgressBar x:Name="InstallProgress" Height="8"
                                     Background="#1A1A2E" Foreground="#6C3AED"
                                     Margin="0,0,0,12" Maximum="100"/>
                        <TextBlock x:Name="InstallStatus"
                                   Text="Preparing installation..."
                                   Foreground="#AAAACC" FontSize="13"
                                   Margin="0,0,0,24"/>
                        <Border Background="#111128" CornerRadius="6" Padding="14"
                                BorderBrush="#252545" BorderThickness="1">
                            <TextBlock x:Name="InstallLog" TextWrapping="Wrap"
                                       Foreground="#666688" FontSize="11"
                                       FontFamily="Consolas" MaxHeight="180"/>
                        </Border>
                    </StackPanel>
                </Grid>
            </TabItem>

            <!-- Page 4: Complete -->
            <TabItem>
                <Grid Margin="40,20">
                    <StackPanel VerticalAlignment="Center" HorizontalAlignment="Center">
                        <TextBlock Text="✅" FontSize="56" HorizontalAlignment="Center"
                                   Margin="0,0,0,16"/>
                        <TextBlock Text="Installation Complete!" FontSize="28"
                                   FontWeight="Bold" Foreground="#4ADE80"
                                   HorizontalAlignment="Center" Margin="0,0,0,8"/>
                        <TextBlock FontSize="14" Foreground="#AAAACC"
                                   HorizontalAlignment="Center" Margin="0,0,0,24"
                                   TextAlignment="Center">
                            Zepra Browser v0.1.0-beta has been installed successfully.
                            <LineBreak/>Thank you for helping test the beta!
                        </TextBlock>
                        <CheckBox x:Name="LaunchAfter" IsChecked="True"
                                  Content="  Launch Zepra Browser"
                                  Foreground="#CCCCEE" FontSize="13"
                                  HorizontalAlignment="Center" Margin="0,0,0,16"/>
                        <Border Background="#1A1A2E" CornerRadius="8" Padding="16"
                                BorderBrush="#252545" BorderThickness="1"
                                HorizontalAlignment="Center">
                            <TextBlock Foreground="#888899" FontSize="11"
                                       TextAlignment="Center" TextWrapping="Wrap">
                                Report bugs: github.com/KetiveeAI/Zepra/issues
                                <LineBreak/>Support: support@ketivee.com
                            </TextBlock>
                        </Border>
                    </StackPanel>
                </Grid>
            </TabItem>
        </TabControl>

        <!-- Bottom Navigation Bar -->
        <Border Grid.Row="2" Background="#0A0A14" Padding="20,12"
                BorderBrush="#1A1A2E" BorderThickness="0,1,0,0">
            <Grid>
                <Button x:Name="CancelBtn" Content="Cancel"
                        Style="{StaticResource SecondaryButton}"
                        HorizontalAlignment="Left"/>
                <StackPanel Orientation="Horizontal" HorizontalAlignment="Right">
                    <Button x:Name="BackBtn" Content="← Back"
                            Style="{StaticResource SecondaryButton}"
                            Margin="0,0,8,0" Visibility="Collapsed"/>
                    <Button x:Name="NextBtn" Content="Next →"
                            Style="{StaticResource AccentButton}"/>
                </StackPanel>
            </Grid>
        </Border>
    </Grid>
</Window>
"@

    # Parse XAML
    $reader = [System.Xml.XmlReader]::Create([System.IO.StringReader]::new($xaml))
    $window = [Windows.Markup.XamlReader]::Load($reader)

    # Get controls
    $pages          = $window.FindName("Pages")
    $eulaTextBlock  = $window.FindName("EulaTextBlock")
    $acceptEula     = $window.FindName("AcceptEula")
    $installPathBox = $window.FindName("InstallPathBox")
    $browseBtn      = $window.FindName("BrowseBtn")
    $desktopShortcut= $window.FindName("DesktopShortcut")
    $startMenuEntry = $window.FindName("StartMenuEntry")
    $installProgress= $window.FindName("InstallProgress")
    $installStatus  = $window.FindName("InstallStatus")
    $installLog     = $window.FindName("InstallLog")
    $launchAfter    = $window.FindName("LaunchAfter")
    $cancelBtn      = $window.FindName("CancelBtn")
    $backBtn        = $window.FindName("BackBtn")
    $nextBtn        = $window.FindName("NextBtn")

    # Initialize
    $eulaTextBlock.Text = $Script:EulaText
    $installPathBox.Text = $Script:DefaultPath
    $pages.SelectedIndex = 0

    # State
    $Script:CurrentPage = 0
    $Script:InstallResult = $false

    function Update-Navigation {
        $backBtn.Visibility = if ($Script:CurrentPage -gt 0 -and $Script:CurrentPage -lt 3) { "Visible" } else { "Collapsed" }

        switch ($Script:CurrentPage) {
            0 { $nextBtn.Content = "Get Started →"; $nextBtn.IsEnabled = $true }
            1 { $nextBtn.Content = "Accept & Continue →"; $nextBtn.IsEnabled = $acceptEula.IsChecked }
            2 { $nextBtn.Content = "Install"; $nextBtn.IsEnabled = $true }
            3 { $nextBtn.Visibility = "Collapsed"; $cancelBtn.Visibility = "Collapsed" }
            4 { $nextBtn.Content = "Finish"; $nextBtn.IsEnabled = $true; $cancelBtn.Visibility = "Collapsed"; $backBtn.Visibility = "Collapsed" }
        }
    }

    # Event handlers
    $acceptEula.Add_Checked({ $nextBtn.IsEnabled = $true })
    $acceptEula.Add_Unchecked({ $nextBtn.IsEnabled = $false })

    $browseBtn.Add_Click({
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = "Select installation folder"
        $dialog.SelectedPath = $installPathBox.Text
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $installPathBox.Text = $dialog.SelectedPath
        }
    })

    $backBtn.Add_Click({
        if ($Script:CurrentPage -gt 0) {
            $Script:CurrentPage--
            $pages.SelectedIndex = $Script:CurrentPage
            Update-Navigation
        }
    })

    $cancelBtn.Add_Click({
        $result = [System.Windows.MessageBox]::Show(
            "Are you sure you want to cancel the installation?",
            "Cancel Installation",
            [System.Windows.MessageBoxButton]::YesNo,
            [System.Windows.MessageBoxImage]::Question)
        if ($result -eq [System.Windows.MessageBoxResult]::Yes) {
            $window.Close()
        }
    })

    $nextBtn.Add_Click({
        switch ($Script:CurrentPage) {
            {$_ -lt 2} {
                $Script:CurrentPage++
                $pages.SelectedIndex = $Script:CurrentPage
                Update-Navigation
            }
            2 {
                # Start installation
                $Script:CurrentPage = 3
                $pages.SelectedIndex = 3
                Update-Navigation

                $targetPath = $installPathBox.Text
                $createDesktop = $desktopShortcut.IsChecked
                $createStartMenu = $startMenuEntry.IsChecked

                # Run install in background
                $window.Dispatcher.InvokeAsync({
                    try {
                        Install-ZepraBrowser -Path $targetPath `
                            -Desktop:$createDesktop `
                            -StartMenu:$createStartMenu `
                            -ProgressBar $installProgress `
                            -StatusText $installStatus `
                            -LogText $installLog `
                            -Dispatcher $window.Dispatcher
                        $Script:InstallResult = $true
                    } catch {
                        $installStatus.Text = "Installation failed: $($_.Exception.Message)"
                        $installLog.Text += "`nERROR: $($_.Exception.Message)"
                    }
                    # Move to complete page
                    $Script:CurrentPage = 4
                    $pages.SelectedIndex = 4
                    Update-Navigation
                }.GetNewClosure())
            }
            4 {
                # Finish
                if ($launchAfter.IsChecked -and $Script:InstallResult) {
                    $exePath = Join-Path $installPathBox.Text "zepra_browser.exe"
                    if (Test-Path $exePath) {
                        Start-Process $exePath
                    }
                }
                $window.Close()
            }
        }
    })

    Update-Navigation
    $window.ShowDialog() | Out-Null
}

# ============================================================================
# Installation Logic
# ============================================================================
function Install-ZepraBrowser {
    param(
        [string]$Path,
        [switch]$Desktop,
        [switch]$StartMenu,
        $ProgressBar,
        $StatusText,
        $LogText,
        $Dispatcher
    )

    function Update-UI($progress, $status, $log) {
        if ($Dispatcher) {
            $Dispatcher.Invoke({
                if ($ProgressBar) { $ProgressBar.Value = $progress }
                if ($StatusText)  { $StatusText.Text = $status }
                if ($LogText -and $log) { $LogText.Text += "$log`n" }
            }.GetNewClosure())
        }
    }

    # Locate source files
    $sourceDir = $Script:ScriptDir
    $binDir = Join-Path $sourceDir "bin"
    if (-not (Test-Path (Join-Path $binDir "zepra_browser.exe"))) {
        $binDir = Join-Path $sourceDir "build-win\bin"
    }

    # Step 1: Create directories
    Update-UI 5 "Creating directories..." "Creating: $Path"
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    New-Item -ItemType Directory -Path "$Path\resources" -Force | Out-Null
    New-Item -ItemType Directory -Path "$Path\resources\icons" -Force | Out-Null
    New-Item -ItemType Directory -Path "$Path\resources\web" -Force | Out-Null
    New-Item -ItemType Directory -Path "$Path\legal" -Force | Out-Null
    Start-Sleep -Milliseconds 300

    # Step 2: Copy main executable
    Update-UI 15 "Copying Zepra Browser executable..." "Copying: zepra_browser.exe"
    $exeSrc = Join-Path $binDir "zepra_browser.exe"
    if (Test-Path $exeSrc) {
        Copy-Item $exeSrc -Destination $Path -Force
    } else {
        Update-UI 15 "Warning: zepra_browser.exe not found in $binDir" "WARN: exe not found"
    }
    Start-Sleep -Milliseconds 200

    # Step 3: Copy support executables
    Update-UI 25 "Copying support tools..." ""
    $tools = @("zepra-repl.exe", "zepra-dump-bytecode.exe")
    foreach ($tool in $tools) {
        $toolSrc = Join-Path $binDir $tool
        if (Test-Path $toolSrc) {
            Copy-Item $toolSrc -Destination $Path -Force
            Update-UI 0 "" "Copied: $tool"
        }
    }
    Start-Sleep -Milliseconds 200

    # Step 4: Copy resources
    Update-UI 40 "Copying resources..." ""
    $resDir = Join-Path $sourceDir "resources"
    if (Test-Path $resDir) {
        $icons = Join-Path $resDir "icons"
        if (Test-Path $icons) {
            Copy-Item "$icons\*" -Destination "$Path\resources\icons" -Recurse -Force -ErrorAction SilentlyContinue
            Update-UI 0 "" "Copied: icons"
        }
        $web = Join-Path $resDir "web"
        if (Test-Path $web) {
            Copy-Item "$web\*" -Destination "$Path\resources\web" -Recurse -Force -ErrorAction SilentlyContinue
            Update-UI 0 "" "Copied: web resources"
        }
    }
    Start-Sleep -Milliseconds 200

    # Step 5: Copy legal documents
    Update-UI 50 "Copying license and EULA..." ""
    $legalDir = Join-Path $sourceDir "legal"
    if (Test-Path $legalDir) {
        Copy-Item "$legalDir\*" -Destination "$Path\legal" -Recurse -Force -ErrorAction SilentlyContinue
    }
    $license = Join-Path $sourceDir "LICENSE.md"
    if (Test-Path $license) {
        Copy-Item $license -Destination "$Path\legal\LICENSE.md" -Force
    }
    $releaseNotes = Join-Path $sourceDir "RELEASE_NOTES.md"
    if (Test-Path $releaseNotes) {
        Copy-Item $releaseNotes -Destination "$Path\RELEASE_NOTES.md" -Force
    }
    Update-UI 0 "" "Copied: legal documents"
    Start-Sleep -Milliseconds 200

    # Step 6: Create uninstaller
    Update-UI 60 "Creating uninstaller..." "Creating: Uninstall-ZepraBrowser.ps1"
    $uninstallScript = Join-Path $sourceDir "Uninstall-ZepraBrowser.ps1"
    if (Test-Path $uninstallScript) {
        Copy-Item $uninstallScript -Destination "$Path\Uninstall-ZepraBrowser.ps1" -Force
    }
    Start-Sleep -Milliseconds 200

    # Step 7: Registry entries (Add/Remove Programs)
    Update-UI 70 "Registering application..." "Writing: registry entries"
    $regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($Script:UninstallGuid)"
    try {
        New-Item -Path $regPath -Force | Out-Null
        Set-ItemProperty -Path $regPath -Name "DisplayName" -Value "$($Script:AppName) (Beta)"
        Set-ItemProperty -Path $regPath -Name "DisplayVersion" -Value $Script:AppVersion
        Set-ItemProperty -Path $regPath -Name "Publisher" -Value $Script:Publisher
        Set-ItemProperty -Path $regPath -Name "InstallLocation" -Value $Path
        Set-ItemProperty -Path $regPath -Name "UninstallString" -Value "powershell.exe -ExecutionPolicy Bypass -File `"$Path\Uninstall-ZepraBrowser.ps1`""
        Set-ItemProperty -Path $regPath -Name "DisplayIcon" -Value "$Path\zepra_browser.exe"
        Set-ItemProperty -Path $regPath -Name "URLInfoAbout" -Value $Script:Website
        Set-ItemProperty -Path $regPath -Name "NoModify" -Value 1 -Type DWord
        Set-ItemProperty -Path $regPath -Name "NoRepair" -Value 1 -Type DWord
        $size = (Get-ChildItem $Path -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1KB
        Set-ItemProperty -Path $regPath -Name "EstimatedSize" -Value ([int]$size) -Type DWord
    } catch {
        Update-UI 0 "" "WARN: Registry write failed (non-critical)"
    }
    Start-Sleep -Milliseconds 200

    # Step 8: Desktop shortcut
    if ($Desktop) {
        Update-UI 80 "Creating desktop shortcut..." "Creating: desktop shortcut"
        try {
            $shell = New-Object -ComObject WScript.Shell
            $desktopPath = [Environment]::GetFolderPath("CommonDesktopDirectory")
            $shortcut = $shell.CreateShortcut("$desktopPath\Zepra Browser (Beta).lnk")
            $shortcut.TargetPath = "$Path\zepra_browser.exe"
            $shortcut.WorkingDirectory = $Path
            $shortcut.Description = "Zepra Browser v0.1.0-beta by KetiveeAI"
            $shortcut.IconLocation = "$Path\zepra_browser.exe,0"
            $shortcut.Save()
        } catch {
            Update-UI 0 "" "WARN: Desktop shortcut failed"
        }
    }
    Start-Sleep -Milliseconds 200

    # Step 9: Start Menu entry
    if ($StartMenu) {
        Update-UI 90 "Creating Start Menu entry..." "Creating: Start Menu entry"
        try {
            $shell = New-Object -ComObject WScript.Shell
            $startMenuPath = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\KetiveeAI"
            New-Item -ItemType Directory -Path $startMenuPath -Force | Out-Null
            $shortcut = $shell.CreateShortcut("$startMenuPath\Zepra Browser (Beta).lnk")
            $shortcut.TargetPath = "$Path\zepra_browser.exe"
            $shortcut.WorkingDirectory = $Path
            $shortcut.Description = "Zepra Browser v0.1.0-beta by KetiveeAI"
            $shortcut.Save()

            # Uninstall shortcut
            $unShortcut = $shell.CreateShortcut("$startMenuPath\Uninstall Zepra Browser.lnk")
            $unShortcut.TargetPath = "powershell.exe"
            $unShortcut.Arguments = "-ExecutionPolicy Bypass -File `"$Path\Uninstall-ZepraBrowser.ps1`""
            $unShortcut.Save()
        } catch {
            Update-UI 0 "" "WARN: Start Menu entry failed"
        }
    }
    Start-Sleep -Milliseconds 200

    # Step 10: Complete
    Update-UI 100 "Installation complete!" "Done. Installed to: $Path"
    Start-Sleep -Milliseconds 500
}

# ============================================================================
# Main Entry Point
# ============================================================================
if ($Silent) {
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host "  Zepra Browser v0.1.0-beta Silent Install" -ForegroundColor Cyan
    Write-Host "  Copyright (c) 2025-2026 KetiveeAI" -ForegroundColor DarkCyan
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "[!] BETA SOFTWARE — For testing purposes only" -ForegroundColor Yellow
    Write-Host ""

    Install-ZepraBrowser -Path $Script:DefaultPath -Desktop -StartMenu
    Write-Host ""
    Write-Host "[OK] Installation complete: $($Script:DefaultPath)" -ForegroundColor Green
} else {
    Show-InstallerGUI
}
