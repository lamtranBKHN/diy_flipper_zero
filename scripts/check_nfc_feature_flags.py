#!/usr/bin/env python3
"""
Verify all NFC protocol feature flags match their scene handler implementations.

Parses each protocol's support .c file and cross-references feature flags
against scene on_enter handlers. Reports mismatches as errors.

Usage:
    python scripts/check_nfc_feature_flags.py
    python scripts/check_nfc_feature_flags.py --ci    # exit 1 on any error
"""

import argparse
import os
import re
import sys

PROTOCOL_DIR = os.path.join(
    os.path.dirname(__file__),
    "..",
    "applications",
    "main",
    "nfc",
    "helpers",
    "protocol_support",
)

# Ordered list of all 17 protocols — name matches the directory name
PROTOCOLS = [
    "iso14443_3a",
    "iso14443_3b",
    "iso14443_4a",
    "iso14443_4b",
    "iso15693_3",
    "felica",
    "mf_ultralight",
    "mf_classic",
    "mf_plus",
    "mf_desfire",
    "st25tb",
    "ntag4xx",
    "type_4_tag",
    "slix",
    "emv",
    "srix",
    "jewel",
]

# Feature flag name → bit position mapping (from nfc_protocol_support_common.h)
FEATURE_BITS = {
    "NfcProtocolFeatureEmulateUid": 0,
    "NfcProtocolFeatureEmulateFull": 1,
    "NfcProtocolFeatureEditUid": 2,
    "NfcProtocolFeatureMoreInfo": 3,
    "NfcProtocolFeatureWrite": 4,
}

# Scene → which feature flags REQUIRE a non-empty handler
FEATURE_TO_SCENE = {
    "scene_more_info": ["NfcProtocolFeatureMoreInfo"],
    "scene_emulate": ["NfcProtocolFeatureEmulateUid", "NfcProtocolFeatureEmulateFull"],
    "scene_write": ["NfcProtocolFeatureWrite"],
}

# All scenes we check (info and read are always present, skip them)
SCENES_TO_CHECK = ["scene_more_info", "scene_emulate", "scene_write"]

EMPTY_HANDLER = "nfc_protocol_support_common_on_enter_empty"


def parse_features_line(text):
    """Extract the .features = line and parse feature flag names."""
    # Match: .features = NfcProtocolFeatureA | NfcProtocolFeatureB,
    match = re.search(r"\.features\s*=\s*([^;]+?)\s*,", text, re.MULTILINE | re.DOTALL)
    if not match:
        return set()
    features_expr = match.group(1).strip()
    # Split by | and extract flag names
    flags = set()
    for token in features_expr.split("|"):
        token = token.strip()
        if token in FEATURE_BITS:
            flags.add(token)
    return flags


def parse_scene_handlers(text, protocol_name):
    """Extract all .scene_xxx blocks and their on_enter handler names."""
    handlers = {}

    # Pattern: .scene_xxx =
    #     {
    #         .on_enter = func_name,
    scene_pattern = re.compile(
        r"\.(\w+)\s*=\s*\{" r"\s*\.on_enter\s*=\s*(\w+)\s*,", re.MULTILINE
    )

    for match in scene_pattern.finditer(text):
        scene_name = match.group(1)
        handler = match.group(2)
        handlers[scene_name] = handler
    return handlers


def check_protocol(protocol_dir, protocol_name):
    """Check a single protocol's feature flags vs scene handlers."""
    c_file = os.path.join(protocol_dir, f"{protocol_name}.c")
    if not os.path.isfile(c_file):
        return [], [f"File not found: {c_file}"]

    with open(c_file, "r", encoding="utf-8") as f:
        text = f.read()

    flags = parse_features_line(text)
    handlers = parse_scene_handlers(text, protocol_name)

    errors = []
    warnings = []

    # Check each scene that requires a feature flag
    for scene in SCENES_TO_CHECK:
        required_flags = FEATURE_TO_SCENE.get(scene, [])
        handler = handlers.get(scene, EMPTY_HANDLER)
        is_empty = handler == EMPTY_HANDLER
        has_flag = any(f in flags for f in required_flags)

        if has_flag and is_empty:
            errors.append(
                f"[{protocol_name}] {scene}: has feature flag{'s' if len(required_flags) > 1 else ''} "
                f"{'|'.join(required_flags)} but handler is empty (nfc_protocol_support_common_on_enter_empty)"
            )
        elif (
            not has_flag
            and not is_empty
            and scene
            in (
                "scene_more_info",
                "scene_emulate",
                "scene_write",
            )
        ):
            # Non-empty handler without the corresponding feature flag
            warnings.append(
                f"[{protocol_name}] {scene}: has custom handler ({handler}) "
                f"but no {'/'.join(required_flags)} feature flag"
            )

    return errors, warnings, flags, handlers


def format_feature_table(results):
    """Build a formatted feature matrix table similar to docs."""
    lines = []
    lines.append(
        f"{'#':>2} {'Protocol':<20} {'Info':>5} {'MoreInfo':>9} {'Emulate':>8} {'Write':>6}"
    )
    lines.append("-" * 58)

    for i, (name, flags, handlers) in enumerate(results, 1):
        has_more = "NfcProtocolFeatureMoreInfo" in flags
        has_emu_uid = "NfcProtocolFeatureEmulateUid" in flags
        has_emu_full = "NfcProtocolFeatureEmulateFull" in flags
        has_write = "NfcProtocolFeatureWrite" in flags

        more_info_val = "[OK]" if has_more else "--"
        emu_val = "--"
        if has_emu_full:
            emu_val = "FULL"
        elif has_emu_uid:
            emu_val = "UID"
        write_val = "[OK]" if has_write else "--"

        display_name = name.replace("_", " ").title()
        lines.append(
            f"{i:>2} {display_name:<20} {'[OK]':>5} {more_info_val:>9} {emu_val:>8} {write_val:>6}"
        )

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Verify NFC protocol feature flags match scene handlers"
    )
    parser.add_argument(
        "--ci",
        action="store_true",
        help="Exit with code 1 on any error (for CI pipelines)",
    )
    parser.add_argument(
        "--table",
        action="store_true",
        help="Print feature matrix table after verification",
    )
    args = parser.parse_args()

    all_errors = []
    all_warnings = []
    results = []
    total = 0

    for protocol in PROTOCOLS:
        protocol_dir = os.path.join(PROTOCOL_DIR, protocol)
        if not os.path.isdir(protocol_dir):
            all_errors.append(f"[{protocol}] Directory not found: {protocol_dir}")
            continue

        errors, warnings, flags, handlers = check_protocol(protocol_dir, protocol)
        total += 1
        all_errors.extend(errors)
        all_warnings.extend(warnings)
        results.append((protocol, flags, handlers))

    # Report results
    print(f"\nChecked {total}/{len(PROTOCOLS)} protocol support files\n")

    if all_warnings:
        print("=== Warnings ===")
        for w in all_warnings:
            print(f"  WARN  {w}")
        print()

    if all_errors:
        print("=== ERRORS ===")
        for e in all_errors:
            print(f"  FAIL  {e}")
        print()
    else:
        print("No feature flag / scene handler mismatches detected. [OK]\n")

    if args.table:
        print("=== Feature Matrix ===")
        print(format_feature_table(results))
        print()

    # Summary counts
    emu_uid_count = sum(1 for _, f, _ in results if "NfcProtocolFeatureEmulateUid" in f)
    emu_full_count = sum(
        1 for _, f, _ in results if "NfcProtocolFeatureEmulateFull" in f
    )
    write_count = sum(1 for _, f, _ in results if "NfcProtocolFeatureWrite" in f)
    more_info_count = sum(1 for _, f, _ in results if "NfcProtocolFeatureMoreInfo" in f)

    print(
        f"Summary: Info={total}/{total} MoreInfo={more_info_count}/{total} "
        f"Emulate={emu_uid_count + emu_full_count}/{total} "
        f"({emu_uid_count} UID, {emu_full_count} Full) "
        f"Write={write_count}/{total}"
    )

    if args.ci and all_errors:
        return 1
    return 0 if not all_errors else 1


if __name__ == "__main__":
    sys.exit(main())
