# fbt Build Commands

## Debug build
`./fbt`

## Release build
`./fbt TARGET_HW=7 DEBUG=0 COMPACT=1 copro_dist updater_package fap_dist`

## Flash firmware
- SWD: `./fbt flash`
- USB DFU: `./fbt flash_usb_full` (device in DFU mode)

## Unit tests
`./fbt FIRMWARE_APP_SET=unit_tests`

## Lint & Format
- Lint all: `./fbt lint_all`
- Format all: `./fbt format_all`
- Python only: `./fbt lint_py` / `./fbt format_py`

## OTA & Distribution
- Update package: `./fbt updater_package`
- Core2 BLE stack: `./fbt copro_dist`
- All external apps: `./fbt fap_dist`
- Single app: `./fbt fap_{APPID}` (e.g. `./fbt fap_qrcode`)

## Debug
- GDB + OpenOCD: `./fbt debug`
- Black Magic probe: `./fbt blackmagic`
- CLI session over USB: `./fbt cli`

## Key Options
`COMPACT=1` `DEBUG=0` `VERBOSE=1` `FIRMWARE_APP_SET=unit_tests` `FBT_NO_SYNC=1`
