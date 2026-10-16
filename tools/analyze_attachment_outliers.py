"""Summarize attachment outliers and their enclosing rollback records."""
import argparse
import json
import statistics
from pathlib import Path


NAMES = {"kirigakure_effect_update", "game_effect_attachment_update"}


def analyze(path, limit):
    records = json.loads(path.read_text())
    samples = [(i, record, hook) for i, record in enumerate(records)
               for hook in record.get("hooks", []) if hook["name"] in NAMES]
    print(f"\n{path}")
    if not samples:
        print("No attachment samples")
        return
    calls = sum(hook["calls"] for _, _, hook in samples)
    total = sum(hook["total_ms"] for _, _, hook in samples)
    print(f"windows={len(samples)} calls={calls} total_ms={total:.6f} "
          f"weighted_avg_us={total * 1000 / calls:.6f}")
    maxima = sorted(hook["max_us"] for _, _, hook in samples)
    print(f"window_max_us median={statistics.median(maxima):.3f} "
          f"largest={maxima[-limit:]}")
    for index, record, hook in sorted(samples, key=lambda x: x[2]["max_us"], reverse=True)[:limit]:
        print(json.dumps({"index": index,
                          "start_frame": record.get("start_frame"),
                          "end_frame": record.get("end_frame"),
                          "total_ms": record.get("total_ms"),
                          "attachment": hook,
                          "headless_tick": record.get("headless_tick"),
                          "top_hooks": sorted(record.get("hooks", []),
                                              key=lambda h: h["max_us"], reverse=True)[:8],
                          "top_objects": sorted(record.get("unit_objects", []),
                                                key=lambda o: o["max_us"], reverse=True)[:5]}, indent=2))
        for neighbor in records[max(0, index - 2):index + 3]:
            matches = [h for h in neighbor.get("hooks", []) if h["name"] in NAMES]
            if matches:
                print("neighbor", neighbor.get("start_frame"), neighbor.get("end_frame"), matches)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profiles", nargs="+", type=Path)
    parser.add_argument("--limit", type=int, default=3)
    args = parser.parse_args()
    if args.limit < 1:
        parser.error("--limit must be positive")
    for profile in args.profiles:
        analyze(profile, args.limit)
