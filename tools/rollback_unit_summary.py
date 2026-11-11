#!/usr/bin/env python3
"""Summarize lifetime captures and check hold/restore/collect accounting."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re


def summarize(path):
    counts = Counter()
    held = set()
    collected = set()
    ever_held = set()
    held_by_kind = Counter()
    collected_by_kind = Counter()
    teardown = Counter()
    errors = []
    skips = Counter()
    peak = populated_holds = restored_held = 0
    active_restores = active_resurrections = 0
    first_sequence = last_sequence = None
    gaps = 0
    completed = False
    stats = None
    truncated = False
    for line in path.read_text(errors="replace").splitlines():
        truncated |= "older log entries discarded" in line
        completed |= "training frame limit reached" in line
        if line.startswith("LIFESTATS "):
            stats = dict(re.findall(r"(\w+)=(\d+)", line))
        if not line.startswith("LIFE "):
            continue
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
        event = fields["event"]
        identity = int(fields["id"])
        sequence = int(fields["seq"])
        if first_sequence is None:
            first_sequence = sequence
        elif sequence != last_sequence + 1:
            gaps += 1
        last_sequence = sequence
        counts[event] += 1
        if event == "retiring_state_restored" and int(fields.get("d", "0"), 0) == 2:
            active_restores += 1
            active_resurrections += bool(int(fields["b"], 0))
        if event == "quarantine_hold":
            if identity in held or identity in collected:
                errors.append(f"invalid hold for identity {identity}")
            held.add(identity)
            if identity not in ever_held:
                held_by_kind[fields.get("kind", "effect")] += 1
            ever_held.add(identity)
            populated_holds += int(fields["c"], 0) > 0
            peak = max(peak, len(held))
        elif event == "quarantine_collect" or (
                event == "retiring_state_restored" and int(fields["b"], 0)):
            if identity not in held:
                errors.append(f"{event} without hold for identity {identity}")
            held.discard(identity)
            if event == "quarantine_collect":
                if identity in collected:
                    errors.append(f"duplicate collection for identity {identity}")
                collected.add(identity)
                collected_by_kind[fields.get("kind", "effect")] += 1
            else:
                restored_held += 1
        elif event in ("game_effect_base_begin", "shot_base_begin", "unit_dispose_begin"):
            teardown[identity] += 1
        elif event == "retiring_restore_skip":
            skips[int(fields["b"], 0)] += 1
    for identity in collected:
        if teardown[identity] != 1:
            errors.append(f"collected identity {identity} has {teardown[identity]} base teardowns")
    return {
        "path": str(path), "completed": completed,
        "complete_trace_prefix": not truncated and first_sequence == 1,
        "sequence_gaps": gaps, "events": dict(counts),
        "unique_held": len(ever_held), "populated_holds": populated_holds,
        "restored_from_hold": restored_held, "unique_collected": len(collected),
        "held_by_kind": dict(held_by_kind), "collected_by_kind": dict(collected_by_kind),
        "active_restores": active_restores, "active_resurrections": active_resurrections,
        "remaining_held": sorted(held), "peak_held": peak,
        "restore_skip_reasons": dict(skips), "last_stats": stats,
        "accounting_errors": errors,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps([summarize(path) for path in args.logs], indent=2)
    if args.output:
        args.output.write_text(text + "\n")
    print(text)
