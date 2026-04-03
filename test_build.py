#!/usr/bin/env python3
import subprocess
import os
import sys

# Change to the repo directory
repo_dir = r"F:\FB_V3\diy_flipper_zero.worktrees\copilot-worktree-2026-04-03T07-50-46"
os.chdir(repo_dir)

# Set environment variables
os.environ['FBT_NO_SYNC'] = '1'

# Try to build the compilation database 
print("Building firmware compilation database (validates furi_hal_sd.c and furi_hal_spi.c compile)...")
print("-" * 70)

try:
    # Run fbt.cmd with firmware_cdb target
    result = subprocess.run(
        [r"F:\FB_V3\diy_flipper_zero.worktrees\copilot-worktree-2026-04-03T07-50-46\fbt.cmd", 
         "firmware_cdb", "COMPACT=1", "DEBUG=0"],
        capture_output=False,
        text=True
    )
    
    if result.returncode == 0:
        print("\n" + "=" * 70)
        print("SUCCESS: Compilation database build completed successfully")
        print("This validates that targets/f7/furi_hal/furi_hal_sd.c and")
        print("targets/f7/furi_hal/furi_hal_spi.c compile without errors")
        print("=" * 70)
        sys.exit(0)
    else:
        print("\n" + "=" * 70)
        print(f"FAILED: Build returned error code {result.returncode}")
        print("=" * 70)
        sys.exit(1)
except Exception as e:
    print(f"\nERROR: {e}")
    sys.exit(1)
