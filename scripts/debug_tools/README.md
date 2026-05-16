# Debug Tools

This directory contains debugging utilities for Flipper Zero firmware development.

## Tools

### crash_dump.py

Analyzes crash dumps from `__furi_check_registers` and `__furi_check_message`.

**Usage:**
```bash
python crash_dump.py <dump_file>
python crash_dump.py crash.dump --verbose
python crash_dump.py crash.dump --registers-only
python crash_dump.py crash.dump --fault-only
```

**Output:**
- Register values (R0-R12, SP, LR, PC, XPSR)
- Fault status (CFSR, HFSR, DFSR, AFSR, BFAR, MMFAR)
- Crash message (if present)
- Summary with fault type and address

### flipperapps.py

GDB helper for loading debug symbols for Flipper external apps (`.fap` files).

**Usage in GDB:**
```gdb
(gdb) fap-set-debug-elf-root /path/to/faps
```

Automatically syncs debug symbols when external apps load/unload with CRC32 validation.

### flipperversion.py

GDB command to read firmware version from RTC backup registers.

**Usage in GDB:**
```gdb
(gdb) fw-version
```

## Integration

These tools are automatically loaded by GDB when debugging with `./fbt debug`.

## See Also

- `scripts/debug/` - OpenOCD configs, SVD files, FreeRTOS helpers
- `scripts/debug/FreeRTOS/` - FreeRTOS GDB commands
- `scripts/debug/PyCortexMDebug/` - Peripheral register inspection
- `documentation/Debugging.md` - Comprehensive debugging guide
- `documentation/Profiling.md` - Performance profiling methods
