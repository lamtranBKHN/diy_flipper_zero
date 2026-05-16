#!/usr/bin/env bash
# =============================================================================
# Flash Firmware to DIY Flipper Zero (Nucleus Dark MK1)
# Target: STM32WB55CGU6 (TARGET_HW=7)
# Usage: ./flash.sh [dfu|stlink|help]
# =============================================================================

set -euo pipefail

# --- Configuration ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${SCRIPT_DIR}/dist/f7-DC"
FIRMWARE_BIN=""
FIRMWARE_DFU=""
FIRMWARE_ELF=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# --- Helper Functions ---
log_info()  { echo -e "${BLUE}[INFO]${NC}  $1"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

find_firmware() {
    log_info "Locating firmware binaries in ${DIST_DIR}..."

    # Find the latest .bin file
    FIRMWARE_BIN=$(find "${DIST_DIR}" -name "*.bin" -type f | head -n1)
    # Find the latest .dfu file
    FIRMWARE_DFU=$(find "${DIST_DIR}" -name "*.dfu" -type f | head -n1)
    # Find the latest .elf file
    FIRMWARE_ELF=$(find "${DIST_DIR}" -name "*.elf" -type f | head -n1)

    if [ -z "$FIRMWARE_BIN" ]; then
        log_error "No .bin firmware found in ${DIST_DIR}"
        log_info "Run: ./fbt TARGET_HW=7 DEBUG=1 COMPACT=1"
        exit 1
    fi

    log_ok "Found firmware:"
    echo "  BIN: ${FIRMWARE_BIN}"
    [ -n "$FIRMWARE_DFU" ] && echo "  DFU: ${FIRMWARE_DFU}"
    [ -n "$FIRMWARE_ELF" ] && echo "  ELF: ${FIRMWARE_ELF}"
    echo ""
}

check_dfu_device() {
    if command -v dfu-util &> /dev/null; then
        if dfu-util -l 2>/dev/null | grep -q "STM32WB"; then
            log_ok "STM32WB55 device found in DFU mode"
            return 0
        else
            log_error "No STM32WB55 device found in DFU mode"
            log_info "Hold [LEFT] + [BACK] buttons while powering on to enter DFU mode"
            log_info "Or use: stm32loader.py --dfu"
            return 1
        fi
    else
        log_error "dfu-util not found. Install it:"
        echo "  Ubuntu/Debian: sudo apt install dfu-util"
        echo "  macOS:         brew install dfu-util"
        echo "  Windows:       Download from: http://dfu-util.sourceforge.net/"
        return 1
    fi
}

check_stlink() {
    if command -v STM32_Programmer_CLI &> /dev/null; then
        if STM32_Programmer_CLI -c port=SWD 2>&1 | grep -q "Device ID"; then
            log_ok "STM32WB55 device found via ST-Link"
            return 0
        else
            log_error "No device detected via ST-Link"
            log_info "Check connections: SWDIO, SWCLK, GND, VCC (3.3V)"
            return 1
        fi
    elif command -v st-flash &> /dev/null; then
        log_ok "st-flash found (fallback programmer)"
        return 0
    else
        log_error "No ST-Link programmer found. Install one:"
        echo "  STM32CubeProgrammer: https://www.st.com/en/development-tools/stm32cubeprog.html"
        echo "  st-flash (CLI):      sudo apt install stlink-tools"
        return 1
    fi
}

# --- Flash Methods ---
flash_dfu() {
    log_info "Flashing via DFU (USB)..."

    check_dfu_device || exit 1

    if [ -z "$FIRMWARE_DFU" ]; then
        log_error "No .dfu file found. Cannot flash via DFU without it."
        log_info "Rebuild with: ./fbt TARGET_HW=7 DEBUG=1 COMPACT=1"
        exit 1
    fi

    log_info "Erasing flash..."
    dfu-util -a 0 -s 0x08000000:mass-erase || {
        log_error "Erase failed. Retrying with sector erase..."
        dfu-util -a 0 -s 0x08000000:force:mass-erase || true
    }

    log_info "Writing firmware (this takes ~30-60 seconds)..."
    dfu-util -a 0 -s 0x08000000:leave -D "${FIRMWARE_DFU}" || {
        log_error "DFU flash failed!"
        exit 1
    }

    log_ok "Firmware flashed successfully via DFU!"
    log_info "Device will reboot automatically."
    log_info "Watch for the Flipper Zero splash screen..."
}

flash_stlink_cube() {
    log_info "Flashing via ST-Link (STM32CubeProgrammer)..."

    check_stlink || exit 1

    log_info "Connecting to device via SWD..."
    STM32_Programmer_CLI -c port=SWD -e all || {
        log_error "Failed to erase flash via ST-Link"
        exit 1
    }

    log_info "Writing firmware..."
    STM32_Programmer_CLI -c port=SWD -d "${FIRMWARE_BIN}" 0x08000000 || {
        log_error "Failed to write firmware via ST-Link"
        exit 1
    }

    log_info "Verifying flash..."
    STM32_Programmer_CLI -c port=SWD -v "${FIRMWARE_BIN}" 0x08000000 || {
        log_warn "Verification had warnings. Check device manually."
    }

    log_info "Resetting device..."
    STM32_Programmer_CLI -c port=SWD -rst || true

    log_ok "Firmware flashed successfully via ST-Link!"
    log_info "Device should reboot automatically."
}

flash_stlink_cli() {
    log_info "Flashing via ST-Link (st-flash CLI fallback)..."

    check_stlink || exit 1

    log_info "Writing firmware binary..."
    st-flash write "${FIRMWARE_BIN}" 0x08000000 || {
        log_error "st-flash write failed!"
        exit 1
    }

    log_ok "Firmware flashed successfully via st-flash!"
    log_info "Reset the device manually (RST button or power cycle)."
}

# --- Main ---
main() {
    echo "============================================="
    echo " DIY Flipper Zero Firmware Flash Utility"
    echo " Target: STM32WB55CGU6 (TARGET_HW=7)"
    echo "============================================="
    echo ""

    case "${1:-help}" in
        dfu)
            find_firmware
            flash_dfu
            ;;
        stlink|st-link)
            find_firmware
            if command -v STM32_Programmer_CLI &> /dev/null; then
                flash_stlink_cube
            elif command -v st-flash &> /dev/null; then
                flash_stlink_cli
            else
                log_error "No ST-Link programmer found"
                exit 1
            fi
            ;;
        help|--help|-h)
            echo "Usage: $0 [METHOD]"
            echo ""
            echo "Methods:"
            echo "  dfu      Flash via USB DFU mode (recommended)"
            echo "  stlink   Flash via ST-Link debugger (SWD)"
            echo "  help     Show this help message"
            echo ""
            echo "Prerequisites:"
            echo "  DFU:     dfu-util installed, device in DFU mode"
            echo "  ST-Link: STM32CubeProgrammer or st-flash installed"
            echo ""
            echo "Entering DFU Mode:"
            echo "  1. Power off the device"
            echo "  2. Hold [LEFT] + [BACK] buttons"
            echo "  3. Power on while holding buttons"
            echo "  4. Release when the screen stays black"
            echo "  5. Verify with: dfu-util -l"
            echo ""
            ;;
        *)
            log_error "Unknown method: $1"
            echo ""
            $0 help
            exit 1
            ;;
    esac
}

main "$@"
