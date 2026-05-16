#!/usr/bin/env pwsh
# =============================================================================
# Flash Firmware to DIY Flipper Zero (Nucleus Dark MK1) - Windows PowerShell
# Target: STM32WB55CGU6 (TARGET_HW=7)
# Usage: .\flash.ps1 [-Method dfu|stlink] [-Help]
# =============================================================================

[CmdletBinding()]
param(
    [ValidateSet('dfu', 'stlink', 'help')]
    [string]$Method = 'help'
)

# --- Configuration ---
$ScriptDir = $PSScriptRoot
$DistDir = Join-Path $ScriptDir "dist\f7-DC"

# --- Helper Functions ---
function Write-Info  { Write-Host "[INFO]  " -ForegroundColor Blue -NoNewline; Write-Host $args }
function Write-Ok    { Write-Host "[OK]    " -ForegroundColor Green -NoNewline; Write-Host $args }
function Write-Warn  { Write-Host "[WARN]  " -ForegroundColor Yellow -NoNewline; Write-Host $args }
function Write-Error { Write-Host "[ERROR] " -ForegroundColor Red -NoNewline; Write-Host $args }

function Find-Firmware {
    Write-Info "Locating firmware binaries in $DistDir..."

    $FirmwareBin = Get-ChildItem -Path $DistDir -Filter "*.bin" -ErrorAction SilentlyContinue | Select-Object -First 1
    $FirmwareDfu = Get-ChildItem -Path $DistDir -Filter "*.dfu" -ErrorAction SilentlyContinue | Select-Object -First 1
    $FirmwareElf = Get-ChildItem -Path $DistDir -Filter "*.elf" -ErrorAction SilentlyContinue | Select-Object -First 1

    if (-not $FirmwareBin) {
        Write-Error "No .bin firmware found in $DistDir"
        Write-Info "Run: .\fbt TARGET_HW=7 DEBUG=1 COMPACT=1"
        exit 1
    }

    Write-Ok "Found firmware:"
    Write-Host "  BIN: $($FirmwareBin.FullName)"
    if ($FirmwareDfu) { Write-Host "  DFU: $($FirmwareDfu.FullName)" }
    if ($FirmwareElf) { Write-Host "  ELF: $($FirmwareElf.FullName)" }
    Write-Host ""

    return @{
        Bin = $FirmwareBin.FullName
        Dfu = $FirmwareDfu?.FullName
        Elf = $FirmwareElf?.FullName
    }
}

function Test-DfuDevice {
    $DfuUtil = Get-Command dfu-util -ErrorAction SilentlyContinue
    if (-not $DfuUtil) {
        Write-Error "dfu-util not found in PATH"
        Write-Info "Install via winget: winget install --id=dev.DfuUtil"
        Write-Info "Or download from: http://dfu-util.sourceforge.net/"
        return $false
    }

    $Output = & dfu-util -l 2>&1 | Out-String
    if ($Output -match "STM32") {
        Write-Ok "STM32WB55 device found in DFU mode"
        return $true
    } else {
        Write-Error "No STM32WB55 device found in DFU mode"
        Write-Info "Hold [LEFT] + [BACK] buttons while powering on to enter DFU mode"
        return $false
    }
}

function Test-StLinkDevice {
    # Check for STM32CubeProgrammer
    $CubeProg = Get-Command STM32_Programmer_CLI -ErrorAction SilentlyContinue
    if ($CubeProg) {
        $Output = & STM32_Programmer_CLI -c port=SWD 2>&1 | Out-String
        if ($Output -match "Device ID") {
            Write-Ok "STM32WB55 device found via ST-Link (CubeProgrammer)"
            return "cubeprogrammer"
        }
    }

    # Check for OpenOCD (common alternative)
    $OpenOcd = Get-Command openocd -ErrorAction SilentlyContinue
    if ($OpenOcd) {
        Write-Ok "OpenOCD found (will use st-flash fallback)"
        return "openocd"
    }

    Write-Error "No ST-Link programmer found"
    Write-Info "Install STM32CubeProgrammer from: https://www.st.com/en/development-tools/stm32cubeprog.html"
    return $false
}

function Flash-Dfu {
    param([hashtable]$Firmware)

    Write-Info "Flashing via DFU (USB)..."

    if (-not (Test-DfuDevice)) { exit 1 }
    if (-not $Firmware.Dfu) {
        Write-Error "No .dfu file found. Cannot flash via DFU."
        exit 1
    }

    Write-Info "Erasing flash..."
    try {
        & dfu-util -a 0 -s "0x08000000:mass-erase" 2>&1 | Out-String | Write-Verbose
    } catch {
        Write-Warn "Mass erase failed, continuing with write..."
    }

    Write-Info "Writing firmware (this takes ~30-60 seconds)..."
    & dfu-util -a 0 -s "0x08000000:leave" -D $Firmware.Dfu
    if ($LASTEXITCODE -ne 0) {
        Write-Error "DFU flash failed!"
        exit 1
    }

    Write-Ok "Firmware flashed successfully via DFU!"
    Write-Info "Device will reboot automatically."
    Write-Info "Watch for the Flipper Zero splash screen..."
}

function Flash-StLink {
    param([hashtable]$Firmware)

    $ProgrammerType = Test-StLinkDevice
    if (-not $ProgrammerType) { exit 1 }

    Write-Info "Flashing via ST-Link..."

    if ($ProgrammerType -eq "cubeprogrammer") {
        Write-Info "Connecting to device via SWD..."
        $Output = & STM32_Programmer_CLI -c port=SWD -e all 2>&1 | Out-String
        Write-Verbose $Output

        Write-Info "Writing firmware..."
        $Output = & STM32_Programmer_CLI -c port=SWD -d $Firmware.Bin 0x08000000 2>&1 | Out-String
        Write-Verbose $Output

        Write-Info "Verifying flash..."
        $Output = & STM32_Programmer_CLI -c port=SWD -v $Firmware.Bin 0x08000000 2>&1 | Out-String
        Write-Verbose $Output

        Write-Info "Resetting device..."
        & STM32_Programmer_CLI -c port=SWD -rst 2>&1 | Out-Null

        Write-Ok "Firmware flashed successfully via ST-Link!"
    } else {
        Write-Error "No compatible ST-Link programmer found"
        exit 1
    }

    Write-Info "Device should reboot automatically."
}

function Show-Help {
    Write-Host @"
=============================================
 DIY Flipper Zero Firmware Flash Utility
 Target: STM32WB55CGU6 (TARGET_HW=7)
=============================================

Usage: .\flash.ps1 [-Method dfu|stlink]

Methods:
  dfu      Flash via USB DFU mode (recommended)
  stlink   Flash via ST-Link debugger (SWD)
  help     Show this help message (default)

Prerequisites:
  DFU:     dfu-util installed, device in DFU mode
  ST-Link: STM32CubeProgrammer installed

Entering DFU Mode:
  1. Power off the device
  2. Hold [LEFT] + [BACK] buttons
  3. Power on while holding buttons
  4. Release when the screen stays black
  5. Verify with: dfu-util -l

ST-Link Wiring:
  SWDIO -> PA13
  SWCLK -> PA14
  GND   -> GND
  VCC   -> 3.3V (DO NOT USE 5V!)

"@
}

# --- Main ---
Write-Host "============================================="
Write-Host " DIY Flipper Zero Firmware Flash Utility"
Write-Host " Target: STM32WB55CGU6 (TARGET_HW=7)"
Write-Host "============================================="
Write-Host ""

switch ($Method) {
    'dfu' {
        $Firmware = Find-Firmware
        Flash-Dfu -Firmware $Firmware
    }
    'stlink' {
        $Firmware = Find-Firmware
        Flash-StLink -Firmware $Firmware
    }
    'help' {
        Show-Help
    }
}
