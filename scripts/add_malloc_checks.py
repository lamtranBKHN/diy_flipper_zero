#!/usr/bin/env python3
"""
Add furi_check() after malloc(sizeof(...)) calls that lack null-pointer checks.

Scans .c files for patterns like:
    SomeType* instance = malloc(sizeof(SomeType));
    // (missing furi_check)

And inserts:
    SomeType* instance = malloc(sizeof(SomeType));
    furi_check(instance);

Handles nested allocs (instance->foo = malloc(...)) correctly by skipping them
(member pointer allocs are not the owning struct alloc).

Usage:
    python scripts/add_malloc_checks.py [--dry-run] [file1.c file2.c ...]
    # if no files given, reads from stdin paths
"""

import os
import re
import sys

# Directories to scan
NFC_DIRS = [
    "lib/nfc",
]

SUBGHZ_DIRS = [
    "lib/subghz",
]

FILES_TO_SKIP = {
    # Third-party / generated code
    "lib/nfc/protocols/iso14443_3a/iso14443_3a.c",  # already has furi_check
    "lib/nfc/protocols/iso14443_3a/iso14443_3a_poller.c",  # already has furi_check
    "lib/nfc/protocols/iso14443_3a/iso14443_3a_listener.c",  # already has furi_check
    "lib/nfc/protocols/iso14443_4a/iso14443_4a_poller.c",  # already has furi_check
    "lib/nfc/protocols/iso14443_4a/iso14443_4a_listener.c",  # already has furi_check
    "lib/nfc/protocols/iso15693_3/iso15693_3_poller.c",  # already has furi_check
    "lib/nfc/protocols/iso15693_3/iso15693_3_listener.c",  # already has furi_check
    "lib/nfc/protocols/felica/felica.c",  # already has furi_check in alloc
    "lib/nfc/protocols/felica/felica_poller.c",  # already has furi_check
    "lib/nfc/protocols/felica/felica_listener.c",  # already has furi_check
    "lib/nfc/protocols/mf_classic/mf_classic_poller.c",  # complex pattern, checked manually
    "lib/nfc/nfc_scanner.c",  # already has furi_check
    "lib/nfc/nfc_poller.c",  # already has furi_check
    "lib/nfc/nfc_listener.c",  # already has furi_check
    # SubGhz infra files that already have checks
    "lib/subghz/subghz_file_encoder_worker.c",
    "lib/subghz/receiver.c",
    "lib/subghz/subghz_keystore.c",
    "lib/subghz/environment.c",
    "lib/subghz/subghz_setting.c",
    "lib/subghz/subghz_worker.c",
    "lib/subghz/transmitter.c",
}


def find_malloc_sites(filepath):
    """Find all malloc(sizeof(...)) sites in a file and report their status."""
    with open(filepath, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    sites = []
    for i, line in enumerate(lines):
        # Match: some_variable = malloc(sizeof(...
        # But NOT: instance->member = malloc(sizeof(...  (nested alloc)
        stripped = line.rstrip()
        if "= malloc(sizeof(" in stripped and not stripped.strip().startswith("//"):
            # Check if it's a member alloc (instance->something = malloc)
            # or a pointer alloc (Type* instance = malloc / instance = malloc)
            malloc_part = stripped.split("= malloc(sizeof(")[0].rstrip()
            var_name_candidate = malloc_part.split()[-1] if malloc_part.split() else ""

            # Skip if it's a member alloc like instance->foo or arr[i]
            if "->" in var_name_candidate or "[" in var_name_candidate:
                continue
            if "." in var_name_candidate:
                continue

            # Check next non-blank, non-comment line for furi_check
            has_check = False
            check_var = None
            for j in range(i + 1, min(i + 4, len(lines))):
                next_line = lines[j].strip()
                if (
                    not next_line
                    or next_line.startswith("//")
                    or next_line.startswith("*")
                ):
                    continue
                # Check for furi_check(var_name)
                check_match = re.match(r"furi_check\s*\(\s*(\w+)\s*\)", next_line)
                if check_match:
                    has_check = True
                    check_var = check_match.group(1)
                # Also check for if(var_name)
                if_match = re.match(r"if\s*\(\s*(\w+)\s*\)", next_line)
                if if_match:
                    # This is an if() guard - note it but don't count as furi_check
                    if not has_check:
                        check_var = if_match.group(1)
                break

            sites.append(
                {
                    "lineno": i + 1,
                    "line": stripped,
                    "var_name": var_name_candidate,
                    "has_furi_check": has_check,
                    "has_if_guard": not has_check and check_var is not None,
                    "check_var": check_var,
                    "next_line_idx": j if not has_check else None,
                }
            )
    return sites, lines


def add_checks(filepath, dry_run=False):
    """Add furi_check() to a file where missing."""
    sites, lines = find_malloc_sites(filepath)

    if not sites:
        return 0  # no sites found

    # Only process sites that need checks
    to_fix = [s for s in sites if not s["has_furi_check"]]

    if not to_fix:
        return 0

    if dry_run:
        for site in to_fix:
            print(f"  {filepath}:{site['lineno']}: {site['line'].strip()}")
            print(f"       -> furi_check({site['var_name']});")
        return len(to_fix)

    # Apply fixes (process in reverse order to preserve line numbers)
    modifications = 0
    for site in reversed(to_fix):
        idx = site["lineno"] - 1  # 0-indexed
        indent = "    "

        # Determine indentation from the malloc line
        orig_line = lines[idx]
        leading_spaces = len(orig_line) - len(orig_line.lstrip())
        indent = " " * leading_spaces

        # Check if there's an if(var) guard that should be replaced
        if site["has_if_guard"]:
            # Find and replace the if() line with furi_check()
            next_idx = site["next_line_idx"]
            if next_idx and next_idx < len(lines):
                if_line = lines[next_idx].strip()
                if_match = re.match(r"if\s*\(\s*(\w+)\s*\)", if_line)
                if if_match and if_match.group(1) == site["var_name"]:
                    # Check the line after the if() - is it the reset call?
                    if next_idx + 1 < len(lines):
                        after_if = lines[next_idx + 1].strip()
                        if (
                            not after_if.startswith("return")
                            and not after_if.startswith("//")
                            and after_if != "}"
                        ):
                            # This looks like if(ctx) { something } - replace with furi_check(ctx);
                            lines[next_idx] = (
                                f"{indent}furi_check({site['var_name']});\n"
                            )
                            modifications += 1
                            continue

        # Insert furi_check line after the malloc line
        check_line = f"{indent}furi_check({site['var_name']});\n"
        lines.insert(idx + 1, check_line)
        modifications += 1

    if modifications > 0 and not dry_run:
        with open(filepath, "w", encoding="utf-8") as f:
            f.writelines(lines)

    return modifications


def collect_c_files(dirs):
    """Recursively collect all .c files from directories."""
    files = []
    for d in dirs:
        for root, _, filenames in os.walk(d):
            for fn in filenames:
                if fn.endswith(".c"):
                    files.append(os.path.join(root, fn))
    return files


def main():
    dry_run = "--dry-run" in sys.argv

    # Determine files to process
    if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
        files = [f for f in sys.argv[1:] if f.endswith(".c") and not f.startswith("--")]
    else:
        # Collect from standard directories
        files = collect_c_files(NFC_DIRS) + collect_c_files(SUBGHZ_DIRS)

    # Filter out skipped files
    files_to_check = []
    for f in files:
        rel_path = f.replace("\\", "/")
        if rel_path in FILES_TO_SKIP:
            continue
        # Check if the dir prefix matches a skip pattern
        skip = False
        for skip_file in FILES_TO_SKIP:
            if rel_path == skip_file or rel_path.endswith("/" + skip_file):
                skip = True
                break
        if not skip:
            files_to_check.append(f)

    total_fixes = 0
    files_modified = 0

    for filepath in sorted(files_to_check):
        # Normalize path
        filepath = filepath.replace("\\", "/")
        if not os.path.exists(filepath):
            print(f"WARNING: File not found: {filepath}", file=sys.stderr)
            continue

        fixes = add_checks(filepath, dry_run)
        if fixes > 0:
            if dry_run:
                print()
            else:
                print(f"  {filepath}: {fixes} fix(es) applied")
            total_fixes += fixes
            files_modified += 1

    mode = "DRY RUN - would fix" if dry_run else "Fixed"
    print(f"\n{mode} {total_fixes} sites across {files_modified} files.")

    return 0 if not dry_run or total_fixes == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
