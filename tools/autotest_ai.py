#!/usr/bin/env python3
"""Run a focused UMVC3 autotest and prepare an AI desync report.

Examples:
  python tools/autotest_ai.py --character Ryu --moves "12,Hadoken"
  python tools/autotest_ai.py --character Ryu --failed-tsv \
      ".../autotest/20260728_193825_519_hawkeye_commands.tsv" --launch

The game itself writes the TSV and host/client snapdumps. When --launch is
used, the game is started through Wine from the game directory, and this
script waits for the pair to exit, then compares the newest matching snapdump
pair with tools/compare_desync_dumps.py.
"""

import argparse
import csv
import datetime as dt
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


EXIT_OK = 0
EXIT_DESYNC = 1
EXIT_USAGE = 2
EXIT_LAUNCH_FAILED = 3
EXIT_TIMEOUT = 4
EXIT_MISSING_ARTIFACTS = 5
EXIT_ANALYZER_FAILED = 6


def failed_moves(path):
    moves = []
    with path.open("r", encoding="utf-8", errors="replace", newline="") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if row.get("result", "").upper() not in {"DESYNC", "FAIL", "ERROR"}:
                continue
            matcher_id = row.get("matcher_id", "").strip()
            line_id = row.get("line_id", "").strip()
            if matcher_id not in {"", "-1"} and line_id not in {"", "-1"}:
                selector = f"{matcher_id}:{line_id}"
            else:
                selector = row.get("move", "").replace("\n", " ").strip()
            if selector and selector not in moves:
                moves.append(selector)
    return moves


def quote_filter(moves):
    return ",".join(move.replace(",", " ") for move in moves)


def latest_report_pair(game_dir, character, not_before=None):
    """Find the newest complete pair in the character's autotest folder.

    Current autotest output uses ``host_<move-id>.usdp`` and
    ``client_<move-id>.usdp``. Keep support for the older timestamped names so
    existing runs can still be analyzed.
    """
    character_dir = game_dir / "autotest" / character
    search_dirs = [character_dir]
    # Older builds wrote timestamped dumps directly in the game directory.
    if game_dir != character_dir:
        search_dirs.append(game_dir)
    current_pattern = re.compile(r"(host|client)_(\d+)\.usdp$")
    legacy_pattern = re.compile(
        r"desync_(\d{8}_\d{6})_(host|client)_f(-?\d+)\.usdp$")
    entries = []
    for directory in search_dirs:
        if not directory.is_dir():
            continue
        for path in directory.glob("*.usdp"):
            if not_before is not None and path.stat().st_mtime < not_before:
                continue
            match = current_pattern.fullmatch(path.name)
            if match:
                entries.append((path.stat().st_mtime, match.group(1),
                                int(match.group(2)), path, None))
                continue
            match = legacy_pattern.fullmatch(path.name)
            if match:
                stamp = dt.datetime.strptime(match.group(1), "%Y%m%d_%H%M%S")
                entries.append((stamp.timestamp(), match.group(2),
                                int(match.group(3)), path, stamp))
    for host in sorted((entry for entry in entries if entry[1] == "host"),
                       reverse=True):
        clients = [entry for entry in entries
                   if entry[1] == "client" and entry[2] == host[2]
                   and (host[4] is None or entry[4] is None or
                        abs(entry[4] - host[4]) <= dt.timedelta(seconds=5))]
        if clients:
            return host[3], max(clients)[3]
    return None


def suite_completed(game_dir, not_before):
    """Accept Wine's shutdown crash when the autotest itself completed."""
    log_path = game_dir / "UMVC3Hook_host.log"
    if log_path.is_file() and log_path.stat().st_mtime >= not_before:
        try:
            log = log_path.read_text(encoding="utf-8", errors="replace")
            if "AutoTestSuite: command-line suite complete" in log:
                return True
        except OSError:
            pass
    for path in (game_dir / "autotest").rglob("*_commands.tsv"):
        if path.stat().st_mtime >= not_before:
            return True
    return False


def crash_artifact_snapshot(game_dir):
    """Capture existing crash logs before launching the game."""
    snapshot = {}
    for path in game_dir.glob("nplive_*/umvc3_crash.log"):
        try:
            stat = path.stat()
        except OSError:
            continue
        snapshot[str(path)] = (stat.st_mtime_ns, stat.st_size)
    return snapshot


def recent_crash_artifacts(game_dir, not_before, before=None):
    """Return crash logs written by this launch.

    Wine can return zero after a child game process crashes, so the process
    status is not sufficient evidence that the game completed successfully.
    The plugin writes one crash log per nplive client/host session; treat any
    fresh log as authoritative failure evidence.
    """
    artifacts = []
    for path in game_dir.glob("nplive_*/umvc3_crash.log"):
        try:
            if not path.is_file():
                continue
            stat = path.stat()
            previous = before.get(str(path)) if before is not None else None
            changed = previous is None or previous != (stat.st_mtime_ns, stat.st_size)
            if not changed or stat.st_mtime < not_before:
                continue
            # Only plugin crash records are authoritative. This prevents an
            # unrelated file with the same name from affecting classification.
            with path.open("r", encoding="utf-8", errors="replace") as stream:
                if "=== UMVC3Hook crash" not in stream.read():
                    continue
            artifacts.append(str(path))
        except OSError:
            continue
    return sorted(artifacts)


def crash_artifact_details(path):
    """Extract stable fields from a plugin crash log for run manifests."""
    details = {"path": str(path)}
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return details
    patterns = {
        "commit": r"^commit=([^\r\n]+)",
        "code": r"^code=(0x[0-9A-Fa-f]+)",
        "rip": r"^RIP=(0x[0-9A-Fa-f]+)",
        "access": r"^access=([^ ]+) address=(0x[0-9A-Fa-f]+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text, re.MULTILINE)
        if not match:
            continue
        if key == "access":
            details["access"] = match.group(1)
            details["access_address"] = match.group(2)
        else:
            details[key] = match.group(1)
    return details


def recent_desync_results(game_dir, character, not_before):
    """Return DESYNC/FAIL/ERROR TSV rows produced by this run."""
    results = []
    for path in (game_dir / "autotest").rglob("*_commands.tsv"):
        if path.stat().st_mtime < not_before:
            continue
        try:
            with path.open("r", encoding="utf-8", errors="replace",
                           newline="") as stream:
                for row in csv.DictReader(stream, delimiter="\t"):
                    if row.get("character", "").casefold() != character.casefold():
                        continue
                    if row.get("result", "").upper() in {"DESYNC", "FAIL", "ERROR"}:
                        results.append({"tsv": str(path), "move": row.get("move", ""),
                                        "result": row.get("result", ""),
                                        "mismatch_frame": row.get("mismatch_frame", ""),
                                        "reason": row.get("reason", "")})
        except OSError:
            continue
    return results


def validate_suite_rows(game_dir, character, not_before):
    """Return malformed runtime rows that cannot identify their command."""
    invalid = []
    for path in (game_dir / "autotest").rglob("*_commands.tsv"):
        if path.stat().st_mtime < not_before:
            continue
        try:
            with path.open("r", encoding="utf-8", errors="replace",
                           newline="") as stream:
                for row in csv.DictReader(stream, delimiter="\t"):
                    if row.get("character", "").casefold() != character.casefold():
                        continue
                    if row.get("suite_type", "") != "commands":
                        continue
                    # Built-in manual scripts do not come from either command
                    # matcher and therefore legitimately use -1/-1.
                    if row.get("move", "") in {"Forward Throw", "Back Throw"}:
                        continue
                    if row.get("matcher_id") in {None, "", "-1"} or \
                       row.get("line_id") in {None, "", "-1"}:
                        invalid.append({"tsv": str(path), "move": row.get("move", ""),
                                        "move_id": row.get("move_id", "")})
        except OSError:
            continue
    return invalid


def build_plan(args, moves):
    game = args.game.expanduser()
    game_dir = args.game_dir.expanduser()
    if not game.is_absolute():
        game = (Path.cwd() / game).resolve()
    if not game_dir.is_absolute():
        game_dir = (Path.cwd() / game_dir).resolve()
    move_filter = quote_filter(moves)
    game_argv = [str(game), f"-autotest={args.character}",
                 f"-autotest-moves={move_filter}", f"-autotest-repeat={args.repeat}"]
    argv = [args.wine] + game_argv if args.wine else game_argv
    plan = {
        "schema": "umvc3_autotest_plan_v1",
        "run_id": dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ"),
        "character": args.character,
        "moves": moves,
        "repeat": args.repeat,
        "executable": str(game),
        "cwd": str(game_dir),
        "argv": argv,
    }
    if args.wine:
        plan["launcher"] = args.wine
    if args.wine_prefix:
        plan["wine_prefix"] = args.wine_prefix
    return plan


def emit(document, path=None):
    rendered = json.dumps(document, indent=2)
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--character", required=True)
    parser.add_argument("--moves", help="comma-separated names or runtime ordinals")
    parser.add_argument("--failed-tsv", type=Path,
                        help="derive move names from DESYNC/FAIL/ERROR rows")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--game", type=Path,
                        default=Path("../ULTIMATE MARVEL VS. CAPCOM 3/umvc3.exe"))
    parser.add_argument("--game-dir", type=Path,
                        default=Path("../ULTIMATE MARVEL VS. CAPCOM 3"))
    parser.add_argument("--launch", action="store_true")
    parser.add_argument("--json", action="store_true",
                        help="emit a machine-readable plan or run manifest")
    parser.add_argument("--wine", default="wine",
                        help="wine launcher to use when launching (default: wine)")
    parser.add_argument("--wine-prefix", default=None,
                        help="set WINEPREFIX when launching the game")
    parser.add_argument("--timeout", type=float, default=1800,
                        help="launch timeout in seconds (default: 1800)")
    parser.add_argument("--manifest", type=Path,
                        help="write the plan/run manifest to this path")
    parser.add_argument("--report", type=Path,
                        help="write comparator JSON here after --launch")
    args = parser.parse_args()

    if args.repeat < 1 or args.repeat > 100:
        parser.error("--repeat must be between 1 and 100")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.moves and args.failed_tsv:
        parser.error("use either --moves or --failed-tsv, not both")

    moves = [part.strip() for part in args.moves.split(",") if part.strip()] if args.moves else []
    if args.failed_tsv:
        moves = failed_moves(args.failed_tsv)
    if not moves:
        parser.error("provide --moves or a TSV containing failed moves")

    plan = build_plan(args, moves)
    command = plan["argv"]

    if not args.launch:
        if args.json or args.manifest:
            emit(plan, args.manifest)
        else:
            print("Launch command:")
            print(" ".join(command))
            print(f"Focused moves ({len(moves)}): {quote_filter(moves)}")
        return EXIT_OK

    started_at = time.time()
    crash_snapshot = crash_artifact_snapshot(Path(plan["cwd"]))
    manifest = {**plan, "schema": "umvc3_autotest_run_v1",
                "started_at": dt.datetime.fromtimestamp(
                    started_at, dt.timezone.utc).isoformat(), "outcome": "running"}
    env = dict(os.environ)
    if args.wine_prefix:
        env["WINEPREFIX"] = args.wine_prefix
    try:
        result = subprocess.run(command, cwd=plan["cwd"], env=env, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        manifest.update(outcome="timeout", exit_code=EXIT_TIMEOUT)
        emit(manifest, args.manifest)
        return EXIT_TIMEOUT
    except OSError as error:
        manifest.update(outcome="launch_failed", exit_code=EXIT_LAUNCH_FAILED,
                        error=str(error))
        emit(manifest, args.manifest)
        return EXIT_LAUNCH_FAILED
    completed_with_shutdown_crash = (
        result.returncode != 0 and suite_completed(Path(plan["cwd"]), started_at)
    )
    crash_artifacts = recent_crash_artifacts(
        Path(plan["cwd"]), started_at, crash_snapshot)
    if crash_artifacts:
        manifest.update(outcome="game_crashed", exit_code=EXIT_LAUNCH_FAILED,
                        game_exit_code=result.returncode,
                        shutdown_crash=False,
                        crash_artifacts=crash_artifacts,
                        crashes=[crash_artifact_details(Path(path))
                                 for path in crash_artifacts])
        emit(manifest, args.manifest)
        return EXIT_LAUNCH_FAILED
    if result.returncode != 0 and not completed_with_shutdown_crash:
        manifest.update(outcome="game_failed", exit_code=EXIT_LAUNCH_FAILED,
                        game_exit_code=result.returncode,
                        crash_artifacts=[])
        emit(manifest, args.manifest)
        return EXIT_LAUNCH_FAILED

    desync_results = recent_desync_results(
        Path(plan["cwd"]), args.character, started_at)
    invalid_rows = validate_suite_rows(
        Path(plan["cwd"]), args.character, started_at)
    pair = latest_report_pair(Path(plan["cwd"]), args.character, started_at)
    if invalid_rows:
        manifest.update(outcome="incomplete", exit_code=EXIT_LAUNCH_FAILED,
                        game_exit_code=result.returncode,
                        shutdown_crash=completed_with_shutdown_crash,
                        crash_artifacts=crash_artifacts,
                        desync_results=desync_results,
                        invalid_command_rows=invalid_rows)
        emit(manifest, args.manifest)
        return EXIT_LAUNCH_FAILED
    if not pair:
        if not suite_completed(Path(plan["cwd"]), started_at):
            manifest.update(outcome="autotest_not_started", exit_code=EXIT_LAUNCH_FAILED,
                            game_exit_code=result.returncode,
                            shutdown_crash=completed_with_shutdown_crash,
                            crash_artifacts=crash_artifacts)
            emit(manifest, args.manifest)
            return EXIT_LAUNCH_FAILED
        manifest.update(outcome="desync" if desync_results else "no_desync_artifacts",
                        exit_code=EXIT_DESYNC if desync_results else EXIT_OK,
                        game_exit_code=result.returncode,
                        shutdown_crash=completed_with_shutdown_crash,
                        crash_artifacts=crash_artifacts,
                        desync_results=desync_results,
                        invalid_command_rows=invalid_rows)
        emit(manifest, args.manifest)
        return EXIT_DESYNC if desync_results else EXIT_OK
    comparator = Path(__file__).with_name("compare_desync_dumps.py")
    report_path = args.report or Path(plan["cwd"]) / "autotest" / "latest_ai_report.json"
    if not report_path.is_absolute():
        report_path = (Path.cwd() / report_path).resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    compare_result = subprocess.run(
        [sys.executable, str(comparator), str(pair[0]), str(pair[1]),
         "--output", str(report_path)],
        cwd=Path(__file__).parents[1],
    )
    if not report_path.is_file():
        manifest.update(outcome="analyzer_failed", exit_code=EXIT_ANALYZER_FAILED,
                        analyzer_exit_code=compare_result.returncode)
        emit(manifest, args.manifest)
        return EXIT_ANALYZER_FAILED
    report = json.loads(report_path.read_text(encoding="utf-8"))
    desynced = report.get("verdict") == "diverged"
    exit_code = EXIT_DESYNC if desynced else EXIT_OK
    manifest.update(
        outcome="desync" if desynced else "sync",
        exit_code=exit_code,
        game_exit_code=result.returncode,
        shutdown_crash=completed_with_shutdown_crash,
        crash_artifacts=crash_artifacts,
        analyzer_exit_code=compare_result.returncode,
        artifacts={"host_dump": str(pair[0]), "client_dump": str(pair[1]),
                   "report": str(report_path)},
        first_divergence=report.get("first_divergence"),
        diagnosis=report.get("diagnosis"),
        desync_results=desync_results,
        invalid_command_rows=invalid_rows,
    )
    # The TSV records per-command failures; preserve that evidence even when
    # the comparator report is incomplete or uses a different verdict shape.
    if desync_results and not desynced:
        manifest["outcome"] = "desync"
        manifest["exit_code"] = EXIT_DESYNC
        exit_code = EXIT_DESYNC
    emit(manifest, args.manifest)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
