#!/usr/bin/env python3
"""Extract current save kernels and benchmark isolated candidates, not the plugin."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from benchmark_subsystem_restore import block, between, clean

ROOT = Path(__file__).resolve().parents[1]
FILES = ["rollback_snapshot.cpp", "rollback_snapshot.h", "rollback_model_policy.cpp",
         "concrete_snapshot.cpp", "concrete_snapshot.h", "effect_generator_state.h",
         "rollback_capture_set.h", "rollback_shot.cpp", "rollback_effect.cpp"]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"source changed: {old[:100]}")
    return text.replace(old, new)


def generate(src):
    snapshot = clean(src["rollback_snapshot.cpp"]).replace(
        "diagnosticStage.Next(LagMetric::SaveShots);", "")
    policy = clean(src["rollback_model_policy.cpp"])
    concrete = clean(src["concrete_snapshot.cpp"])
    declarations = src["rollback_snapshot.h"].split("struct RollbackSnapshotSlot {")[0]
    declarations = "\n".join(s for s in declarations.splitlines()
                             if not s.startswith("#include") and s != "#pragma once")
    constants = []
    for name in ["PTR_SCHARACTER", "PTR_SUNIT", "TEAM0_OFFSET", "TEAM1_OFFSET",
                 "NODE_CHR_OFFSET", "NODE_NEXT_OFFSET", "TEAM_FIRST_NODE_OFFSET",
                 "CCHR_WEAPON_COUNT_OFFSET", "CCHR_WEAPON_ARRAY_OFFSET",
                 "SUNIT_CATEGORY_COUNT_OFFSET", "SUNIT_CATEGORY_LISTS_OFFSET",
                 "SUNIT_CATEGORY_LIST_STRIDE", "SUNIT_CHARACTER_CATEGORY"]:
        constants.append(next(s for s in snapshot.splitlines() if f" {name} =" in s))
    collision = block(snapshot, "static void CaptureCharacterCollisionParentReferences(")
    collision = collision.replace(block(collision, "if constexpr (ROLLBACK_PROFILING_ENABLED)"), "")
    collision = collision.replace("static void CaptureCharacterCollisionParentReferences", "NOINLINE void baseline_collision")
    hashless = collision.replace("baseline_collision", "hashless_collision")
    collision = replace_once(collision, "RollbackCaptureSet<uint64_t, 2 * 16> capturedCharacters;",
                             "std::unordered_set<uint64_t> capturedCharacters;")
    candidate = collision.replace("baseline_collision(RollbackSnapshotSlot& slot)",
                                  "candidate_collision(RollbackSnapshotSlot& slot, const Inventory& inventory)")
    candidate = replace_once(candidate, "std::unordered_set<uint64_t> capturedCharacters;", "SmallSet<uint64_t, 32> capturedCharacters;")
    start = candidate.index("    for (int team = 0;")
    candidate = candidate[:start] + "    for (auto character : inventory.roster) captureCharacter(character);\n}"
    cached = candidate.replace("candidate_collision(", "cached_collision(")
    cached = replace_once(cached, "auto captureParent =", "uint64_t lastParent = 0; uint32_t lastWeapon = 0;\n        auto captureParent =")
    search = block(cached, "while (weaponIndex < weaponCount")
    cached = replace_once(cached, search,
        "if (parent == lastParent) weaponIndex = lastWeapon; else {" + search +
        "lastParent = parent; lastWeapon = weaponIndex; }")
    render = between(snapshot, "    slot.modelRenderStates.clear();", "    Shot_CaptureRecipes(slot);")
    render = "slot.modelRenderStates.clear();" + render
    candidate_render = render[:render.index("    uint64_t renderCharacterManager")]
    candidate_render += "for (auto p : inventory.renderRoster) captureCharacterRenderState((void*)p);\n"
    candidate_render += "for (auto p : inventory.units) captureCharacterRenderState((void*)p);\n"
    membership = between(policy, "    RollbackCaptureSet<void*, 2 * 16 + 64> characters;", "    auto findUniqueOwnedShot")
    membership = "RollbackCaptureSet<void*, 2 * 16 + 64> characters;" + membership
    membership += "size_t n = 0; for (auto p : queries) n += characters.count(p); return n;"
    baseline_membership = replace_once(membership, "RollbackCaptureSet<void*, 2 * 16 + 64> characters;",
                                        "std::unordered_set<void*> characters;")
    kernels = ["\n".join(constants), declarations]
    kernels += ["#include \"save_fixture.h\"", collision, hashless, candidate, cached,
                "NOINLINE void baseline_render(RollbackSnapshotSlot& slot) {" + render + "}",
                "NOINLINE void candidate_render(RollbackSnapshotSlot& slot, const Inventory& inventory) {" + candidate_render + "}",
                "NOINLINE size_t baseline_membership(const std::vector<void*>& queries) {" + baseline_membership + "}"]
    kernels.append("NOINLINE size_t hashless_membership(const std::vector<void*>& queries) {" +
                   membership + "}")
    for name in ["kShotBodyStart", "kMaxObjectSize", "kShotCallbackEffectsOffset",
                 "kShotCallbackEffectCount", "kEffectDti"]:
        kernels.append(next(s for s in concrete.splitlines() if f" {name} =" in s))
    kernels.append('#include "save_capture_fixture.h"')
    for kind, param in [("Shot", "out"), ("Effect", "state")]:
        fn = block(concrete, f"bool Concrete{kind}Snapshot_Capture(")
        baseline = fn.replace(f"Concrete{kind}Snapshot_Capture", f"baseline_{kind.lower()}")
        candidate = fn.replace(f"Concrete{kind}Snapshot_Capture", f"candidate_{kind.lower()}")
        baseline = replace_once(baseline, f"{param}.ResetForCapture();", f"{param} = {{}};")
        kernels += ["NOINLINE " + baseline, "NOINLINE " + candidate]
    for kind, end in [("Shot", "saved.savedPtr = shot;"), ("Effect", "saved.a8 =")]:
        body = block(clean(src[f"rollback_{kind.lower()}.cpp"]), f"void {kind}_CaptureRecipes(")
        setup = "if (capturedCount ==" + between(body, "if (capturedCount ==", end)
        setup = setup.replace("recipe->second" if kind == "Shot" else "AssignCaptureMetadata(recipe)",
                              "source" if kind == "Shot" else "AssignCaptureMetadata(source)")
        kernels.append(f"static Rollback{kind}Recipe& reuse_{kind.lower()}(RollbackSnapshotSlot& slot, "
                       f"const Rollback{kind}Recipe& source, size_t& capturedCount) {{" + setup + "return saved; }")
    for name in ["RegionDependency", "RegionDependencies"]:
        kernels.append(block(snapshot, f"struct {name} {{") + ";")
    kernels.append(block(snapshot, "static bool RegionDependenciesValid("))
    return "\n".join(kernels)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--iterations", type=int, default=20000)
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()
    if args.iterations < 1:
        p.error("positive iterations required")
    parent = Path("/tmp/opencode")
    if not parent.is_dir():
        p.error("/tmp/opencode missing")
    out = Path(tempfile.mkdtemp(prefix="save-bookkeeping-", dir=parent))
    paths = [ROOT / "UMVC3Hook/code" / f for f in FILES]
    paths += [ROOT / "tools" / f for f in ["benchmark_save_bookkeeping.py",
              "benchmark_subsystem_restore.py", "benchmark_save_bookkeeping.cpp",
              "save_fixture.h", "save_capture_fixture.h"]]
    hashes = {str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in paths}
    source = {f: (ROOT / "UMVC3Hook/code" / f).read_text() for f in FILES}
    header = out / "save.generated.h"
    header.write_text(generate(source))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-fno-strict-aliasing"]
    flags += (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
              if args.sanitize else ["-O3"])
    cmd = ["g++", *flags, "-I", str(out), "-I", str(ROOT / "tools"),
           "-I", str(ROOT / "UMVC3Hook/code"), str(ROOT / "tools/benchmark_save_bookkeeping.cpp"),
           "-o", str(out / "bench")]
    subprocess.run(cmd, check=True)
    run = ["taskset", "-c", str(min(os.sched_getaffinity(0))), str(out / "bench"), str(args.iterations)]
    result = subprocess.run(run, capture_output=True, text=True, check=True)
    for f, digest in hashes.items():
        if hashlib.sha256((ROOT / f).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"input changed: {f}")
    (out / "results.csv").write_text(result.stdout)
    (out / "checks.txt").write_text(result.stderr)
    (out / "metadata.json").write_text(json.dumps(dict(inputs=hashes, compile=cmd, run=run,
        generated_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
        compiler=subprocess.check_output(["g++", "--version"], text=True),
        hardware=subprocess.check_output(["lscpu"], text=True), sanitizer=args.sanitize), indent=2))
    print(f"Artifacts: {out}")
    print(result.stderr, end="")


if __name__ == "__main__":
    main()
