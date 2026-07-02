#!/usr/bin/env python3
"""
compare_nplive.py — diff two NDJSON rollback logs and report where/why they diverge.

Usage:
    python3 compare_nplive.py fileA.ndjson fileB.ndjson [--pos-eps 0.001] [--context 3]

Designed for the UMVC3Hook nplive/rdiag NDJSON schema:
    {"v":1,"ts":..., "frame":N, "side":"host"|"client"|"ground_truth"|"resim",
     "inputs":{"local":..,"usedRemote":..,"recvRemote":..},
     "chars":[{"team":0,"slot":0,"pos":[x,y,z],"act":"0x..","fl":"0x..",
               "mHeld":"0x..","fcnt":N,"rng":["0x..","0x..","0x..","0x.."]}, ...]}

What it does:
  1. Parses both files, picks one "primary" entry per frame per file (skipping
     resim/diag duplicate entries unless that's all there is).
  2. Walks frames in increasing order, comparing matching (team,slot) characters.
  3. Reports the FIRST frame where anything differs beyond tolerance, classifies
     *what kind* of divergence it is (RNG-only / position / action / flags /
     mHeld / fcnt), and prints N frames of context before it so you can see the
     input stream leading up to the split.
  4. Also separately flags the first frame where RNG diverges even if nothing
     else has yet — this is usually the earlier, "root cause" frame, since RNG
     misconsumption often doesn't show up in position/action until later frames
     consume the now-different stream.
  5. Prints a summary table of every frame that still differs after the first
     divergence, so you can see whether it's a one-frame blip (self-corrects)
     or compounds forever (real desync).
"""
import argparse
import json
import sys
from collections import OrderedDict

PRIMARY_SIDE_PRIORITY = ["host", "client", "ground_truth", "live"]
SECONDARY_SIDES = {"resim"}  # skip these when a primary entry exists for the frame


def load_ndjson(path):
    """Returns dict: frame -> entry (picks the best entry per frame)."""
    by_frame = OrderedDict()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"  [warn] {path}:{lineno}: failed to parse ({e}), skipping", file=sys.stderr)
                continue
            frame = entry.get("frame")
            side = entry.get("side")
            if frame is None or "chars" not in entry:
                continue
            existing = by_frame.get(frame)
            if existing is None:
                by_frame[frame] = entry
            else:
                # Prefer a primary-side entry over a secondary one if we see both.
                if existing.get("side") in SECONDARY_SIDES and side not in SECONDARY_SIDES:
                    by_frame[frame] = entry
    return by_frame


def chars_by_key(entry, by_mid=False):
    out = {}
    for c in entry.get("chars", []):
        if c.get("plr", -1) < 0:
            continue
        if by_mid:
            key = (c.get("team"), c.get("mid"))
        else:
            key = (c.get("team"), c.get("slot"))
        out[key] = c
    return out


def hex_eq(a, b):
    if a is None or b is None:
        return a == b
    try:
        return int(a, 16) == int(b, 16)
    except (TypeError, ValueError):
        return a == b


def rng_eq(a, b):
    if a is None or b is None:
        return a == b
    if len(a) != len(b):
        return False
    return all(hex_eq(x, y) for x, y in zip(a, b))


def pos_diff(a, b):
    if a is None or b is None:
        return None
    return [abs(a[i] - b[i]) for i in range(min(len(a), len(b)))]


def is_uninit(v):
    return v == [0.0, 0.0, 0.0]


def detect_mid_swap(ca, cb):
    """Detect mids swapped between slots on the same team.
    Returns dict (team,slot) -> (swapped_to_slot, mid)."""
    from collections import defaultdict
    teams_a, teams_b = defaultdict(dict), defaultdict(dict)
    for (team, slot), c in ca.items():
        teams_a[team][slot] = c.get("mid")
    for (team, slot), c in cb.items():
        teams_b[team][slot] = c.get("mid")
    swaps = {}
    for team in set(teams_a) & set(teams_b):
        da, db = teams_a[team], teams_b[team]
        if set(da.values()) != set(db.values()):
            continue
        by_mid_a = {v: k for k, v in da.items()}
        by_mid_b = {v: k for k, v in db.items()}
        for mid in by_mid_a:
            sa, sb = by_mid_a[mid], by_mid_b[mid]
            if sa != sb:
                swaps[(team, sa)] = (sb, mid)
    return swaps


def diff_char(ca, cb, pos_eps):
    """Returns dict of field -> description, empty if no diff beyond tolerance."""
    diffs = {}
    pa, pb = ca.get("pos"), cb.get("pos")
    pd = pos_diff(pa, pb)
    if pd is not None and max(pd, default=0) > pos_eps:
        if is_uninit(pa) != is_uninit(pb):
            diffs["pos"] = f"A={pa} B={pb} delta={pd}  (uninit skip — one side not yet initialized)"
        else:
            diffs["pos"] = f"A={pa} B={pb} delta={pd}"
    if not hex_eq(ca.get("act"), cb.get("act")):
        diffs["act"] = f"A={ca.get('act')} B={cb.get('act')}"
    if not hex_eq(ca.get("fl"), cb.get("fl")):
        diffs["fl"] = f"A={ca.get('fl')} B={cb.get('fl')}"
    if not hex_eq(ca.get("mHeld"), cb.get("mHeld")):
        diffs["mHeld"] = f"A={ca.get('mHeld')} B={cb.get('mHeld')}"
    if ca.get("fcnt") != cb.get("fcnt"):
        diffs["fcnt"] = f"A={ca.get('fcnt')} B={cb.get('fcnt')}"
    if ca.get("ast") != cb.get("ast"):
        diffs["ast"] = f"A={ca.get('ast')} B={cb.get('ast')}"
    if ca.get("mid") != cb.get("mid"):
        diffs["mid"] = f"A={ca.get('mid')} B={cb.get('mid')}"
    if not rng_eq(ca.get("rng"), cb.get("rng")):
        diffs["rng"] = f"A={ca.get('rng')} B={cb.get('rng')}"
    return diffs


def fmt_entry_chars(entry):
    if entry is None:
        return "  (missing)"
    lines = []
    for c in entry.get("chars", []):
        lines.append(
            f"    team={c.get('team')} slot={c.get('slot')} mid={c.get('mid', '?')} plr={c.get('plr')} ast={c.get('ast')} "
            f"pos={c.get('pos')} act={c.get('act')} fl={c.get('fl')} mHeld={c.get('mHeld')} "
            f"fcnt={c.get('fcnt')} rng={c.get('rng')}"
        )
    return "\n".join(lines) if lines else "  (no chars)"


def classify(diffs):
    """Best-effort interpretation of what a divergence pattern means."""
    keys = set(diffs.keys())
    if keys == {"rng"}:
        return ("RNG-only divergence — random stream has split but it hasn't propagated "
                "into position/action/flags YET. This is very likely the true root-cause "
                "frame; whatever consumed RNG differently here (hit roll, damage variance, "
                "AI decision, particle/projectile pattern) is the bug. Position/action "
                "divergence a few frames later is just the downstream symptom.")
    if "rng" in keys and ("pos" in keys or "act" in keys):
        return ("RNG AND state diverge together — either this is several frames after the "
                "true RNG-only split (check earlier frames), or the RNG-consuming call "
                "itself directly altered position/action this same frame (e.g. a randomized "
                "hit reaction or knockback roll).")
    if keys == {"pos"}:
        return ("Position-only divergence with RNG/action/flags still matching — looks like "
                "a physics/velocity integration difference (e.g. an unrestored physics "
                "sub-state, or input applied on a different frame than expected) rather than "
                "a logic/RNG branch difference.")
    if "act" in keys or "fl" in keys:
        return ("Action/flag divergence — the two simulations took a different state-machine "
                "branch this frame. Check the input fields (local/usedRemote/recvRemote) for "
                "this and the prior 1-2 frames: a differing/late input is the most common "
                "cause of an act/fl split.")
    if "mHeld" in keys:
        return ("mHeld (translated input) differs — the two sides disagree about what input "
                "was actually applied this frame, independent of any RNG/physics issue. "
                "Check delay-buffer / remote-input resolution timing.")
    if "fcnt" in keys:
        return ("Frame counter differs — one side's character is running on a different "
                "internal tick count than the other, which usually means a desync in how "
                "many times this character's update actually ran (e.g. a skipped or "
                "double-counted frame somewhere in the resim loop).")
    if "__slot_swap__" in keys:
        return ("SLOT SWAP — characters are in different slot indices on each side. "
                "The same set of character mids exists on both sides but assigned to "
                "different slots. This is a team-ordering / slot-assignment bug at "
                "round start, not a rollback-state desync.")
    if "mid" in keys:
        return ("mid (character model ID) differs — the two sides have a different "
                "character in this slot. This indicates the game assigned characters "
                "to slots differently between host and client (e.g. team ordering mismatch "
                "or assist/snapback slot confusion).")
    if "ast" in keys:
        return ("ast (action sub-timer/assist state) differs — a frame-precision timing "
                "or assist-related state has drifted between the two simulations.")
    return "Mixed divergence — see individual fields above."


def latest_nplive_paths():
    """Return (host_path, client_path) for the latest nplive_* pair in cwd."""
    import glob, os, re
    dirs = glob.glob("nplive_*_host")
    if not dirs:
        return None, None
    ts_pat = re.compile(r"nplive_(\d{8}_\d{6})_host")
    latest = max((ts_pat.match(d) for d in dirs if ts_pat.match(d)),
                 key=lambda m: m.group(1), default=None)
    if latest is None:
        return None, None
    ts = latest.group(1)
    host = os.path.join(f"nplive_{ts}_host", "frames.ndjson")
    client = os.path.join(f"nplive_{ts}_client", "frames.ndjson")
    return host, client


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file_a", nargs="?", default=None, help="First ndjson log (e.g. host's nplive)")
    ap.add_argument("file_b", nargs="?", default=None, help="Second ndjson log (e.g. client's nplive)")
    ap.add_argument("--by-slot", action="store_true", help="Compare using (team,slot) key instead of (team,mid) — exposes slot assignment differences")
    ap.add_argument("--pos-eps", type=float, default=0.001, help="Position tolerance (default 0.001)")
    ap.add_argument("--context", type=int, default=3, help="Frames of context to print before first divergence (default 3)")
    ap.add_argument("--max-report", type=int, default=50, help="Max diverging frames to list in the summary table (default 50)")
    args = ap.parse_args()

    if args.file_a is None or args.file_b is None:
        host_path, client_path = latest_nplive_paths()
        if host_path is None or client_path is None:
            print("error: no nplive_*_host/_client directories found in cwd and no file arguments given", file=sys.stderr)
            sys.exit(1)
        args.file_a, args.file_b = host_path, client_path
        print(f"Auto-detected latest nplive pair:\n  host   = {args.file_a}\n  client = {args.file_b}\n")

    print(f"Loading A: {args.file_a}")
    a = load_ndjson(args.file_a)
    print(f"Loading B: {args.file_b}")
    b = load_ndjson(args.file_b)

    frames_a = set(a.keys())
    frames_b = set(b.keys())
    common = sorted(frames_a & frames_b)
    only_a = sorted(frames_a - frames_b)
    only_b = sorted(frames_b - frames_a)

    print(f"\nFrames in A: {len(frames_a)}  |  Frames in B: {len(frames_b)}  |  Common: {len(common)}")
    if only_a:
        print(f"  Frames only in A ({len(only_a)}): {only_a[:10]}{' ...' if len(only_a) > 10 else ''}")
    if only_b:
        print(f"  Frames only in B ({len(only_b)}): {only_b[:10]}{' ...' if len(only_b) > 10 else ''}")
    if not common:
        print("\nNo overlapping frames between the two files — nothing to compare.")
        return

    by_mid = not args.by_slot

    # Per-frame: dict of key -> diffs
    frame_diffs = OrderedDict()
    first_any_divergence = None
    first_rng_only_divergence = None
    first_ca = first_cb = None

    for frame in common:
        ea, eb = a[frame], b[frame]
        ca, cb = chars_by_key(ea, by_mid=by_mid), chars_by_key(eb, by_mid=by_mid)
        keys = sorted(set(ca.keys()) | set(cb.keys()))
        this_frame_diffs = {}
        for key in keys:
            if key not in ca or key not in cb:
                this_frame_diffs[key] = {"_missing": f"present in {'A' if key in ca else 'B'} only"}
                continue
            d = diff_char(ca[key], cb[key], args.pos_eps)
            if d:
                this_frame_diffs[key] = d

        # Annotate mid diffs that are actually slot swaps (only when comparing by slot)
        if args.by_slot:
            swaps = detect_mid_swap(ca, cb)
            if swaps:
                for key, fields in list(this_frame_diffs.items()):
                    if key in swaps and "mid" in fields:
                        sb, mid = swaps[key]
                        fields["mid"] += f" (SLOT SWAP: both sides have mid={mid} but in slot={key[1]} on A vs slot={sb} on B)"
                this_frame_diffs["__slot_swap__"] = {"__slot_swap__": "Character slot assignment differs between A and B"}

        if this_frame_diffs:
            # Skip frames where the only diffs are uninit positions
            only_uninit_pos = True
            for fields in this_frame_diffs.values():
                for k, v in fields.items():
                    if k != "pos" or "uninit" not in v:
                        only_uninit_pos = False
                        break
            if only_uninit_pos:
                continue
            frame_diffs[frame] = this_frame_diffs
            if first_any_divergence is None:
                first_any_divergence = frame
                first_ca, first_cb = ca, cb
            if first_rng_only_divergence is None:
                if all(set(d.keys()) == {"rng"} for d in this_frame_diffs.values()):
                    first_rng_only_divergence = frame

    if not frame_diffs:
        print("\nNo divergence found — A and B agree on every common frame (within tolerance).")
        return

    print(f"\n{'='*70}")
    print(f"FIRST DIVERGENCE: frame {first_any_divergence}")
    print(f"{'='*70}")

    idx = common.index(first_any_divergence)
    ctx_frames = common[max(0, idx - args.context):idx]
    if ctx_frames:
        print(f"\n--- {args.context} frame(s) of context before divergence ---")
        for cf in ctx_frames:
            ia, ib = a[cf].get("inputs", {}), b[cf].get("inputs", {})
            print(f"frame {cf}:  A.inputs={ia}  B.inputs={ib}")

    print(f"\n--- Divergence detail at frame {first_any_divergence} ---")
    ia, ib = a[first_any_divergence].get("inputs", {}), b[first_any_divergence].get("inputs", {})
    print(f"A.inputs={ia}")
    print(f"B.inputs={ib}")

    if "__slot_swap__" in frame_diffs[first_any_divergence]:
        print(f"\n  *** SLOT SWAP: characters are in different slot indices on each side. ***\n"
              f"       The same set of character mids exists on both sides but assigned to\n"
              f"       different slots. This is a team-ordering / slot-assignment bug at\n"
              f"       round start, not a rollback-state desync.")

    for key, fields in frame_diffs[first_any_divergence].items():
        if not isinstance(key, tuple):
            continue
        if by_mid:
            team, mid = key
            slot_a = first_ca.get(key, {}).get("slot", "?")
            slot_b = first_cb.get(key, {}).get("slot", "?")
            print(f"\n  [team={team} mid={mid} slot=({slot_a} vs {slot_b})]")
        else:
            team, slotv = key
            print(f"\n  [team={team} slot={slotv}]")
        for fname, desc in fields.items():
            print(f"    {fname}: {desc}")
        print(f"\n    Likely cause: {classify(fields)}")

    if first_rng_only_divergence is not None and first_rng_only_divergence != first_any_divergence:
        print(f"\n{'='*70}")
        print(f"NOTE: an RNG-only divergence was found EARLIER, at frame {first_rng_only_divergence}")
        print(f"      (before the frame {first_any_divergence} that has visible pos/act/flag")
        print(f"      differences). This is almost certainly the true root-cause frame —")
        print(f"      start your investigation there, not at frame {first_any_divergence}.")
        print(f"{'='*70}")

    # Summary of subsequent diverging frames
    print(f"\n{'='*70}")
    print(f"SUMMARY: {len(frame_diffs)} of {len(common)} common frames diverge")
    print(f"{'='*70}")
    shown = 0
    for frame, fields in frame_diffs.items():
        if shown >= args.max_report:
            print(f"  ... ({len(frame_diffs) - shown} more, use --max-report to see more)")
            break
        raw_fields = set()
        for d in fields.values():
            if isinstance(d, dict):
                for fname in d.keys():
                    if fname != "__slot_swap__":
                        raw_fields.add(fname)
        has_swap = any(k == "__slot_swap__" for k in fields)
        field_names = sorted(raw_fields)
        if has_swap:
            field_names.insert(0, "SLOT_SWAP")
        print(f"  frame {frame}: diverging fields = {field_names}")
        shown += 1

    # Does it self-correct or compound?
    diverging_frame_list = list(frame_diffs.keys())
    tail_window = diverging_frame_list[-min(10, len(diverging_frame_list)):]
    contiguous_tail = all(
        tail_window[i + 1] - tail_window[i] <= 3 for i in range(len(tail_window) - 1)
    ) if len(tail_window) > 1 else True
    print(f"\n{'='*70}")
    if len(frame_diffs) == 1:
        print("Pattern: single isolated frame diverged and the rest agree — likely a transient")
        print("blip (e.g. a one-frame logging race) rather than a persistent desync.")
    elif contiguous_tail and tail_window[-1] == common[-1]:
        print("Pattern: divergence is ONGOING through the end of the log (still diverging on the")
        print("last common frame) — this is a real, compounding desync, not a self-correction.")
    else:
        print("Pattern: divergence appears bounded/intermittent — check whether a rollback+resim")
        print("event corrected it, or whether it's recurring periodically.")


if __name__ == "__main__":
    main()
