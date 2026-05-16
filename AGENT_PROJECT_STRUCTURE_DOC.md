# Project Structure Documentation

## Project Overview for Agents

This is a **DIY Flipper Zero firmware** project built on the **STM32WB55CGU6** microcontroller. It is a custom hardware implementation that replicates Flipper Zero functionality with modified peripherals (no LCD, no nRF24, different button layout via PCF8574, etc.).

### High-Level Purpose

Provides a complete firmware stack for a DIY Flipper Zero-compatible device, including:
- Multi-protocol NFC/RFID support (PN532 via I2C)
- Sub-GHz radio communication (CC1101)
- GPIO, 1-Wire (iButton), IR TX/RX
- Storage (microSD over SPI), UI (OLED via I2C)
- Game and application support (FAP external apps)

### Main Subsystems

- **Build System**: SCons-based (`fbt`), Python scripts, toolchain management
- **Firmware Core**: FreeRTOS-based runtime with FURI HAL
- **Applications**: Main apps (archive, subghz, nfc, infrared, etc.) + Services (desktop, gui, loader, etc.)
- **External Apps**: User-contributed FAP applications in `applications/external/`
- **Hardware Abstraction Layer (HAL)**: Target-specific HAL for STM32WB55 in `targets/f7/furi_hal/`
- **Libraries**: Protocol stacks (nfc, subghz, infrared, lfrfid, etc.), drivers (cc1101, pn532, pcf8574)

### Typical End-to-End Workflow

1. **Configure**: User runs `./fbt` which loads toolchain via `scripts/toolchain/fbtenv.cmd` (Windows) or `fbtenv.sh` (Linux/macOS)
2. **Build**: SCons reads `SConstruct` and `firmware.scons`, compiles C/C++ code with ARM GCC, links against FreeRTOS and protocol libraries
3. **Output**: Produces firmware ELF, HEX, BIN, DFU files in `build/` directory
4. **Flash**: `./fbt flash` uses OpenOCD to program via ST-Link/CMSIS-DAP debugger
5. **Deploy**: `./fbt fap_dist` packages external apps for deployment to SD card
6. **Debug**: `./fbt debug` launches GDB with OpenOCD for real-time debugging
7. **Update**: `./fbt updater_package` creates OTA update bundles with radio stack
8. **Lint**: `./fbt lint_all` runs Python black, C/C++ lint, and image lint checks
9. **Format**: `./fbt format_all` auto-formats code using black and clang-format
10. **Dist**: `./fbt copro_dist` packages Core2 BLE stack for qFlipper

---

## Directory Tree

```
F:\FB_V3\diy_flipper_zero.worktrees\copilot-worktree-2026-04-03T07-50-46\
├── SConstruct                      # Main SCons entry point - defines all build targets and aliases
├── firmware.scons                   # Firmware compilation script - builds libs, assets, apps
├── fbt_options.py                  # Build configuration - TARGET_HW, COMPACT, DEBUG, COPRO_STACK_*
├── fbt.cmd / fbt                   # Build wrapper script (launches SCons)
├── fbt                             # Symlink to SConstruct on Unix
│
├── AGENTS.md                       # Agent instructions for this project (build commands, hardware quirks)
├── ReadMe.md                       # Top-level project README
├── CHANGELOG.md                    # Version history
├── CODING_STYLE.md                 # Code style guidelines
│
├── applications/                   # Built-in Flipper applications
│   ├── main/                       # Primary apps (archive, bad_usb, gpio, ibutton, infrared, lfrfid, nfc, subghz, u2f)
│   │   ├── archive/                # File browser / archive manager
│   │   ├── bad_usb/               # USB Rubber Ducky style attacks
│   │   ├── gpio/                  # GPIO testing utility
│   │   ├── ibutton/               # iButton (1-Wire) reader/writer
│   │   ├── infrared/               # IR transmitter / universal remote
│   │   ├── lfrfid/                # Low-frequency RFID (EM4100, etc.)
│   │   ├── nfc/                   # NFC/RFID reader (ST25R3916 or PN532)
│   │   ├── subghz/                # Sub-GHz receiver/transmitter (CC1101)
│   │   ├── u2f/                   # FIDO U2F token
│   │   └── application.fam         # App manifest for main apps
│   │
│   ├── services/                   # System services (run at boot, always loaded)
│   │   ├── desktop/               # Main menu UI service
│   │   ├── gui/                  # Graphics/ViewPort subsystem
│   │   ├── loader/               # App launcher service
│   │   ├── cli/                  # Command-line interface service
│   │   ├── bt/                   # Bluetooth stack service
│   │   ├── power/               # Power management (battery, charging)
│   │   ├── notification/         # Notification (LED, vibro, buzzer)
│   │   ├── storage/              # File storage (SD card, internal flash)
│   │   ├── dialogs/              # Dialog UI component
│   │   ├── input/                # Input handling (buttons via PCF8574)
│   │   ├── rpc/                  # RPC bridge (USB/UART)
│   │   ├── crypto/               # Cryptography service
│   │   ├── locale/               # Localization service
│   │   ├── region/               # Region settings (frequency bands, etc.)
│   │   ├── expansion/           # Expansion board service
│   │   ├── dolphin/             # Dolphin animations service
│   │   ├── application.fam       # App manifest for services
│   │   └── applications.h        # Service/app registration header
│   │
│   ├── settings/                  # Settings apps (wifi, bluetooth, nfc, etc.)
│   ├── debug/                     # Debug/testing apps
│   ├── drivers/                   # Built-in drivers (st25r3916, cc1101, etc.)
│   ├── examples/                  # Example code (thermo, number_input, images, etc.)
│   └── external/                   # External FAP applications (games, tools, etc.)
│       ├── qrcode/
│       ├── wifi_scanner/
│       ├── ...
│       └── application.fam        # External app manifest
│
├── applications_user/              # User-installed external applications
│   ├── README.md
│   └── .gitignore
│
├── lib/                           # Core protocol libraries and drivers
│   ├── FreeRTOS-Kernel/           # Real-time OS
│   ├── stm32wb_hal/               # STM32 HAL (hardware abstraction)
│   ├── stm32wb_cmsis/             # STM32 CMSIS ( Cortex Microcontroller)
│   ├── mbedtls/                   # TLS/crypto library
│   ├── fatfs/                     # FAT file system (SD card)
│   ├── u8g2/                      # OLED display driver (SH1106/SSD1306)
│   ├── nfc/                       # NFC protocol stack
│   ├── subghz/                    # Sub-GHz protocol stack (CC1101)
│   ├── infrared/                   # IR protocol stack
│   ├── lfrfid/                    # Low-frequency RFID protocols
│   ├── ibutton/                   # iButton/1-Wire protocols
│   ├── cc1101/                    # CC1101 radio driver
│   ├── pn532/                     # PN532 NFC driver (I2C)
│   ├── pcf8574/                   # PCF8574 I2C expander driver
│   ├── flipper_format/            # Flipper file format (.fur) handling
│   ├── flipper_application/       # FAP (Flipper Application) framework
│   ├── update_util/               # Update/package utilities
│   └── ... (many more protocol/libs)
│
├── furi/                          # FURI core runtime
│   ├── core/                      # Core kernel (threads, messages, timers)
│   ├── flipper.c / flipper.h      # Main entry point
│   ├── furi.c / furi.h            # Core API
│   └── SConscript                 # FURI build config
│
├── targets/                       # Hardware target definitions
│   ├── f7/                        # DIY board target (TARGET_HW=7, STM32WB55CGU6)
│   │   ├── furi_hal/              # Hardware abstraction layer (GPIO, SPI, I2C, etc.)
│   │   │   ├── furi_hal_spi.c/h   # SPI bus (display, CC1101, SD card sharing)
│   │   │   ├── furi_hal_i2c.c/h   # I2C bus (OLED, PCF8574, PN532)
│   │   │   ├── furi_hal_gpio.c/h  # GPIO pins
│   │   │   ├── furi_hal_pcf8574.c/h # PCF8574 driver for buttons/vibro/buzzer
│   │   │   ├── furi_hal_pn532.c/h # PN532 NFC driver
│   │   │   ├── furi_hal_subghz.c/h # Sub-GHz (CC1101) driver
│   │   │   ├── furi_hal_nfc.c/h  # NFC subsystem
│   │   │   ├── furi_hal_resources.c/h # Pin macros (CC1101_CS, SD_CS, etc.)
│   │   │   └── furi_hal_target_hw.h # Target hardware config
│   │   ├── boards/
│   │   │   └── custom_pn532_board.mk  # PN532-enabled board config
│   │   ├── src/                   # Target-specific startup/boot
│   │   │   ├── main.c             # Main entry point for firmware
│   │   │   ├── dfu.c              # DFU bootloader
│   │   │   ├── recovery.c         # Recovery mode
│   │   │   └── stm32wb55_startup.c # Startup code
│   │   ├── api_symbols.csv        # API symbols for SDK compatibility check
│   │   ├── application_ext.ld     # Linker script for external apps
│   │   ├── stm32wb55xx_flash.ld   # Flash memory layout
│   │   ├── stm32wb55xx_ram_fw.ld  # RAM layout
│   │   └── target.json             # Target metadata
│   │
│   └── furi_hal_include/          # HAL header search path
│
├── assets/                        # Runtime assets (icons, animations, protobuf)
│   ├── icons/                     # UI icons
│   ├── dolphin/                   # Dolphin animation frames
│   ├── packs/                     # Asset packs (infrared, subghz, nfc)
│   ├── protobuf/                  # Protobuf definitions
│   ├── slideshow/                 # Update slideshow images
│   └── SConscript                 # Asset build script
│
├── scripts/                       # Build/utility scripts
│   ├── toolchain/                 # Toolchain setup (fbtenv.cmd, fbtenv.sh)
│   ├── fbt/                       # SCons tool modules
│   ├── fbt_tools/                 # Build tool plugins (fwbin, openocd, etc.)
│   ├── flipper/                  # Flipper-specific scripts
│   ├── version.py                # Version stamping script
│   ├── storage.py                # SD card operations
│   ├── lint.py                   # Linting script (C, Python, images)
│   ├── imglint.py                # Image linting
│   ├── serial_cli.py             # Serial CLI
│   ├── flash.py                  # Flash utility
│   └── ... (many utility scripts)
│
├── site_scons/                    # SCons configuration modules
│   ├── environ.scons              # Core environment setup
│   ├── cc.scons                   # C/C++ compiler configuration
│   ├── commandline.scons          # Command-line option handling
│   ├── firmwareopts.scons         # Firmware-specific options
│   ├── extapps.scons              # External apps build config
│   └── fbt_extra/                 # Extra build utilities
│
├── toolchain/                     # Embedded toolchain (ARM GCC)
│
├── build/                        # Build output directory (generated)
│   ├── f7-debug/                  # Debug firmware output
│   ├── f7-update-*/              # Update package output
│   └── resources/                # Packed resources
│
├── dist/                         # Distribution output (generated)
│
├── documentation/                # Doxygen documentation
│
├── .github/                      # GitHub workflows and assets
│   ├── workflows/
│   │   ├── build.yml            # CI build pipeline
│   │   ├── lint.yml             # CI lint pipeline
│   │   └── ...
│   └── assets/                  # Social media assets
│
├── .vscode/                     # VSCode configuration
│
└── mics/                        # Hardware photos and misc images
```

---

## Key Files and Their Roles

### Build System Files

| File | Description | Dependencies |
|------|-------------|-------------|
| `SConstruct` | Main SCons entry point - defines all build targets (flash, debug, lint, dist, etc.) | `site_scons/*`, `fbt_options.py` |
| `firmware.scons` | Builds firmware ELF/BIN/HEX/DFU - compiles libs, assets, apps, links | `site_scons/*.scons`, `lib/`, `assets/`, `targets/` |
| `fbt_options.py` | Build configuration - TARGET_HW=7, COPRO_STACK_*, DEBUG, COMPACT flags | Referenced by SConstruct |
| `fbt` / `fbt.cmd` | Wrapper script that invokes SCons with correct environment | Calls `scripts/toolchain/fbtenv.cmd` |

### Core Firmware Files

| File | Description | Dependencies |
|------|-------------|-------------|
| `furi/flipper.c` | Main firmware entry point - initializes FURI, starts services | `furi/core/*`, FreeRTOS |
| `furi/furi.c` | Core FURI API - thread management, messaging, timers | STM32 HAL |
| `applications/services/desktop/desktop.c` | Main menu UI service | `gui/`, `loader/`, `storage/` |
| `applications/services/loader/loader.c` | App launcher - loads/starts applications by name | `gui/`, `storage/` |
| `applications/services/gui/gui.c` | Graphics subsystem - ViewPort, canvas, icons | `furi/core/`, `assets/icons/` |

### Hardware Abstraction Layer

| File | Description | Hardware |
|------|-------------|----------|
| `targets/f7/furi_hal/furi_hal_resources.h` | Pin macro definitions (CC1101_CS, SD_CS, PCF8574_INT, etc.) | STM32WB55 |
| `targets/f7/furi_hal/furi_hal_pcf8574.c` | PCF8574 I2C expander driver - buttons, buzzer, vibro | PCF8574 @ 0x20 |
| `targets/f7/furi_hal/furi_hal_spi.c` | SPI1 driver - shared by display, CC1101, SD card | SPI1 (PA5-7, PB5-6) |
| `targets/f7/furi_hal/furi_hal_subghz.c` | Sub-GHz radio driver for CC1101 | CC1101 (SPI1 + PA1 IRQ) |
| `targets/f7/furi_hal/furi_hal_nfc.c` | NFC subsystem - bridges PN532/I2C to nfc lib | PN532 @ 0x24 on I2C1 |
| `targets/f7/furi_hal/furi_hal_i2c.c` | I2C1 driver - OLED, PCF8574, PN532 | I2C1 (PA9, PB9) |

### Application Structure

| File/Directory | Description | Entry Point |
|----------------|-------------|-------------|
| `applications/main/nfc/` | NFC application | `application.c` (view NFC tags, read/write) |
| `applications/main/subghz/` | Sub-GHz application | `application.c` (read/send 433-915MHz) |
| `applications/main/infrared/` | IR application | `application.c` (universal remote) |
| `applications/main/archive/` | File browser | `application.c` (browse SD files) |
| `applications/external/` | ~250+ user apps | Each has `application.c` + `application.fam` |

---

## Major Directory Summaries

### `applications/`
Contains all Flipper applications. Organized into:
- **main/**: Core apps (archive, subghz, nfc, infrared, gpio, ibutton, lfrfid, bad_usb, u2f)
- **services/**: Background services (desktop, gui, loader, storage, power, bt, etc.)
- **settings/**: Configuration apps
- **external/**: ~250+ community/contributed applications (FAP format)
- **examples/**: Example code for developers
- **drivers/**: Built-in hardware drivers

### `lib/`
Protocol stacks and drivers:
- **nfc/**: NFC protocol stack (ISO14443, ISO15693, Felica)
- **subghz/**: Sub-GHz protocols (433-915MHz)
- **infrared/**: IR protocols (NEC, Samsung, Sony, etc.)
- **lfrfid/**: Low-frequency RFID (EM4100, HID, Indala)
- **cc1101/**: CC1101 radio driver
- **u8g2/**: OLED display driver
- **fatfs/**: FAT filesystem for SD cards
- **flipper_application/**: FAP plugin framework

### `targets/f7/furi_hal/`
Hardware abstraction for the DIY board:
- **GPIO/I2C/SPI/UART** drivers
- **PCF8574** driver for buttons/buzzer/vibration
- **PN532** driver for NFC
- **CC1101** driver for sub-GHz
- **Resources** (pin definitions, interrupt handlers)

### `scripts/`
Build and utility scripts:
- **toolchain/**: fbtenv.cmd/sh - loads ARM GCC toolchain
- **fbt_tools/**: SCons tools (fwbin, openocd, jlink, etc.)
- **flipper/**: Runtime scripts (storage.py, serial_cli.py)

### `site_scons/`
SCons build modules:
- **environ.scons**: Core environment (compiler flags, paths)
- **cc.scons**: C/C++ toolchain configuration
- **commandline.scons**: CLI argument parsing
- **firmwareopts.scons**: App-specific build flags
- **extapps.scons**: External app (FAP) build configuration

---

## Hardware Configuration (DIY Board)

### Key Peripherals

| Peripheral | Interface | Address/Config |
|------------|-----------|----------------|
| OLED Display | I2C1 | SH1106/SSD1306 |
| PCF8574 | I2C1 | 0x20 (buttons P0-P5, vibro P6, buzzer P7) |
| PN532 NFC | I2C1 | 0x48 (7-bit) / 0x90-0x91 (8-bit) |
| CC1101 Sub-GHz | SPI1 | CS=PA15, IRQ=PA1 |
| SD Card | SPI1 | CS=PA10 |
| IR TX/RX | GPIO | TX=PA8, RX=PA0 |
| iButton | 1-Wire | PA3 |

### SPI1 Bus Contention

The SPI1 bus is shared between display (OLED), CC1101, and SD card. This causes occasional ViewPort lockup warnings - see `view_port.c:208` and `AGENTS.md` Storage Optimization TODOs.

---

## Build Commands Reference

```bash
./fbt                              # Debug firmware build
./fbt flash                        # Build + flash via ST-Link
./fbt flash_usb_full              # Build + flash via USB DFU
./fbt updater_package             # Create OTA update bundle
./fbt copro_dist                  # Package Core2 BLE stack
./fbt fap_qrcode                  # Build single external app
./fbt fap_dist                    # Build all external apps
./fbt lint_all                    # Run all linters
./fbt format_all                  # Format all code
./fbt debug                       # Debug with GDB + OpenOCD
```