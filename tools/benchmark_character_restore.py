#!/usr/bin/env python3
"""Extract the working-tree common-character restore kernel and benchmark it.

This is a portable microbenchmark, not a game or plugin build. Generated source,
executables, provenance and CSV results live outside the repository by default.
"""
import argparse
import hashlib
import json
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("/tmp/opencode/umvc3_restore_bench"))
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--ms", type=int, default=30)
    parser.add_argument("--stream-mib", type=int, default=64)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if min(args.samples, args.ms, args.stream_mib) < 1:
        parser.error("samples, ms and stream-mib must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source_path = ROOT / "UMVC3Hook/code/rollback_restore.cpp"
    source = source_path.read_text()
    memory_source = (ROOT / "UMVC3Hook/code/rollback_memory.cpp").read_text()
    generated = output / "current_character_restore.generated.h"
    generated.write_text(generate(source, memory_source))
    harness = ROOT / "tools/benchmark_character_restore.cpp"
    executable = output / "benchmark_character_restore"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-fno-strict-aliasing"]
    if args.sanitize:
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    else:
        flags += ["-O3", "-DNDEBUG", "-march=native"]
    compile_command = [args.compiler, *flags, "-I", str(output), str(harness), "-o", str(executable)]
    subprocess.run(compile_command, check=True)
    run_command = [str(executable), str(args.samples), str(args.ms), str(args.stream_mib)]
    if args.cpu is not None:
        run_command = ["taskset", "-c", str(args.cpu), *run_command]
    provenance = {
        "restore_source": str(source_path),
        "restore_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "memory_sha256": hashlib.sha256(memory_source.encode()).hexdigest(),
        "harness_sha256": hashlib.sha256(harness.read_bytes()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.read_bytes()).hexdigest(),
        "compile_command": compile_command,
        "run_command": run_command,
        "compiler": subprocess.check_output([args.compiler, "--version"], text=True),
        "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "scope": "Extracted common-character body + late scalar repairs; fixed-policy synthetic fixtures.",
        "exclusions": ["concrete-tail detection", "native reconstruction", "owned buffers",
                       "collision-parent repair", "shot/effect remapping", "worker barriers"],
    }
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    with (output / "results.csv").open("w") as results:
        subprocess.run(run_command, stdout=results, check=True)
    print(f"Results: {output / 'results.csv'}")


if __name__ == "__main__":
    main()
