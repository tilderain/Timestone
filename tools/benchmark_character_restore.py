#!/usr/bin/env python3
"""Benchmark a frozen legacy common-character restore oracle, not current production.

This is a portable microbenchmark, not a game or plugin build. Generated source,
executables, provenance and CSV results live outside the repository by default.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    if text.count(start) != 1:
        raise ValueError(f"extraction anchor not unique: {start!r}")
    suffix = text.split(start, 1)[1]
    if end not in suffix:
        raise ValueError(f"missing end anchor: {end!r}")
    return suffix.split(end, 1)[0]


def declaration(text, name):
    text = re.sub(r"//[^\n]*", "", text)
    match = re.search(
        rf"static constexpr [^;\n]*\b{re.escape(name)}\b[^;]*;", text
    )
    if not match:
        raise ValueError(f"missing declaration: {name}")
    return match.group(0)


def generate(source, memory_source):
    """Historical extractor for the legacy source format; not used by the runner."""
    top = source[:source.index("static void RestoreCharacterCollisionParentReferences")]
    names = re.findall(
        r"static constexpr \w+ ((?:CCHR_|CUNIT_|UCHRCPU_|DTI_UIRONMAN)\w*)", top
    )
    constants = "\n".join(declaration(top, name) for name in names)
    tables = "\n".join(declaration(source, name) for name in (
        "CCHR_SCALAR_RESTORE_RANGES", "CCHR_POST_RESTORE_TAIL_RANGES"
    ))
    pointer_predicate = "bool IsLikelyPointer(uint64_t value)" + between(
        memory_source, "bool IsLikelyPointer(uint64_t value)", "\nbool IsAccessiblePointer"
    )
    helpers = "    auto isTeamCharacter" + between(
        source, "    auto isTeamCharacter", "    auto restoreCharacterPostState"
    )
    body = between(
        source,
        "        } else if (slot.regions[i].skipVtable && copySize > sizeof(uint64_t)) {",
        "        } else if (copySize > 0) {"
    )
    late_start = source.index("    // Recipe reconstruction and model state repair")
    late = "            for (const SnapshotRange& range : CCHR_POST_RESTORE_TAIL_RANGES)" + between(
        source[late_start:],
        "            for (const SnapshotRange& range : CCHR_POST_RESTORE_TAIL_RANGES)",
        "            Rollback_RemapCharacterShotLinks("
    )
    # These hooks need the running game's object/resource graph. Exclude them
    # explicitly on BOTH sides rather than inventing a timing estimate for them.
    return f"""// Generated from the working-tree rollback_restore.cpp. Do not edit.
struct SnapshotRange {{ uint32_t offset, size; }};
{constants}
{tables}
static uint64_t _addr(uint64_t value) {{ return value; }}
{pointer_predicate}
static uint64_t GetObjectDti(uint64_t) {{ return 0; }}
static bool IsEffectObject(void*) {{ return false; }}
static void HookLog(const char*, ...) {{}}
static void ConcreteCharacterSnapshot_RestoreFields(void*, const uint8_t*, size_t, uint64_t) {{}}

__attribute__((noinline)) static void currentEarly(
    uint8_t* destination, const uint8_t* saved, size_t copySize,
    const uint64_t* teamCharacters) {{
    constexpr size_t teamCharacterCount = 6;
    struct Region {{ void* ptr; }};
    struct Slot {{ Region regions[1]; const uint8_t* data; }};
    Slot slot{{{{{{destination}}}}, saved}};
    constexpr int i = 0;
    constexpr size_t offset = 0;
{helpers}
    {{
{body}
    }}
}}
__attribute__((noinline)) static void currentLate(
    uint8_t* destination, const uint8_t* saved, size_t copySize,
    const uint64_t* teamCharacters) {{
    constexpr size_t teamCharacterCount = 6;
    struct Region {{ void* ptr; }};
    struct Slot {{ Region regions[1]; const uint8_t* data; }};
    Slot slot{{{{{{destination}}}}, saved}};
    constexpr int i = 0;
    constexpr size_t offset = 0;
{helpers}
    {{
{late}
    }}
}}
__attribute__((noinline)) static void currentRestore(
    uint8_t* destination, const uint8_t* saved, size_t copySize,
    const uint64_t* teamCharacters) {{
    currentEarly(destination, saved, copySize, teamCharacters);
    asm volatile("" ::: "memory");
    currentLate(destination, saved, copySize, teamCharacters);
}}
"""


def generate_fixed(plans):
    """Emit literal fixture offsets AND sizes; no runtime size-switch substitute."""
    lines = ["// Generated from plans.json; fixture-only policies. Do not edit."]
    for layout, plan in enumerate(plans):
        for phase in ("early", "late"):
            lines.append(f"__attribute__((noinline)) static void fixed_{layout}_{phase}("
                         "uint8_t* dst, const uint8_t* src) {")
            for offset, size in plan[phase]:
                lines.append(f"    std::memcpy(dst + {offset}, src + {offset}, {size});")
            lines.append("}")
    lines.append("static const FixedLayout fixedLayouts[] = {")
    for layout, plan in enumerate(plans):
        ranges = ["{" + ",".join(f"{{{offset},{size}}}" for offset, size in plan[phase]) + "}"
                  for phase in ("early", "late")]
        lines.append(f'    {{{plan["bytes"]}, "{plan["shape"]}", fixed_{layout}_early, '
                     f'fixed_{layout}_late, {ranges[0]}, {ranges[1]}}},')
    lines += ["};", "__attribute__((noinline)) static const FixedLayout* resolveFixedLayout(size_t id) {",
              '    asm volatile("" ::: "memory");',
              '    if (id >= std::size(fixedLayouts)) throw std::runtime_error("invalid layout id");',
              "    return &fixedLayouts[id];", "}"]
    return "\n".join(lines) + "\n"


def audit_range_tables(header):
    """Audit unconditional table copies only, not embedded loops or native calls."""
    masks = []
    report = {"scope": "Two scalar tables only; excludes embedded records, broad copies, and conditional fixups."}
    for name in ("CCHR_SCALAR_RESTORE_RANGES", "CCHR_POST_RESTORE_TAIL_RANGES"):
        table = declaration(header, name)
        ranges = [(int(offset, 0), int(size, 0)) for offset, size in re.findall(
            r"\{\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\}", table)]
        if not ranges:
            raise ValueError(f"empty range table: {name}")
        mask = set()
        for offset, size in ranges:
            mask.update(range(offset, offset + size))
        ordered = sorted(mask)
        spans = []
        for offset in ordered:
            if spans and offset == spans[-1][0] + spans[-1][1]:
                spans[-1][1] += 1
            else:
                spans.append([offset, 1])
        written = sum(size for _, size in ranges)
        report[name] = {"ranges": len(ranges), "written_bytes": written,
            "unique_bytes": len(mask), "same_phase_duplicate_bytes": written - len(mask),
            "full_buffer_coalesced_ranges": len(spans), "coalesced_spans": spans}
        masks.append(mask)
    report["cross_phase_overlap_bytes"] = len(masks[0] & masks[1])
    report["cross_phase_removal_safe"] = False
    return report


def summarize_experiments(output):
    with (output / "results.csv").open() as results:
        rows = list(csv.DictReader(results))
    cases = {}
    for row in rows:
        key = (row["bytes"], row["shape"], row["working_set"])
        cases.setdefault(key, {})[row["kernel"]] = float(row["median_ns"])
    pairs = {"fixed_vs_table": ("table_inline64", "fixed_cached"),
             "coalesced_vs_fragmented": ("fragmented_inline64", "coalesced_inline64"),
             "coalesced_vs_original": ("table_inline64", "coalesced_inline64"),
             "layout_vs_range_bounds": ("bounds_range", "bounds_layout"),
             "cached_vs_lookup": ("fixed_lookup", "fixed_cached")}
    summary = {}
    for group in ["all", *sorted({key[2] for key in cases})]:
        summary[group] = {}
        for name, (baseline, candidate) in pairs.items():
            ratios = [values[baseline] / values[candidate] for key, values in cases.items()
                      if group == "all" or key[2] == group]
            summary[group][name] = {"cases": len(ratios), "wins": sum(r > 1 for r in ratios),
                "geomean_speedup": math.exp(sum(map(math.log, ratios)) / len(ratios)),
                "min_speedup": min(ratios), "max_speedup": max(ratios)}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary["all"], indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("/tmp/opencode/umvc3_restore_bench"))
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--ms", type=int, default=30)
    parser.add_argument("--stream-mib", type=int, default=64)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--no-pie", action="store_true", help="non-PIE host executable for sanitizer startup troubleshooting")
    parser.add_argument("--portable", action="store_true", help="omit -march=native")
    parser.add_argument("--experiments", action="store_true",
                        help="opt-in generated offsets, coalescing, bounds and layout-dispatch matrix")
    parser.add_argument("--shapes", default="all", help="experiment comma-separated fixture filter")
    parser.add_argument("--workloads", default="all", help="experiment comma-separated workload filter")
    parser.add_argument("--sizes", default="all", help="experiment comma-separated byte-size filter")
    parser.add_argument("--frozen-header", type=Path,
                        help="required: previously extracted legacy current_character_restore.generated.h")
    args = parser.parse_args()
    if args.frozen_header is None:
        parser.error(
            "--frozen-header PATH is required for legacy oracle comparisons. "
            "Production now uses rollback_character_restore.cpp with explicit policies, "
            "not the old scalar tables/restore branch, and requires a separate harness. "
            "Use a previously extracted current_character_restore.generated.h "
            "(for example /tmp/opencode/restore_baseline/current_character_restore.generated.h). "
            "These results do not measure or establish equivalence with new production.")
    if min(args.samples, args.ms, args.stream_mib) < 1:
        parser.error("samples, ms and stream-mib must be positive")
    filters = ((args.shapes, {"sparse32", "pointers200", "false_positive256", "cap_stress600"}),
               (args.workloads, {"hot", "match_ring", "streaming", "mixed_ring", "mixed_streaming"}),
               (args.sizes, {"27232", "30000"}))
    for value, allowed in filters:
        if value != "all" and not set(value.split(",")) <= allowed:
            parser.error(f"invalid experiment filter: {value}")
    if not args.experiments and any(value != "all" for value, _ in filters):
        parser.error("filters require --experiments")
    frozen_header = args.frozen_header.resolve()
    try:
        header_bytes = frozen_header.read_bytes()
        header = header_bytes.decode("utf-8")
        audit = audit_range_tables(header)
        for name in ("currentEarly", "currentLate", "currentRestore"):
            if not re.search(rf"\b{name}\s*\(", header):
                raise ValueError(f"missing legacy entry point: {name}")
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(f"invalid --frozen-header {frozen_header}: {error}. "
                     "Use a previously extracted legacy header, not production .cpp/.h source.")
    print("Legacy frozen-oracle benchmark only; not the explicit production path.", flush=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "current_character_restore.generated.h"
    generated.write_bytes(header_bytes)
    (output / "scalar_range_audit.json").write_text(
        json.dumps(audit, indent=2) + "\n")
    harness = ROOT / "tools/benchmark_character_restore.cpp"
    executable = output / "benchmark_character_restore"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-fno-strict-aliasing"]
    if args.sanitize:
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    else:
        flags += ["-O3", "-DNDEBUG"]
        if not args.portable:
            flags += ["-march=native"]
    if args.no_pie:
        flags += ["-fno-pie", "-no-pie"]
    compile_command = [args.compiler, *flags, "-I", str(output), str(harness), "-o", str(executable)]
    subprocess.run(compile_command, check=True)
    dump_command = None
    bootstrap_compile_command = None
    if args.experiments:
        bootstrap_compile_command = compile_command
        dump_command = [str(executable), "--dump-plans"]
        plans_text = subprocess.check_output(dump_command, text=True)
        (output / "plans.json").write_text(plans_text)
        (output / "fixed_character_restore.generated.h").write_text(generate_fixed(json.loads(plans_text)))
        compile_command = [args.compiler, *flags, "-DFIXED_LAYOUT_EXPERIMENTS", "-I", str(output),
                           str(harness), "-o", str(executable)]
        subprocess.run(compile_command, check=True)
    run_command = [str(executable), str(args.samples), str(args.ms), str(args.stream_mib)]
    if args.experiments:
        run_command = [str(executable), "--experiments", str(args.samples), str(args.ms),
                       str(args.stream_mib), args.shapes, args.workloads, args.sizes]
    if args.cpu is not None:
        run_command = ["taskset", "-c", str(args.cpu), *run_command]
    provenance = {
        "mode": "legacy_frozen_oracle",
        "restore_source": None,
        "restore_sha256": None,
        "memory_sha256": None,
        "frozen_header": str(frozen_header),
        "frozen_header_sha256": hashlib.sha256(header_bytes).hexdigest(),
        "harness_sha256": hashlib.sha256(harness.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "compile_command": compile_command,
        "bootstrap_compile_command": bootstrap_compile_command,
        "dump_command": dump_command,
        "run_command": run_command,
        "compiler": subprocess.check_output([args.compiler, "--version"], text=True),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "scope": "Frozen legacy common-character body + late scalar repairs; fixed-policy synthetic fixtures. Not current explicit production or evidence of equivalence with it.",
        "exclusions": ["concrete-tail detection", "native reconstruction", "owned buffers",
                       "collision-parent repair", "shot/effect remapping", "worker barriers"],
    }
    if args.experiments:
        for filename in ("plans.json", "fixed_character_restore.generated.h"):
            provenance[filename + "_sha256"] = hashlib.sha256((output / filename).read_bytes()).hexdigest()
        # Keep per-function text sizes and whole-binary section sizes, not source-line estimates.
        symbols = subprocess.check_output(["nm", "-S", "--size-sort", "-C", str(executable)], text=True)
        (output / "symbols.txt").write_text(symbols)
        (output / "sections.txt").write_text(subprocess.check_output(["size", "-A", str(executable)], text=True))
        fixed_sizes = {}
        for line in symbols.splitlines():
            match = re.match(r"[0-9a-f]+\s+([0-9a-f]+)\s+\w\s+(fixed_\d+_(?:early|late))\(", line)
            if match:
                fixed_sizes[match[2]] = int(match[1], 16)
        (output / "code_sizes.json").write_text(json.dumps({"fixed_functions": fixed_sizes,
            "fixed_total_bytes": sum(fixed_sizes.values())}, indent=2) + "\n")
        provenance["experiment_filters"] = {"shapes": args.shapes, "workloads": args.workloads, "sizes": args.sizes}
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    with (output / "results.csv").open("w") as results:
        subprocess.run(run_command, stdout=results, check=True)
    if args.experiments:
        summarize_experiments(output)
    print(f"Results: {output / 'results.csv'}")


if __name__ == "__main__":
    main()
