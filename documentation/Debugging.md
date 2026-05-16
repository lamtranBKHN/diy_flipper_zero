# Debugging Flipper Zero Firmware

## Prerequisites

### Required Tools

- **arm-none-eabi-gdb-py3** - Python-enabled GDB (required for FreeRTOS and PyCortexMDebug helpers)
- **Debug Probe** - One of:
  - ST-Link v2/v3 (most common)
  - Blackmagic Probe (WiFi devboard or standalone)
  - J-Link (Segger)
  - CMSIS-DAP compatible probe
- **VS Code** (optional) - With C/C++ extension for integrated debugging

### Installing GDB with Python Support

The toolchain included with fbt should include `arm-none-eabi-gdb-py3`. Verify:

```bash
arm-none-eabi-gdb-py3 --version
```

If not available, install via your package manager or download from ARM.

## Build Commands

### Debug Build

```bash
# Build with debug symbols (now default with DEBUG=1)
./fbt

# Explicit debug build
./fbt DEBUG=1

# Build without optimization (slower but better debugging)
./fbt DEBUG=1 COMPACT=0
```

### Flash and Debug

```bash
# Flash firmware and attach GDB (auto-detects probe)
./fbt debug

# Flash and attach with specific probe
./fbt SWD_TRANSPORT=stlink debug
./fbt SWD_TRANSPORT=cmsis-dap debug
./fbt SWD_TRANSPORT=blackmagic debug

# Flash and attach with J-Link
./fbt SWD_TRANSPORT=jlink debug

# Flash without attaching
./fbt flash
```

### Build Verification

```bash
# Verify debug symbols present
arm-none-eabi-nm build/latest/firmware.elf | grep furi_check

# Check file size (debug builds are ~2-3x larger)
ls -lh build/latest/firmware.bin
```

## GDB Commands

### Basic Commands

```gdb
# Load firmware
(gdb) target remote :3333
(gdb) load
(gdb) continue

# Set breakpoint
(gdb) break furi_hal_nfc_poll

# Set hardware breakpoint (limited number available)
(gdb) hbreak furi_hal_nfc_poll

# Set watchpoint on variable
(gdb) watch nfc_device->state

# Set read watchpoint
(gdb) rwatch nfc_device->buffer

# Set access watchpoint (read or write)
(gdb) awatch nfc_device->buffer

# Continue execution
(gdb) continue
(gdb) c

# Step instruction
(gdb) stepi
(gdb) si

# Step over function
(gdb) next
(gdb) n

# Show backtrace
(gdb) bt

# Show registers
(gdb) info registers

# Show current thread
(gdb) info threads
```

### FreeRTOS Commands

FreeRTOS GDB helpers (loaded automatically):

```gdb
# Show all tasks
(gdb) show Task-List

# Show queue information
(gdb) show Queue-Info

# Show handle registry
(gdb) show Handle-Registry

# Show handle name by address
(gdb) show Handle-Name 0x20001234

# Show arbitrary linked list
(gdb) show List-Handle 0x20005678
```

### Peripheral Inspection (PyCortexMDebug)

```gdb
# List all peripherals
(gdb) svd_load stm32/STM32WB55_CM4.svd

# Show peripheral registers
(gdb) svd GPIOA

# Show specific register
(gdb) svd GPIOA MODER

# Show register in hex
(gdb) svd/x GPIOA MODER

# Show register in binary
(gdb) svd/t GPIOA MODER

# Show register as address
(gdb) svd/a GPIOA MODER
```

### Flipper-Specific Commands

```gdb
# Load FAP debug symbols
(gdb) fap-set-debug-elf-root /path/to/faps

# Show firmware version
(gdb) fw-version
```

## CLI Debug Commands

Connect via CLI:

```bash
./fbt cli
```

### System Control

```bash
# Enable debug mode (enables SWD pins)
> sysctl debug 1

# Disable debug mode (releases SWD pins for GPIO)
> sysctl debug 0

# Check debug status
> sysctl debug
```

### Log Level Control

```bash
# Set log level to debug
> sysctl log_level debug

# Set log level to trace (most verbose)
> sysctl log_level trace

# Set log level to info
> sysctl log_level info

# Set log level to error (least verbose)
> sysctl log_level error

# View current log level
> sysctl log_level

# View logs
> log
```

### Available Log Levels

- `none` - No logging
- `error` - Errors only
- `warn` - Warnings and errors
- `info` - Informational messages
- `debug` - Debug messages
- `trace` - Trace messages (most verbose)

## VS Code Debugging

### Configurations

Four debug configurations are available (generated via `./fbt vscode_dist`):

| Configuration | Probe | Interface |
|---------------|-------|-----------|
| Attach FW (ST-Link) | ST-Link | SWD |
| Attach FW (DAP) | CMSIS-DAP | SWD |
| Attach FW (blackmagic) | Blackmagic | SWD |
| Attach FW (JLink) | J-Link | SWD |

All configurations load:
- SVD file for peripheral inspection
- FreeRTOS plugin for task inspection
- FAP symbol helper for external apps
- Firmware version reader

### Using VS Code

1. Generate VS Code config:
   ```bash
   ./fbt vscode_dist
   ```

2. Open VS Code in project directory

3. Press F5 or go to Run and Debug

4. Select desired configuration

5. Use VS Code debugging UI (breakpoints, watch window, call stack)

## Debug Applications

### Unit Tests

```bash
# Build and flash unit tests
./fbt FIRMWARE_APP_SET=unit_tests flash

# Run unit tests via CLI
./fbt cli
> unit_tests
```

### Crash Test

Tests all fatal exception paths:

```bash
# Build and flash
./fbt FIRMWARE_APP_SET=unit_tests flash

# Run crash test
./fbt cli
> crash_test
```

### Hardware Tests

Various hardware test apps available:

- `bt_debug_app` - Bluetooth testing
- `battery_test_app` - Battery monitoring
- `display_test` - Display testing
- `infrared_test` - IR signal testing
- `lfrfid_debug` - LF RFID debugging
- `subghz_test` - SubGhz testing
- `uart_echo` - UART loopback
- `speaker_debug` - Speaker testing
- `usb_test` - USB enumeration

## Debug Helper Macros

Include `furi/core/debug.h` for convenience macros:

```c
#include <furi/core/debug.h>

// Timing measurement
FURI_DEBUG_TIMING_START(nfc_operation);
// ... code ...
FURI_DEBUG_TIMING_END(nfc_operation, "NFC");

// Conditional breakpoint
FURI_DEBUG_BREAK_IF(nfc_device->state == NFC_STATE_ERROR);

// Stack check
FURI_DEBUG_STACK_CHECK();
```

## Common Debugging Scenarios

### NFC Not Responding

```bash
# Enable debug logging
> sysctl log_level debug

# Check NFC HAL state
(gdb) svd PN532

# Set breakpoint in poller
(gdb) hbreak furi_hal_nfc_poll

# Check I2C bus
(gdb) svd I2C1 ISR
```

### I2C Communication Issues

```bash
# Check I2C status
(gdb) svd I2C1 ISR
(gdb) svd I2C1 CR1
(gdb) svd I2C1 CR2

# Set breakpoint in I2C handler
(gdb) hbreak furi_hal_i2c_isr

# Check GPIO state
(gdb) svd GPIOB MODER
(gdb) svd GPIOB ODR
```

### Task Stuck

```bash
# Show all tasks
(gdb) show Task-List

# Check task state
(gdb) info threads

# Set breakpoint in task
(gdb) break furi_thread_entry

# Check stack usage
> top
```

### Memory Corruption

```bash
# Enable heap checking
> sysctl debug 1

# Run crash test
> crash_test

# Check heap
> free

# Set watchpoint on memory
(gdb) watch *(uint32_t*)0x20005000
```

## Troubleshooting

### GDB Won't Connect

1. Check probe is detected:
   ```bash
   # For ST-Link
   lsusb | grep STMicro

   # For Blackmagic
   ls /dev/ttyACM* | grep blackmagic
   ```

2. Check OpenOCD config:
   ```bash
   cat scripts/debug/stm32wbx.cfg
   ```

3. Try manual OpenOCD:
   ```bash
   openocd -f interface/cmsis-dap.cfg -f scripts/debug/stm32wbx.cfg
   ```

### No Debug Symbols

1. Verify DEBUG=1 in fbt_options.py
2. Rebuild firmware:
   ```bash
   ./fbt clean
   ./fbt
   ```
3. Check ELF file:
   ```bash
   arm-none-eabi-nm build/latest/firmware.elf | grep furi_check
   ```

### Hardware Breakpoints Not Working

1. Check if using hardware breakpoint:
   ```gdb
   hbreak function_name
   ```

2. Limited number of hardware breakpoints available (typically 6)
3. Use software breakpoints for code in RAM:
   ```gdb
   break function_name
   ```

### FreeRTOS Commands Not Available

1. Verify using Python-enabled GDB:
   ```bash
   arm-none-eabi-gdb-py3 --version
   ```

2. Check FreeRTOS plugin loaded:
   ```gdb
   help show
   ```

## Additional Resources

- `documentation/FuriHalDebugging.md` - GPIO signal debugging
- `documentation/devboard/Debugging via the Devboard.md` - Devboard debugging guide
- `scripts/debug/FreeRTOS/README.md` - FreeRTOS GDB commands
- `scripts/debug/PyCortexMDebug/README.md` - Peripheral inspection
- `documentation/Profiling.md` - Performance profiling methods
