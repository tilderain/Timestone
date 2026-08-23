#!/usr/bin/env python3
"""Launch and compare frame-limited UMVC3 rollback profiling runs.

Examples:
  python tools/rollback_profile_ai.py run --label baseline --frames 2000
  python tools/rollback_profile_ai.py run --label candidate --frames 2000 \
      --compare-to rollback_profile_runs/20260822T120000_baseline
  python tools/rollback_profile_ai.py compare BASELINE_RUN CANDIDATE_RUN

Each run launches ``wine umvc3.exe -training-cpus`` with an in-game fighting
frame limit. Profiles created after the launch timestamp are copied and merged
into a labeled run directory. Comparisons always use explicit run directories;
they never guess that arbitrary pre-existing profiles form a valid A/B pair.
"""

import argparse
import datetime as dt
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


EXIT_OK = 0
EXIT_USAGE = 2
EXIT_LAUNCH_FAILED = 3
EXIT_TIMEOUT = 4
EXIT_MISSING_ARTIFACTS = 5
EXIT_INSUFFICIENT_SIGNAL = 6
EXIT_COMPARE_FAILED = 7
EXIT_CORRECTNESS_REGRESSION = 8

MIN_WINDOWS = 30
MIN_REPLAYED_FRAMES = 200
FRAME_LIMIT_MESSAGE = "FrameStepper: training frame limit reached"
SIDE_PATTERN = re.compile(r"_(host|client)(?:_r\d+)?$")


def utc_now():
    return dt.datetime.now(dt.timezone.utc)


def emit(document, path=None, print_output=True):
    rendered = json.dumps(document, indent=2, sort_keys=True)
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rendered + "\n", encoding="utf-8")
    if print_output:
        print(rendered)


def safe_label(label):
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", label.strip()).strip("._")
    return cleaned or "run"


def percentile(values, quantile):
    if not values:
        return 0.0
    ordered = sorted(float(value) for value in values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def finite_nonnegative(value):
    return isinstance(value, (int, float)) and math.isfinite(value) and value >= 0


def aggregate_numeric_objects(entries, field):
    totals = {}
    for entry in entries:
        obj = entry.get(field, {})
        if not isinstance(obj, dict):
            continue
        for name, value in obj.items():
            if finite_nonnegative(value):
                if name.endswith("_max") or name.endswith("_max_ms") or name == "max_ms":
                    totals[name] = max(totals.get(name, 0.0), float(value))
                else:
                    totals[name] = totals.get(name, 0.0) + float(value)
    return totals


def aggregate_snapshots(entries):
    totals = {}
    count = 0.0
    weighted_kib = 0.0
    weighted_regions = 0.0
    for entry in entries:
        snapshot = entry.get("snapshot", {})
        if not isinstance(snapshot, dict):
            continue
        snapshot_count = snapshot.get("count", 0)
        if not finite_nonnegative(snapshot_count):
            continue
        count += snapshot_count
        weighted_kib += snapshot.get("avg_kib", 0) * snapshot_count
        weighted_regions += snapshot.get("avg_regions", 0) * snapshot_count
        for name, value in snapshot.items():
            if name in {"count", "avg_ms", "avg_kib", "avg_regions"}:
                continue
            if finite_nonnegative(value):
                totals[name] = totals.get(name, 0.0) + float(value)
    totals["count"] = count
    totals["avg_ms"] = totals.get("total_ms", 0.0) / count if count else 0.0
    totals["avg_kib"] = weighted_kib / count if count else 0.0
    totals["avg_regions"] = weighted_regions / count if count else 0.0
    return totals


def aggregate_rows(entries, field, key_fields):
    rows = {}
    for entry in entries:
        values = entry.get(field, [])
        if not isinstance(values, list):
            continue
        for value in values:
            if not isinstance(value, dict):
                continue
            key = tuple(value.get(name) for name in key_fields)
            aggregate = rows.setdefault(key, {name: value.get(name) for name in key_fields})
            for name in ("visits", "calls", "total_ms"):
                item = value.get(name, 0)
                if finite_nonnegative(item):
                    aggregate[name] = aggregate.get(name, 0) + item
            maximum = value.get("max_us", 0)
            if finite_nonnegative(maximum):
                aggregate["max_us"] = max(aggregate.get("max_us", 0), maximum)
    for aggregate in rows.values():
        calls = aggregate.get("calls", 0)
        aggregate["avg_us"] = (
            aggregate.get("total_ms", 0.0) * 1000.0 / calls if calls else 0.0
        )
    return sorted(rows.values(), key=lambda row: row.get("total_ms", 0), reverse=True)


def summarize_profile(entries, side):
    if not isinstance(entries, list):
        raise ValueError(f"{side} profile root is not a JSON array")
    rollback = [entry for entry in entries
                if isinstance(entry, dict) and entry.get("type") == "rollback"]
    post_resim = [entry for entry in entries
                  if isinstance(entry, dict) and entry.get("type") == "post_resim"]
    errors = []
    for index, entry in enumerate(rollback):
        if not isinstance(entry.get("frame_count"), int) or entry["frame_count"] <= 0:
            errors.append(f"rollback[{index}] has invalid frame_count")
        for field in ("total_ms", "avg_frame_ms", "max_frame_ms"):
            if not finite_nonnegative(entry.get(field)):
                errors.append(f"rollback[{index}] has invalid {field}")
        if entry.get("ndjson_enabled") is not False:
            errors.append(f"rollback[{index}] has NDJSON enabled")

    frame_counts = [entry.get("frame_count", 0) for entry in rollback
                    if isinstance(entry.get("frame_count"), int)]
    total_values = [float(entry.get("total_ms", 0)) for entry in rollback
                    if finite_nonnegative(entry.get("total_ms"))]
    frame_max_values = [float(entry.get("max_frame_ms", 0)) for entry in rollback
                        if finite_nonnegative(entry.get("max_frame_ms"))]
    replayed_frames = sum(max(0, value) for value in frame_counts)
    total_ms = sum(total_values)

    phases = aggregate_numeric_objects(rollback, "phases_ms")
    headless = aggregate_numeric_objects(rollback, "headless_tick")
    snapshot = aggregate_snapshots(rollback)
    restore = aggregate_numeric_objects(rollback, "restore")
    shot_restore = aggregate_numeric_objects(rollback, "shot_restore")
    post = {name: sum(float(entry.get(name, 0)) for entry in post_resim
                      if finite_nonnegative(entry.get(name, 0)))
            for name in ("notify_ms", "resolve_ms", "save_snapshot_ms", "total_ms")}

    summary = {
        "side": side,
        "valid": not errors,
        "errors": errors,
        "rollback_windows": len(rollback),
        "replayed_frames": replayed_frames,
        "total_rollback_ms": total_ms,
        "ms_per_replayed_frame": total_ms / replayed_frames if replayed_frames else 0.0,
        "average_depth": replayed_frames / len(rollback) if rollback else 0.0,
        "median_depth": percentile(frame_counts, 0.5),
        "maximum_depth": max(frame_counts, default=0),
        "average_window_ms": total_ms / len(rollback) if rollback else 0.0,
        "p50_window_ms": percentile(total_values, 0.5),
        "p95_window_ms": percentile(total_values, 0.95),
        "maximum_window_ms": max(total_values, default=0.0),
        "p50_max_frame_ms": percentile(frame_max_values, 0.5),
        "p95_max_frame_ms": percentile(frame_max_values, 0.95),
        "maximum_frame_ms": max(frame_max_values, default=0.0),
        "multi_resim_values": sorted({entry.get("multi_resim_enabled") for entry in rollback},
                                      key=str),
        "phases_ms": phases,
        "headless_tick": headless,
        "snapshot": snapshot,
        "restore": restore,
        "shot_restore": shot_restore,
        "post_resim_count": len(post_resim),
        "post_resim": post,
        "unit_categories": aggregate_rows(
            rollback, "unit_categories", ("phase", "category", "name")),
        "unit_objects": aggregate_rows(
            rollback, "unit_objects", ("phase", "category", "dti", "name")),
        "hooks": aggregate_rows(rollback, "hooks", ("name",)),
    }
    phase_total = sum(value for name, value in phases.items()
                      if name != "native_tick_max")
    summary["unattributed_ms"] = total_ms - phase_total
    return summary


def combined_summary(sides):
    valid_sides = [summary for summary in sides.values() if summary]
    windows = sum(summary["rollback_windows"] for summary in valid_sides)
    frames = sum(summary["replayed_frames"] for summary in valid_sides)
    total_ms = sum(summary["total_rollback_ms"] for summary in valid_sides)
    errors = [f"{summary['side']}: {error}" for summary in valid_sides
              for error in summary["errors"]]
    return {
        "valid": not errors,
        "errors": errors,
        "rollback_windows": windows,
        "replayed_frames": frames,
        "total_rollback_ms": total_ms,
        "ms_per_replayed_frame": total_ms / frames if frames else 0.0,
        "profile_ready": not errors and windows >= MIN_WINDOWS and
                         frames >= MIN_REPLAYED_FRAMES,
        "minimum_windows": MIN_WINDOWS,
        "minimum_replayed_frames": MIN_REPLAYED_FRAMES,
    }


def artifact_snapshot(game_dir):
    snapshot = {}
    for pattern in ("nplive_*/rollback_profile.json", "nplive_*/Timestone_*.log",
                    "nplive_*/umvc3_crash.log", "desync_*.usdp"):
        for path in game_dir.glob(pattern):
            try:
                stat = path.stat()
            except OSError:
                continue
            snapshot[str(path)] = (stat.st_mtime_ns, stat.st_size)
    return snapshot


def changed_since(path, not_before, before):
    try:
        stat = path.stat()
    except OSError:
        return False
    previous = before.get(str(path))
    return stat.st_mtime >= not_before and previous != (stat.st_mtime_ns, stat.st_size)


def profile_side(path):
    match = SIDE_PATTERN.search(path.parent.name)
    return match.group(1) if match else None


def recent_profiles(game_dir, not_before, before):
    profiles = {"host": [], "client": []}
    for path in game_dir.glob("nplive_*/rollback_profile.json"):
        side = profile_side(path)
        if side and changed_since(path, not_before, before):
            profiles[side].append(path)
    for paths in profiles.values():
        paths.sort(key=lambda path: path.stat().st_mtime_ns)
    return profiles


def recent_logs(game_dir, not_before, before):
    logs = {"host": [], "client": []}
    for path in game_dir.glob("nplive_*/Timestone_*.log"):
        side = profile_side(path)
        if side and changed_since(path, not_before, before):
            logs[side].append(path)
    return logs


def completion_sides(game_dir, not_before, before):
    complete = set()
    for side, paths in recent_logs(game_dir, not_before, before).items():
        for path in paths:
            try:
                if FRAME_LIMIT_MESSAGE in path.read_text(
                        encoding="utf-8", errors="replace"):
                    complete.add(side)
                    break
            except OSError:
                continue
    return complete


def recent_crashes(game_dir, not_before, before):
    crashes = []
    for path in game_dir.glob("nplive_*/umvc3_crash.log"):
        if not changed_since(path, not_before, before):
            continue
        try:
            if "=== UMVC3Hook crash" in path.read_text(
                    encoding="utf-8", errors="replace"):
                crashes.append(str(path))
        except OSError:
            continue
    return sorted(crashes)


def recent_desyncs(game_dir, not_before, before):
    return sorted(str(path) for path in game_dir.glob("desync_*.usdp")
                  if changed_since(path, not_before, before))


def load_and_merge(paths):
    merged = []
    for path in paths:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream)
        if not isinstance(document, list):
            raise ValueError(f"{path} does not contain a JSON array")
        merged.extend(document)
    return merged


def resolve_path(path):
    return path.expanduser().resolve()


def build_run_plan(args):
    game_dir = resolve_path(args.game_dir)
    game = args.game.expanduser()
    if not game.is_absolute():
        game = game_dir / game
    game = game.resolve()
    command = [args.wine, str(game), "-training-cpus",
               f"-training-frame-limit={args.frames}"]
    run_id = utc_now().strftime("%Y%m%dT%H%M%S.%fZ")
    output_root = resolve_path(args.output_root)
    run_dir = output_root / f"{run_id}_{safe_label(args.label)}"
    return {
        "schema": "umvc3_rollback_profile_plan_v1",
        "run_id": run_id,
        "label": args.label,
        "frames": args.frames,
        "executable": str(game),
        "cwd": str(game_dir),
        "argv": command,
        "wine_prefix": args.wine_prefix,
        "run_dir": str(run_dir),
    }


def wait_for_both_completions(game_dir, not_before, before, timeout):
    deadline = time.monotonic() + timeout
    complete = set()
    while time.monotonic() < deadline:
        complete = completion_sides(game_dir, not_before, before)
        if complete == {"host", "client"}:
            return complete
        time.sleep(0.1)
    return complete


def copy_sources(paths, destination, side):
    copied = []
    source_dir = destination / "source"
    source_dir.mkdir(parents=True, exist_ok=True)
    for index, path in enumerate(paths):
        target = source_dir / f"{side}_{index:02d}_{path.parent.name}.json"
        shutil.copy2(path, target)
        copied.append(str(target))
    return copied


def write_run_artifacts(plan, profiles, started_at, game_exit_code,
                        completions, crashes, desyncs):
    run_dir = Path(plan["run_dir"])
    run_dir.mkdir(parents=True, exist_ok=True)
    sides = {}
    copied_sources = {}
    errors = []
    for side in ("host", "client"):
        paths = profiles[side]
        copied_sources[side] = copy_sources(paths, run_dir, side)
        try:
            merged = load_and_merge(paths)
            merged_path = run_dir / f"{side}_profile.json"
            merged_path.write_text(json.dumps(merged, indent=2) + "\n",
                                   encoding="utf-8")
            sides[side] = summarize_profile(merged, side)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            sides[side] = None
            errors.append(f"{side}: {error}")

    combined = combined_summary(sides)
    errors.extend(combined["errors"])
    if crashes:
        outcome = "game_crashed"
        exit_code = EXIT_LAUNCH_FAILED
    elif desyncs:
        outcome = "correctness_regression"
        exit_code = EXIT_CORRECTNESS_REGRESSION
    elif completions != {"host", "client"}:
        outcome = "run_incomplete"
        exit_code = EXIT_MISSING_ARTIFACTS
    elif not profiles["host"] or not profiles["client"] or errors:
        outcome = "missing_or_invalid_profiles"
        exit_code = EXIT_MISSING_ARTIFACTS
    elif not combined["profile_ready"]:
        outcome = "insufficient_signal"
        exit_code = EXIT_INSUFFICIENT_SIGNAL
    else:
        outcome = "profile_ready"
        exit_code = EXIT_OK

    document = {
        **plan,
        "schema": "umvc3_rollback_profile_run_v1",
        "started_at": dt.datetime.fromtimestamp(
            started_at, dt.timezone.utc).isoformat(),
        "finished_at": utc_now().isoformat(),
        "game_exit_code": game_exit_code,
        "frame_limit_completed_sides": sorted(completions),
        "crash_artifacts": crashes,
        "desync_artifacts": desyncs,
        "source_profiles": {side: [str(path) for path in profiles[side]]
                            for side in profiles},
        "copied_profiles": copied_sources,
        "sides": sides,
        "combined": combined,
        "errors": errors,
        "outcome": outcome,
        "exit_code": exit_code,
    }
    emit(document, run_dir / "summary.json", print_output=False)
    emit({
        "schema": "umvc3_rollback_profile_run_result_v1",
        "run_dir": str(run_dir),
        "outcome": outcome,
        "exit_code": exit_code,
        "completed_sides": sorted(completions),
        "crash_artifacts": crashes,
        "desync_artifacts": desyncs,
        "combined": combined,
    })
    return document


def load_run(path):
    path = resolve_path(path)
    summary = path / "summary.json" if path.is_dir() else path
    with summary.open("r", encoding="utf-8") as stream:
        document = json.load(stream)
    if document.get("schema") != "umvc3_rollback_profile_run_v1":
        raise ValueError(f"{summary} is not a rollback profile run")
    return document, summary


def percent_change(baseline, candidate):
    if baseline == 0:
        return None
    return (candidate - baseline) * 100.0 / baseline


def compare_metric(baseline, candidate):
    change = percent_change(baseline, candidate)
    return {
        "baseline": baseline,
        "candidate": candidate,
        "change": candidate - baseline,
        "change_percent": change,
        "improvement_percent": -change if change is not None else None,
    }


def compare_bucket_per_frame(before, after, before_frames, after_frames):
    names = set(before) | set(after)
    compared = {}
    for name in sorted(names):
        baseline = before.get(name, 0)
        candidate = after.get(name, 0)
        if not finite_nonnegative(baseline) or not finite_nonnegative(candidate):
            continue
        if (name.startswith("avg_") or name.endswith("_max") or
                name.endswith("_max_ms") or name == "max_ms"):
            compared[name] = compare_metric(baseline, candidate)
        else:
            compared[f"{name}_per_replayed_frame"] = compare_metric(
                baseline / max(1, before_frames),
                candidate / max(1, after_frames),
            )
    return compared


def compare_ranked_rows(before, after, key_fields, before_frames, after_frames):
    before_rows = {tuple(row.get(name) for name in key_fields): row for row in before}
    after_rows = {tuple(row.get(name) for name in key_fields): row for row in after}
    compared = []
    for key in set(before_rows) | set(after_rows):
        baseline = before_rows.get(key, {})
        candidate = after_rows.get(key, {})
        row = {name: key[index] for index, name in enumerate(key_fields)}
        row["total_ms_per_replayed_frame"] = compare_metric(
            baseline.get("total_ms", 0) / max(1, before_frames),
            candidate.get("total_ms", 0) / max(1, after_frames),
        )
        row["calls_per_replayed_frame"] = compare_metric(
            baseline.get("calls", 0) / max(1, before_frames),
            candidate.get("calls", 0) / max(1, after_frames),
        )
        row["max_us"] = compare_metric(
            baseline.get("max_us", 0), candidate.get("max_us", 0))
        compared.append(row)
    return sorted(
        compared,
        key=lambda row: max(
            row["total_ms_per_replayed_frame"]["baseline"],
            row["total_ms_per_replayed_frame"]["candidate"],
        ),
        reverse=True,
    )


def compare_runs(baseline, candidate):
    errors = []
    if not baseline.get("combined", {}).get("profile_ready"):
        errors.append("baseline is not profile_ready")
    if not candidate.get("combined", {}).get("profile_ready"):
        errors.append("candidate is not profile_ready")
    if baseline.get("frames") != candidate.get("frames"):
        errors.append("frame limits differ")

    sides = {}
    metric_names = (
        "ms_per_replayed_frame", "average_window_ms", "p50_window_ms",
        "p95_window_ms", "maximum_window_ms", "p95_max_frame_ms",
        "maximum_frame_ms",
    )
    for side in ("host", "client"):
        before = baseline.get("sides", {}).get(side)
        after = candidate.get("sides", {}).get(side)
        if not before or not after:
            errors.append(f"{side} summary missing")
            continue
        if before.get("multi_resim_values") != after.get("multi_resim_values"):
            errors.append(f"{side} multi-resim settings differ")
        side_metrics = {name: compare_metric(before[name], after[name])
                        for name in metric_names}
        before_frames = before["replayed_frames"]
        after_frames = after["replayed_frames"]
        for field in ("phases_ms", "headless_tick", "snapshot", "restore",
                      "shot_restore", "post_resim"):
            side_metrics[field] = compare_bucket_per_frame(
                before.get(field, {}), after.get(field, {}),
                before_frames, after_frames)
        side_metrics["unit_categories"] = compare_ranked_rows(
            before.get("unit_categories", []), after.get("unit_categories", []),
            ("phase", "category", "name"), before_frames, after_frames)
        side_metrics["unit_objects"] = compare_ranked_rows(
            before.get("unit_objects", []), after.get("unit_objects", []),
            ("phase", "category", "dti", "name"), before_frames, after_frames)
        side_metrics["hooks"] = compare_ranked_rows(
            before.get("hooks", []), after.get("hooks", []), ("name",),
            before_frames, after_frames)
        sides[side] = side_metrics

    combined = compare_metric(
        baseline.get("combined", {}).get("ms_per_replayed_frame", 0),
        candidate.get("combined", {}).get("ms_per_replayed_frame", 0),
    )
    return {
        "schema": "umvc3_rollback_profile_comparison_v1",
        "created_at": utc_now().isoformat(),
        "baseline": {"run_id": baseline.get("run_id"),
                     "label": baseline.get("label"),
                     "run_dir": baseline.get("run_dir")},
        "candidate": {"run_id": candidate.get("run_id"),
                      "label": candidate.get("label"),
                      "run_dir": candidate.get("run_dir")},
        "comparable": not errors,
        "errors": errors,
        "combined_ms_per_replayed_frame": combined,
        "sides": sides,
        "verdict": "comparison_ready" if not errors else "comparison_failed",
    }


def run_command(args):
    plan = build_run_plan(args)
    run_dir = Path(plan["run_dir"])
    run_dir.mkdir(parents=True, exist_ok=False)
    game_dir = Path(plan["cwd"])
    before = artifact_snapshot(game_dir)
    started_at = time.time()
    env = dict(os.environ)
    if args.wine_prefix:
        env["WINEPREFIX"] = args.wine_prefix
    try:
        result = subprocess.run(
            plan["argv"], cwd=game_dir, env=env, timeout=args.timeout,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        game_exit_code = result.returncode
    except subprocess.TimeoutExpired:
        document = {**plan, "schema": "umvc3_rollback_profile_run_v1",
                    "started_at": dt.datetime.fromtimestamp(
                        started_at, dt.timezone.utc).isoformat(),
                    "finished_at": utc_now().isoformat(), "outcome": "timeout",
                    "exit_code": EXIT_TIMEOUT}
        emit(document, run_dir / "summary.json")
        return EXIT_TIMEOUT
    except OSError as error:
        document = {**plan, "schema": "umvc3_rollback_profile_run_v1",
                    "started_at": dt.datetime.fromtimestamp(
                        started_at, dt.timezone.utc).isoformat(),
                    "finished_at": utc_now().isoformat(),
                    "outcome": "launch_failed", "error": str(error),
                    "exit_code": EXIT_LAUNCH_FAILED}
        emit(document, run_dir / "summary.json")
        return EXIT_LAUNCH_FAILED

    completions = wait_for_both_completions(
        game_dir, started_at, before, args.settle_timeout)
    profiles = recent_profiles(game_dir, started_at, before)
    crashes = recent_crashes(game_dir, started_at, before)
    desyncs = recent_desyncs(game_dir, started_at, before)
    document = write_run_artifacts(
        plan, profiles, started_at, game_exit_code, completions, crashes, desyncs)

    if args.compare_to:
        try:
            baseline, _ = load_run(args.compare_to)
            comparison = compare_runs(baseline, document)
            report = run_dir / "comparison.json"
            emit(comparison, report, print_output=False)
            emit({
                "schema": "umvc3_rollback_profile_comparison_result_v1",
                "report": str(report),
                "verdict": comparison["verdict"],
                "comparable": comparison["comparable"],
                "errors": comparison["errors"],
                "combined_ms_per_replayed_frame":
                    comparison["combined_ms_per_replayed_frame"],
            })
            if not comparison["comparable"]:
                return EXIT_COMPARE_FAILED
        except (OSError, ValueError, json.JSONDecodeError) as error:
            emit({"schema": "umvc3_rollback_profile_comparison_v1",
                  "verdict": "comparison_failed", "errors": [str(error)]},
                 run_dir / "comparison.json")
            return EXIT_COMPARE_FAILED
    return document["exit_code"]


def compare_command(args):
    try:
        baseline, _ = load_run(args.baseline)
        candidate, _ = load_run(args.candidate)
        document = compare_runs(baseline, candidate)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        document = {"schema": "umvc3_rollback_profile_comparison_v1",
                    "verdict": "comparison_failed", "comparable": False,
                    "errors": [str(error)]}
    emit(document, args.output)
    return EXIT_OK if document.get("comparable") else EXIT_COMPARE_FAILED


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    run = subparsers.add_parser("run", help="launch and capture a profiling run")
    run.add_argument("--label", required=True, help="baseline or candidate label")
    run.add_argument("--frames", type=int, default=2000,
                     help="native fighting frames to run (default: 2000)")
    run.add_argument("--game", type=Path, default=Path("umvc3.exe"))
    run.add_argument("--game-dir", type=Path,
                     default=Path("../ULTIMATE MARVEL VS. CAPCOM 3"))
    run.add_argument("--wine", default="wine")
    run.add_argument("--wine-prefix", default=None)
    run.add_argument("--timeout", type=float, default=300,
                     help="hard launch timeout in seconds (default: 300)")
    run.add_argument("--settle-timeout", type=float, default=15,
                     help="seconds to wait for both completion logs")
    run.add_argument("--output-root", type=Path,
                     default=Path("rollback_profile_runs"))
    run.add_argument("--compare-to", type=Path,
                     help="explicit baseline run directory or summary JSON")
    run.set_defaults(handler=run_command)

    compare = subparsers.add_parser("compare", help="compare two captured runs")
    compare.add_argument("baseline", type=Path)
    compare.add_argument("candidate", type=Path)
    compare.add_argument("--output", type=Path)
    compare.set_defaults(handler=compare_command)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    if args.command == "run":
        if args.frames < 1 or args.frames > 10000000:
            parser.error("--frames must be between 1 and 10000000")
        if args.timeout <= 0 or args.settle_timeout < 0:
            parser.error("timeouts must be positive")
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
