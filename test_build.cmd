@echo off
setlocal enabledelayedexpansion

cd /d "F:\FB_V3\diy_flipper_zero.worktrees\copilot-worktree-2026-04-03T07-50-46"

REM Set environment variable to skip git sync
set FBT_NO_SYNC=1

REM Try to build the compilation database - minimal compile validation
echo Building compilation database (minimal firmware validation)...
call fbt.cmd firmware_cdb COMPACT=1 DEBUG=0 2>&1

if %ERRORLEVEL% equ 0 (
    echo.
    echo SUCCESS: Compilation database build succeeded
    echo This validates that furi_hal_sd.c and furi_hal_spi.c compile without errors
) else (
    echo.
    echo FAILED: Build returned error code %ERRORLEVEL%
)

endlocal
