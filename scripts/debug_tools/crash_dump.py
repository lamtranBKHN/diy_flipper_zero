#!/usr/bin/env python3
"""
Flipper Zero Crash Dump Analyzer

Analyzes crash dumps from __furi_check_registers and __furi_check_message.
Extracted from furi/core/check.c crash data structures.

Usage:
    python crash_dump.py <dump_file>
    python crash_dump.py --help
"""

import argparse
import struct
import sys
from pathlib import Path


class CrashDumpParser:
    """Parser for Flipper Zero crash dumps."""

    # Register offsets in crash dump structure
    REGISTERS = {
        "r0": 0x00,
        "r1": 0x04,
        "r2": 0x08,
        "r3": 0x0C,
        "r4": 0x10,
        "r5": 0x14,
        "r6": 0x18,
        "r7": 0x1C,
        "r8": 0x20,
        "r9": 0x24,
        "r10": 0x28,
        "r11": 0x2C,
        "r12": 0x30,
        "sp": 0x34,
        "lr": 0x38,
        "pc": 0x3C,
        "xpsr": 0x40,
    }

    # Fault status registers
    CFSR_OFFSET = 0x44
    HFSR_OFFSET = 0x48
    DFSR_OFFSET = 0x4C
    AFSR_OFFSET = 0x50
    BFAR_OFFSET = 0x54
    MMFAR_OFFSET = 0x58

    def __init__(self, data: bytes):
        """Initialize parser with crash dump data.

        Args:
            data: Raw crash dump bytes
        """
        self.data = data
        self.registers = {}
        self.fault_status = {}
        self.message = ""

    def parse(self) -> bool:
        """Parse crash dump.

        Returns:
            True if parsing successful, False otherwise
        """
        try:
            # Parse registers
            for reg_name, offset in self.REGISTERS.items():
                if offset + 4 <= len(self.data):
                    value = struct.unpack_from("<I", self.data, offset)[0]
                    self.registers[reg_name] = value

            # Parse fault status registers
            if self.CFSR_OFFSET + 4 <= len(self.data):
                self.fault_status["cfsr"] = struct.unpack_from(
                    "<I", self.data, self.CFSR_OFFSET
                )[0]
            if self.HFSR_OFFSET + 4 <= len(self.data):
                self.fault_status["hfsr"] = struct.unpack_from(
                    "<I", self.data, self.HFSR_OFFSET
                )[0]
            if self.DFSR_OFFSET + 4 <= len(self.data):
                self.fault_status["dfsr"] = struct.unpack_from(
                    "<I", self.data, self.DFSR_OFFSET
                )[0]
            if self.AFSR_OFFSET + 4 <= len(self.data):
                self.fault_status["afsr"] = struct.unpack_from(
                    "<I", self.data, self.AFSR_OFFSET
                )[0]
            if self.BFAR_OFFSET + 4 <= len(self.data):
                self.fault_status["bfar"] = struct.unpack_from(
                    "<I", self.data, self.BFAR_OFFSET
                )[0]
            if self.MMFAR_OFFSET + 4 <= len(self.data):
                self.fault_status["mmfar"] = struct.unpack_from(
                    "<I", self.data, self.MMFAR_OFFSET
                )[0]

            # Parse message (if present)
            message_offset = 0x5C
            if message_offset < len(self.data):
                # Message is null-terminated string
                end = self.data.find(b"\x00", message_offset)
                if end != -1:
                    self.message = self.data[message_offset:end].decode(
                        "utf-8", errors="ignore"
                    )

            return True
        except Exception as e:
            print(f"Error parsing crash dump: {e}", file=sys.stderr)
            return False

    def print_registers(self):
        """Print register values."""
        print("\n=== Registers ===")
        for reg in [
            "r0",
            "r1",
            "r2",
            "r3",
            "r4",
            "r5",
            "r6",
            "r7",
            "r8",
            "r9",
            "r10",
            "r11",
            "r12",
            "sp",
            "lr",
            "pc",
            "xpsr",
        ]:
            if reg in self.registers:
                print(f"  {reg.upper():4s}: 0x{self.registers[reg]:08X}")

    def print_fault_status(self):
        """Print fault status information."""
        print("\n=== Fault Status ===")

        if "cfsr" in self.fault_status:
            cfsr = self.fault_status["cfsr"]
            print(f"  CFSR: 0x{cfsr:08X}")

            # Memory Management Fault
            if cfsr & 0x000000FF:
                print("    Memory Management Fault:")
                mmfsr = cfsr & 0xFF
                if mmfsr & 0x01:
                    print("      Instruction access violation")
                if mmfsr & 0x02:
                    print("      Data access violation")
                if mmfsr & 0x08:
                    print("      MemManage Fault on unstacking")
                if mmfsr & 0x10:
                    print("      MemManage Fault on stacking")
                if mmfsr & 0x20:
                    print("      MemManage Fault during FP lazy state preservation")
                if mmfsr & 0x80:
                    print("      MemManage Fault address valid")

            # Bus Fault
            if cfsr & 0x0000FF00:
                print("    Bus Fault:")
                bfsr = (cfsr >> 8) & 0xFF
                if bfsr & 0x01:
                    print("      Instruction bus error")
                if bfsr & 0x02:
                    print("      Precise data bus error")
                if bfsr & 0x04:
                    print("      Imprecise data bus error")
                if bfsr & 0x08:
                    print("      Bus Fault on unstacking")
                if bfsr & 0x10:
                    print("      Bus Fault on stacking")
                if bfsr & 0x20:
                    print("      Bus Fault during FP lazy state preservation")
                if bfsr & 0x80:
                    print("      Bus Fault address valid")

            # Usage Fault
            if cfsr & 0xFFFF0000:
                print("    Usage Fault:")
                ufsr = (cfsr >> 16) & 0xFFFF
                if ufsr & 0x0001:
                    print("      Undefined instruction")
                if ufsr & 0x0002:
                    print("      Invalid state")
                if ufsr & 0x0004:
                    print("      Invalid PC load")
                if ufsr & 0x0008:
                    print("      No coprocessor")
                if ufsr & 0x0010:
                    print("      Unaligned access")
                if ufsr & 0x0020:
                    print("      Division by zero")

        if "hfsr" in self.fault_status:
            hfsr = self.fault_status["hfsr"]
            print(f"  HFSR: 0x{hfsr:08X}")
            if hfsr & 0x40000000:
                print("    Debug event")
            if hfsr & 0x80000000:
                print("    Escalation")

        if "bfar" in self.fault_status:
            print(f"  BFAR: 0x{self.fault_status['bfar']:08X}")

        if "mmfar" in self.fault_status:
            print(f"  MMFAR: 0x{self.fault_status['mmfar']:08X}")

    def print_message(self):
        """Print crash message."""
        if self.message:
            print("\n=== Message ===")
            print(f"  {self.message}")

    def print_summary(self):
        """Print crash summary."""
        print("\n=== Summary ===")

        pc = self.registers.get("pc", 0)
        lr = self.registers.get("lr", 0)
        sp = self.registers.get("sp", 0)

        print(f"  PC: 0x{pc:08X} (Program Counter)")
        print(f"  LR: 0x{lr:08X} (Link Register)")
        print(f"  SP: 0x{sp:08X} (Stack Pointer)")

        # Determine fault type
        if "cfsr" in self.fault_status:
            cfsr = self.fault_status["cfsr"]

            if cfsr & 0x000000FF:
                print("  Fault Type: Memory Management Fault")
                if cfsr & 0x80 and "mmfar" in self.fault_status:
                    print(f"  Fault Address: 0x{self.fault_status['mmfar']:08X}")
            elif cfsr & 0x0000FF00:
                print("  Fault Type: Bus Fault")
                if cfsr & 0x0200 and "bfar" in self.fault_status:
                    print(f"  Fault Address: 0x{self.fault_status['bfar']:08X}")
            elif cfsr & 0xFFFF0000:
                print("  Fault Type: Usage Fault")
            else:
                print("  Fault Type: Unknown")

        if self.message:
            print(f"  Message: {self.message}")


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="Analyze Flipper Zero crash dumps",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python crash_dump.py crash.dump
  python crash_dump.py /path/to/crash.dump --verbose
        """,
    )

    parser.add_argument("dump_file", help="Crash dump file to analyze")
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Show detailed information"
    )
    parser.add_argument(
        "-r", "--registers-only", action="store_true", help="Show only registers"
    )
    parser.add_argument(
        "-f", "--fault-only", action="store_true", help="Show only fault status"
    )

    args = parser.parse_args()

    # Read crash dump file
    dump_path = Path(args.dump_file)
    if not dump_path.exists():
        print(f"Error: File not found: {args.dump_file}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(dump_path, "rb") as f:
            data = f.read()
    except Exception as e:
        print(f"Error reading file: {e}", file=sys.stderr)
        sys.exit(1)

    # Parse crash dump
    parser = CrashDumpParser(data)
    if not parser.parse():
        print("Error: Failed to parse crash dump", file=sys.stderr)
        sys.exit(1)

    # Print results
    if args.registers_only:
        parser.print_registers()
    elif args.fault_only:
        parser.print_fault_status()
    else:
        parser.print_summary()
        if args.verbose:
            parser.print_registers()
            parser.print_fault_status()
            parser.print_message()


if __name__ == "__main__":
    main()
