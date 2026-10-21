#!/usr/bin/env python3
"""Portable, isolated current-kernel experiments. Never builds/deploys the plugin."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INPUTS = [
    "UMVC3Hook/code/rollback_restore.cpp",
    "UMVC3Hook/code/rollback_snapshot.cpp",
    "UMVC3Hook/code/rollback_character_restore.cpp",
    "UMVC3Hook/code/rollback_character_restore.h",
    "UMVC3Hook/code/concrete_snapshot.cpp",
    "UMVC3Hook/code/concrete_snapshot.h",
    "UMVC3Hook/code/rollback_model_policy.cpp",
    "doc/compact_snapshot.md", "doc/scollision_rollback_snapshot.md",
    "tools/benchmark_subsystem_restore.cpp",
    "tools/benchmark_subsystem_restore.py",
    "tools/benchmark_subsystem_restore.md",
]


def clean(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def between(text, start, end):
    if text.count(start) != 1:
        raise ValueError(f"nonunique/missing anchor: {start}")
    rest = text.split(start)[1]
    if end not in rest:
        raise ValueError(f"missing end anchor: {end}")
    return rest.split(end)[0]


def block(text, anchor):
    if text.count(anchor) != 1:
        raise ValueError(f"nonunique/missing block: {anchor}")
    start = text.index(anchor)
    opening = text.index("{", start)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


def table(text, name):
    value = block(text, f"static constexpr SnapshotRange {name}[]") + ";"
    ranges = [tuple(int(x, 0) for x in pair) for pair in
              re.findall(r"\{\s*(0x[\da-fA-F]+|\d+)\s*,\s*(0x[\da-fA-F]+|\d+)\s*\}", value)]
    if not ranges:
        raise ValueError(f"empty table {name}")
    return value, ranges


def coalesce(ranges):
    spans = []
    for offset in sorted({b for o, n in ranges for b in range(o, o + n)}):
        if spans and sum(spans[-1]) == offset:
            spans[-1][1] += 1
        else:
            spans.append([offset, 1])
    return spans


def generate(texts):
    source = clean(texts[INPUTS[0]])
    character = clean(texts[INPUTS[2]])
    weapon = between(source, "            uint8_t* weapon =", "        } else if (slot.regions[i].skipVtable &&")
    weapon = "uint8_t* weapon =" + weapon
    wt, wr = table(weapon, "weaponScalarRanges")
    motion = block(character, "void Rollback_RestoreModelMotions(")
    if "for (size_t base = 0x560; base < 0x1160; base += 0x180)" not in motion:
        raise ValueError("motion layout changed")
    mr = [tuple(int(v, 0) for v in pair) for pair in
          re.findall(r"Copy<(0x[\da-fA-F]+), (0x[\da-fA-F]+)>\(live \+ base, saved \+ base\);", motion)]
    if mr != [(8, 0xD8), (0xE0, 0x40), (0x120, 0x40), (0x168, 0x10)]:
        raise ValueError("motion policy changed: re-audit required")
    at, ar = table(source, "SACTION_RESTORE_RANGES")
    tt, tr = table(source, "CCHR_TEAM_RESTORE_RANGES")
    wrapper = block(source, "void Rollback_RestoreExternalAnimBlock(")
    xr = [(8, 1), (0x80, 0x10), (0x90, 0x28), (0xC8, 0x18)]
    # Literal expected schemas make extraction fail closed on policy changes.
    if wr != [(0x48, 0xB8), (0x510, 0x20), (0x558, 8), (0x1160, 0x18),
              (0x1180, 0x30), (0x1458, 0x48), (0x14A8, 0x48),
              (0x14F8, 0x48), (0x1548, 0x98)]:
        raise ValueError("weapon policy changed")
    if ar != [(0x28, 0xE8), (0x140, 8), (0x150, 8), (0x170, 1), (0x218, 4)]:
        raise ValueError("action policy changed")
    if tr != [(0x50, 0xC0), (0xC0, 0x50), (0x110, 0x1B0)]:
        raise ValueError("team policy changed")
    scaffolding = """
    struct Region { void* ptr; };
    struct Slot { Region regions[1]; const uint8_t* data; };
    Slot slot{{{dst}}, src};
    constexpr int i = 0;
    constexpr size_t offset = 0;
"""
    output = ["// Generated from current sources; no native stubs.",
              "struct SnapshotRange { uint32_t offset, size; };", at, tt,
              "constexpr size_t CCHR_EXTERNAL_ANIM_SIZE = 0xE0;", wrapper,
              "NOINLINE void current_weapon(uint8_t* dst, const uint8_t* src, size_t copySize) {",
              "if (copySize < 0x1300) return;", scaffolding, weapon, "}"]
    for name, tab in [("team", "CCHR_TEAM_RESTORE_RANGES"), ("action", "SACTION_RESTORE_RANGES")]:
        loop = block(source, f"for (const SnapshotRange& range : {tab})")
        output += [f"NOINLINE void current_{name}(uint8_t* dst, const uint8_t* src, size_t copySize) {{",
                   scaffolding, loop, "}"]
    output += ["NOINLINE void current_wrapper(uint8_t* dst, const uint8_t* src, size_t n) {",
               "Rollback_RestoreExternalAnimBlock(dst, src, n); }"]
    plans = []
    for name, size, count, ranges, extra in [
        ("weapon", 0x15E0, 12, wr + [(b + o, n) for b in range(0x560, 0x1160, 0x180) for o, n in mr], []),
        ("team", 0x2C0, 2, tr, [(0x48, 8)]),
        ("action", 0x300, 1, ar, []),
        ("wrapper", 0xE0, 24, xr, []),
    ]:
        spans = coalesce(ranges)
        capture = spans + extra
        packed = sum(n for _, n in capture)
        plans.append(dict(name=name, full=size, packed=packed, count=count,
                          original_copy_bytes=sum(n for _, n in ranges),
                          unique_restore_bytes=sum(n for _, n in spans),
                          spans=spans, retained_reference_ranges=extra,
                          forced_zero=0x74 if name == "wrapper" else -1))
        for kind in ("fixed", "pack", "unpack"):
            output += [f"NOINLINE void {kind}_{name}(uint8_t* dst, const uint8_t* src, size_t n) {{"]
            if kind == "fixed":
                output += [f"if (n != {size}) {{ current_{name}(dst, src, n); return; }}"]
            else:
                output += [f"if (n != {size if kind == 'pack' else packed}) return;"]
            cursor = 0
            for o, n in capture if kind == "pack" else spans:
                d, s = (cursor, o) if kind == "pack" else (o, cursor if kind == "unpack" else o)
                output += [f"std::memcpy(dst + {d}, src + {s}, {n});"]
                cursor += n
            if name == "wrapper" and kind != "pack":
                output += ["dst[0x74] = 0;"]
            output += ["}"]
    output += ["static const Case cases[] = {"]
    for p in plans:
        n = p["name"]
        spans = ",".join(f"{{{o},{s}}}" for o, s in p["spans"])
        output += [f'{{"{n}",{p["full"]},{p["packed"]},{p["count"]},'
                   f'{p["original_copy_bytes"]},{p["unique_restore_bytes"]},{p["forced_zero"]},'
                   f'current_{n},fixed_{n},pack_{n},unpack_{n},{{{spans}}}}},']
    output += ["};"]
    return "\n".join(output), plans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trials", type=int, default=9)
    parser.add_argument("--iterations", type=int, default=200000)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.trials < 7 or args.iterations < 1:
        parser.error("at least seven trials and positive iterations required")
    parent = Path("/tmp/opencode")
    if not parent.is_dir():
        parser.error("/tmp/opencode must exist")
    output = Path(tempfile.mkdtemp(prefix="subsystem-restore-", dir=parent))
    texts = {p: (ROOT / p).read_text() for p in INPUTS}
    generated, plans = generate(texts)
    header = output / "subsystem.generated.h"
    header.write_text(generated)
    (output / "plans.json").write_text(json.dumps(plans, indent=2) + "\n")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-fno-strict-aliasing"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
              if args.sanitize else ["-O3", "-DNDEBUG"])
    # Separate translation unit, no LTO: the actual production motion call stays linked.
    command = ["g++", *flags, "-I", str(output), "-I", str(ROOT / "UMVC3Hook/code"),
               str(ROOT / "tools/benchmark_subsystem_restore.cpp"),
               str(ROOT / INPUTS[2]), "-o", str(output / "bench")]
    cpu = args.cpu if args.cpu is not None else min(os.sched_getaffinity(0))
    run = ["taskset", "-c", str(cpu), str(output / "bench"), str(args.trials), str(args.iterations)]
    metadata = dict(started_utc=datetime.now(timezone.utc).isoformat(),
                    inputs={p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in INPUTS},
                    generated_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                    compile_command=command, run_command=run, platform=platform.platform(),
                    hardware=subprocess.check_output(["lscpu"], text=True),
                    compiler=subprocess.check_output(["g++", "--version"], text=True),
                    git_head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                    affinity=sorted(os.sched_getaffinity(0)), sanitizer=args.sanitize)
    subprocess.run(command, check=True)
    result = subprocess.run(run, text=True, capture_output=True, check=True)
    for p, digest in metadata["inputs"].items():
        if hashlib.sha256((ROOT / p).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"input changed during measurement: {p}")
    (output / "results.csv").write_text(result.stdout)
    (output / "checks.txt").write_text(result.stderr)
    metadata["outputs"] = {p: hashlib.sha256((output / p).read_bytes()).hexdigest()
                           for p in ("bench", "results.csv", "checks.txt", "plans.json")}
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(result.stderr, end="")
    print(f"Artifacts: {output}")
    rows = list(csv.DictReader(result.stdout.splitlines()))
    print("case,workload,phase,before_ms_count,fixed_ms_count,packed_ms_count")
    for p in plans:
        for workload in ("hot", "ring16"):
            for phase in ("capture", "restore", "combined"):
                values = [r["median_ms_count"] for r in rows if r["case"] == p["name"]
                          and r["workload"] == workload and r["phase"] == phase]
                print(",".join([p["name"], workload, phase, *values]))


if __name__ == "__main__":
    main()
