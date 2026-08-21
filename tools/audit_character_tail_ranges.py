#!/usr/bin/env python3
"""Audit concrete character-tail rollback coverage against Ghidra and docs."""

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


IMAGE_BASE = 0x140000000
TAIL_START = 0x68F0
RANGE_RE = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)(?:ull)?\s*,\s*(0x[0-9a-fA-F]+)\s*,"
    r"\s*(0x[0-9a-fA-F]+)\s*\}"
)
GENERATED_RE = re.compile(
    r"\{\s*(0x[0-9a-fA-F]+)(?:ull)?\s*,\s*(0x[0-9a-fA-F]+)\s*,"
    r"\s*(0x[0-9a-fA-F]+)\s*\}\s*,?\s*//\s*([^:]+):\s*(.*)"
)
DOC_RANGE_RE = re.compile(r"\+?(0x[0-9a-fA-F]+)/(0x[0-9a-fA-F]+|\d+)")
DOC_DTI_RE = re.compile(r"`(0x[0-9a-fA-F]+)`")


@dataclass(frozen=True)
class TailRange:
    dti: int
    offset: int
    size: int
    source: str
    description: str = ""

    @property
    def end(self):
        return self.offset + self.size


SPECIAL_RESTORE_RANGES = [
    TailRange(0x140D45A10, 0x68F0, 0x28, "concrete_snapshot.cpp:Ryu recipe"),
    TailRange(0x140D45A10, 0x6920, 0x20, "concrete_snapshot.cpp:Ryu recipe"),
    TailRange(0x140D455F8, 0x6900, 0x20, "concrete_snapshot.cpp:Hsien-Ko recipe"),
    TailRange(0x140D45D40, 0x68F0, 0x10, "concrete_snapshot.cpp:Spencer recipe"),
]
KNOWN_UNRESOLVED_RANGES = [
    # Constructor-zeroed state whose gameplay semantics remain unproven.
    TailRange(0x140D450A8, 0x68F0, 0x5C, "chrfields.md:Doctor Strange unresolved"),
    TailRange(0x140D45958, 0x68F8, 0x04, "chrfields.md:Firebrand unresolved"),
    TailRange(0x140D459D8, 0x6950, 0x02, "chrfields.md:Rocket Raccoon unresolved"),
]

# These Ghidra structures have useful names and sizes but zero DTI RVAs in the
# export. Their concrete tails were recovered manually from constructors and
# gameplay consumers instead of treating the placeholder records as schemas.
MANUAL_GHIDRA_LAYOUTS = {
    0x140D44E90: {
        "name": "uCapAmerica", "size": 0x6900,
        "fields": [
            {"offset": 0x68F0, "size": 8, "name": "mpShieldSlash", "type": "sUnit *"},
        ],
    },
    0x140D452F0: {
        "name": "uGouki", "size": 0x6900,
        "fields": [
            {"offset": 0x68F0, "size": 1, "name": "mTagFlagA", "type": "bool"},
            {"offset": 0x68F1, "size": 1, "name": "mTagFlagB", "type": "bool"},
        ],
    },
    0x140D45450: {
        "name": "uHiryu", "size": 0x6930,
        "fields": [
            {"offset": 0x68F0, "size": 4, "name": "mWallStickTime", "type": "float"},
            {"offset": 0x68F8, "size": 8, "name": "mpOptionUnit", "type": "sUnit *"},
            {"offset": 0x6900, "size": 1, "name": "mTeamStateLatch", "type": "bool"},
            {"offset": 0x6910, "size": 0x10, "name": "mFrozenTargets", "type": "MtArray"},
            {"offset": 0x6920, "size": 4, "name": "mOuroborosState", "type": "uint32_t"},
            {"offset": 0x6924, "size": 1, "name": "mOuroborosLatch", "type": "bool"},
            {"offset": 0x6928, "size": 4, "name": "mVoiceTimer", "type": "float"},
        ],
    },
    0x140D45958: {
        "name": "uRedArremer", "size": 0x6930,
        "fields": [
            {"offset": 0x68F0, "size": 4, "name": "mWallStickTime", "type": "float"},
            {"offset": 0x68F4, "size": 1, "name": "mAbilityActive", "type": "bool"},
            {"offset": 0x68F8, "size": 4, "name": "mActionState", "type": "uint32_t"},
            {"offset": 0x68FC, "size": 1, "name": "mSyncLatch", "type": "bool"},
            {"offset": 0x68FD, "size": 1, "name": "mChaosTideLatch", "type": "bool"},
            {"offset": 0x6900, "size": 8, "name": "mpChaosTideEffect", "type": "sUnit *"},
            {"offset": 0x6908, "size": 0x20, "name": "mReferenceSlots", "type": "sUnit *[4]"},
            {"offset": 0x6928, "size": 4, "name": "mFlightIdleTimer", "type": "float"},
        ],
    },
    0x140D459D8: {
        "name": "uRRaccoon", "size": 0x6970,
        "fields": [
            {"offset": 0x68F0, "size": 0x60, "name": "mTrapUnits", "type": "sUnit *[12]"},
            {"offset": 0x6950, "size": 2, "name": "mActionFlags", "type": "uint16_t"},
            {"offset": 0x6954, "size": 4, "name": "mWeaponAction", "type": "int32_t"},
            {"offset": 0x6958, "size": 4, "name": "mMovement", "type": "float"},
            {"offset": 0x695C, "size": 1, "name": "mMovementLatch", "type": "bool"},
            {"offset": 0x6960, "size": 4, "name": "mVoiceTimer", "type": "float"},
        ],
    },
}


def parse_generated(path):
    ranges = []
    names = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = GENERATED_RE.search(line)
        if not match:
            continue
        dti, offset, size = (int(match.group(index), 0) for index in range(1, 4))
        name = match.group(4).strip()
        names.setdefault(dti, name)
        ranges.append(TailRange(
            dti, offset, size, f"{path.name}:{line_number}", match.group(5).strip()
        ))
    return ranges, names


def parse_cpp_ranges(path, array_name):
    text = path.read_text(encoding="utf-8")
    marker = f"constexpr DtiSnapshotRange {array_name}[] = {{"
    start = text.find(marker)
    if start < 0:
        raise ValueError(f"missing {array_name} in {path}")
    end = text.find("\n};", start)
    if end < 0:
        raise ValueError(f"unterminated {array_name} in {path}")
    ranges = []
    line_base = text[:start].count("\n") + 1
    for relative_line, line in enumerate(text[start:end].splitlines(), 0):
        match = RANGE_RE.search(line)
        if not match:
            continue
        dti, offset, size = (int(match.group(index), 0) for index in range(1, 4))
        description = line.split("//", 1)[1].strip() if "//" in line else ""
        ranges.append(TailRange(
            dti, offset, size, f"{path.name}:{line_base + relative_line}", description
        ))
    return ranges


def parse_doc_table(path, heading, next_heading, source_name):
    text = path.read_text(encoding="utf-8")
    start = text.find(heading)
    end = text.find(next_heading, start + len(heading))
    if start < 0 or end < 0:
        raise ValueError(f"missing documentation section {heading!r}")
    ranges = []
    for line_number, line in enumerate(text[:end].splitlines(), 1):
        if line_number <= text[:start].count("\n"):
            continue
        dti_match = DOC_DTI_RE.search(line)
        if not dti_match:
            continue
        dti = int(dti_match.group(1), 0)
        for range_match in DOC_RANGE_RE.finditer(line):
            ranges.append(TailRange(
                dti, int(range_match.group(1), 0), int(range_match.group(2), 0),
                f"{source_name}:{line_number}", line.strip(),
            ))
    return ranges


def load_ghidra_layouts(path):
    document = json.loads(path.read_text(encoding="utf-8"))
    layouts = {}
    for record in document.get("classes", []):
        rva = int(record.get("dti_rva", "0"), 0)
        if not rva:
            continue
        layouts[IMAGE_BASE + rva] = {
            "name": record["name"],
            "size": int(record["size"]),
            "fields": record.get("fields", []),
        }
    layouts.update(MANUAL_GHIDRA_LAYOUTS)
    return layouts


def covers(ranges, offset, size):
    if size <= 0:
        return True
    pending = offset
    end = offset + size
    for item in sorted(ranges, key=lambda value: (value.offset, value.end)):
        if item.end <= pending or item.offset > pending:
            continue
        pending = max(pending, item.end)
        if pending >= end:
            return True
    return False


def covered_bytes(ranges, object_size):
    intervals = []
    for item in ranges:
        start = max(TAIL_START, item.offset)
        end = min(object_size, item.end)
        if start < end:
            intervals.append((start, end))
    total = 0
    current_start = current_end = None
    for start, end in sorted(intervals):
        if current_start is None:
            current_start, current_end = start, end
        elif start <= current_end:
            current_end = max(current_end, end)
        else:
            total += current_end - current_start
            current_start, current_end = start, end
    if current_start is not None:
        total += current_end - current_start
    return total


def build_audit(root):
    generated_path = root / "UMVC3Hook/code/generated_character_tail_fields.inc"
    concrete_path = root / "UMVC3Hook/code/concrete_snapshot.cpp"
    docs_path = root / "doc/chrfields.md"
    structs_path = root / "ghidra_export/snapdump_structs.json"

    generated, generated_names = parse_generated(generated_path)
    verified = parse_cpp_ranges(concrete_path, "kVerifiedCharacterScalarRanges")
    documented = parse_doc_table(
        docs_path, "## Verified Scalar/POD Fields", "## Unsafe References And Owners",
        docs_path.name,
    )
    unsafe = parse_doc_table(
        docs_path, "## Unsafe References And Owners", "## Generated Range Conflicts",
        docs_path.name,
    )
    layouts = load_ghidra_layouts(structs_path)

    all_dtis = sorted({item.dti for item in generated} | {item.dti for item in verified})
    records = []
    errors = []
    warnings = []
    schema_warnings = []
    for dti in all_dtis:
        layout = layouts.get(dti)
        name = generated_names.get(dti, layout["name"] if layout else "unknown")
        generated_ranges = [item for item in generated if item.dti == dti]
        verified_ranges = [item for item in verified if item.dti == dti]
        special_ranges = [item for item in SPECIAL_RESTORE_RANGES if item.dti == dti]
        restore_ranges = generated_ranges + verified_ranges + special_ranges
        unsafe_ranges = [item for item in unsafe if item.dti == dti]
        if layout is None:
            schema_warnings.append(f"{name}: DTI 0x{dti:x} is absent from Ghidra export")
            records.append({
                "name": name, "dti": dti, "schema_warning": "missing Ghidra layout",
                "generated_ranges": len(generated_ranges),
                "verified_ranges": len(verified_ranges),
                "special_ranges": len(special_ranges),
            })
            continue

        for item in restore_ranges:
            if item.offset < TAIL_START:
                errors.append(f"{name}: {item.source} starts before concrete tail")
            if item.end > layout["size"]:
                schema_warnings.append(
                    f"{name}: {item.source} ends at 0x{item.end:x}, "
                    f"past exported structure size 0x{layout['size']:x}"
                )

        uncovered_fields = []
        unsafe_fields = []
        unresolved_fields = []
        unresolved_ranges = [item for item in KNOWN_UNRESOLVED_RANGES if item.dti == dti]
        for field in layout["fields"]:
            offset = int(field["offset"])
            size = int(field["size"])
            if offset < TAIL_START or size <= 0:
                continue
            label = f"{field['name']}@0x{offset:x}/0x{size:x}"
            if covers(unsafe_ranges, offset, size):
                unsafe_fields.append(label)
            elif covers(restore_ranges, offset, size):
                continue
            elif covers(unresolved_ranges, offset, size):
                unresolved_fields.append(label)
            else:
                uncovered_fields.append(label)
        if uncovered_fields:
            warnings.append(f"{name}: uncovered exported fields: {', '.join(uncovered_fields)}")

        records.append({
            "name": name,
            "ghidra_name": layout["name"],
            "dti": dti,
            "size": layout["size"],
            "generated_ranges": len(generated_ranges),
            "verified_ranges": len(verified_ranges),
            "special_ranges": len(special_ranges),
            "covered_bytes": covered_bytes(restore_ranges, layout["size"]),
            "unsafe_fields": unsafe_fields,
            "unresolved_fields": unresolved_fields,
            "uncovered_fields": uncovered_fields,
        })

    restore_by_dti = {
        dti: [item for item in generated + verified + SPECIAL_RESTORE_RANGES if item.dti == dti]
        for dti in all_dtis
    }
    for item in documented:
        if not covers(restore_by_dti.get(item.dti, []), item.offset, item.size):
            errors.append(
                f"documented scalar range 0x{item.dti:x}+0x{item.offset:x}/0x{item.size:x} "
                f"from {item.source} is not implemented"
            )

    arthur_dti = 0x140D44E48
    if not covers(restore_by_dti.get(arthur_dti, []), 0x68F9, 1):
        errors.append("uArthur::mIsCrossSword at +0x68f9 is not restored")

    return {
        "schema": "umvc3_character_tail_audit_v1",
        "character_count": len(records),
        "generated_range_count": len(generated),
        "verified_range_count": len(verified),
        "documented_scalar_range_count": len(documented),
        "records": records,
        "errors": errors,
        "warnings": warnings,
        "schema_warnings": schema_warnings,
    }


def print_report(report):
    print(
        f"Character tail audit: {report['character_count']} DTIs, "
        f"{report['generated_range_count']} generated ranges, "
        f"{report['verified_range_count']} verified ranges"
    )
    print("Character          DTI                 Size    Gen Ver Spc Bytes Uncovered")
    for record in report["records"]:
        if "schema_warning" in record:
            print(
                f"{record['name']:<18} 0x{record['dti']:016x} {'?':>7} "
                f"{record['generated_ranges']:>3} {record['verified_ranges']:>3} "
                f"{record['special_ranges']:>3}     -         -"
            )
            continue
        print(
            f"{record['name']:<18} 0x{record['dti']:016x} 0x{record['size']:05x} "
            f"{record['generated_ranges']:>3} {record['verified_ranges']:>3} "
            f"{record['special_ranges']:>3} {record['covered_bytes']:>5} "
            f"{len(record['uncovered_fields']):>9}"
        )
    for warning in report["warnings"]:
        print(f"warning: {warning}", file=sys.stderr)
    for error in report["errors"]:
        print(f"error: {error}", file=sys.stderr)
    for warning in report["schema_warnings"]:
        print(f"schema warning: {warning}", file=sys.stderr)
    arthur = next((item for item in report["records"] if item.get("dti") == 0x140D44E48), None)
    if arthur and "error" not in arthur:
        print("Arthur mIsCrossSword (+0x68f9): covered")


def main():
    root_default = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=root_default)
    parser.add_argument("--json", action="store_true", help="print JSON instead of the table")
    parser.add_argument("--strict", action="store_true", help="treat uncovered exported fields as errors")
    args = parser.parse_args()
    try:
        report = build_audit(args.root.resolve())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)
    return 1 if report["errors"] or (args.strict and report["warnings"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
