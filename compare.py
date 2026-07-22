#!/usr/bin/env python3
"""
compare_nplive.py — diff two NDJSON rollback logs and report where/why they diverge.

Usage:
    python3 compare_nplive.py fileA.ndjson fileB.ndjson [--pos-eps 0.001] [--context 3] [--post-context 5]

Designed for the UMVC3Hook nplive/rdiag NDJSON schema:
    {"v":1,"ts":..., "frame":N, "side":"host"|"client"|"ground_truth"|"resim"|"host_pre_restore_prediction",
      "chars":[{"team":0,"slot":0,"pos":[x,y,z],"act":"0x..","fl":"0x..",
                "mHeld":"0x..","fcnt":N,
                "mOld":"0x..","mPressed":"0x..","mReleased":"0x..","mMask":"0x..","mProcessed":"0x..",
                "dirSlot":N,"dir0":"0x..","dir4":"0x..","h590":"0x..","h594":"0x..",
                 "rng":["0x..","0x..","0x..","0x.."],
                 "hitboxes":[{"bank":"capsule","index":0,"shape":"capsule",
                               "pos":[x,y,z],"size":[x,y,z]}], ...],
       "shots":[{"list":0,"addr":"0x..","slot":0,"init":0,
                   "owner":"0x..","element":"0x..","state":"0x..",
                    "pos":[x,y,z],"runtimePos":[x,y,z],
                    "collisionMatrix":[m00,m01,m02,m03,m10,m11,m12,m13,
                                         m20,m21,m22,m23,m30,m31,m32,m33],
                    "lifeFrame":0.0,"velocity":[x,y,z],"speed":0.0,
                    "step":0.0,"timer":0.0,
                    "hitboxes":[{"bank":"shot","index":0,"slot":0,
                                  "shape":"capsule","pos":[x,y,z],
                                  "endPos":[x,y,z],"size":[x,y,z],
                                  "radius":0.0}]}, ...]}

What it does:
  1. Parses both files, picks one "primary" entry per frame per file (skipping
     resim/diag duplicate entries unless that's all there is).
   2. Walks frames in increasing order, comparing matching (team,slot) characters
      and matching shots by list/order. Shot addresses and owner/element pointers
      are process-local metadata and are intentionally not treated as gameplay
      divergence.
   3. Reports the FIRST frame where anything differs beyond tolerance, classifies
      *what kind* of divergence it is (RNG-only / position / action / flags /
      mHeld / fcnt / dirSlot / dir0 / dir4 / h590 / h594 / dirRing), and prints N frames of context before and after it
     so you can see the input stream and how the split propagates.
  4. Also separately flags the first frame where RNG diverges even if nothing
     else has yet — this is usually the earlier, "root cause" frame, since RNG
     misconsumption often doesn't show up in position/action until later frames
     consume the now-different stream. Similarly flags dirRing-only divergences
     which indicate lost/delayed directional input delivery.
  5. Prints a summary table of every frame that still differs after the first
     divergence, so you can see whether it's a one-frame blip (self-corrects)
     or compounds forever (real desync).
"""
import argparse
import json
import sys
from collections import OrderedDict

PRIMARY_SIDE_PRIORITY = ["host", "client", "ground_truth", "live"]
SECONDARY_SIDES = {"resim", "pre_restore_prediction"}


def is_secondary_side(side):
    """Return true for diagnostic records, not primary timeline entries."""
    return (side in SECONDARY_SIDES or
            isinstance(side, str) and
            (side.endswith("_resim") or side.endswith("_pre_restore_prediction")))


def side_priority(side):
    """Prefer canonical live records, then corrected resim, then predictions."""
    if not is_secondary_side(side):
        return 2
    if isinstance(side, str) and (side == "resim" or side.endswith("_resim")):
        return 1
    return 0


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
                # A frame can contain both the intentionally stale pre-restore
                # prediction and its corrected resim result. Prefer the latter
                # when no canonical live record is present.
                if side_priority(side) > side_priority(existing.get("side")):
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


def shots_by_key(entry):
    """Match shots by stable list/order identity, not process-local addresses."""
    out = {}
    per_list = {}
    for shot in entry.get("shots", []):
        shot_list = shot.get("list", 0)
        ordinal = per_list.get(shot_list, 0)
        per_list[shot_list] = ordinal + 1
        out[(shot_list, ordinal)] = shot
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


def _normalize_dirRing(dr):
    if dr == "all zeros":
        return ["0"] * 64
    return dr

def dirRing_eq(a, b):
    a = _normalize_dirRing(a)
    b = _normalize_dirRing(b)
    if a is None or b is None:
        return a == b
    if len(a) != len(b):
        return False
    return all(hex_eq(x, y) for x, y in zip(a, b))


def pos_diff(a, b):
    if a is None or b is None:
        return None
    return [abs(a[i] - b[i]) for i in range(min(len(a), len(b)))]


def hitbox_key(hitbox):
    return (hitbox.get("bank"), hitbox.get("slot"),
            hitbox.get("index"), hitbox.get("shape"))


def hitbox_diff(a, b, eps):
    """Return readable differences for hitboxes keyed by bank/index/shape."""
    if a is None or b is None:
        return None if a == b else f"A={a} B={b}"

    map_a = {hitbox_key(h): h for h in a}
    map_b = {hitbox_key(h): h for h in b}
    differences = []
    for key in sorted(set(map_a) | set(map_b), key=str):
        if key not in map_a:
            differences.append(f"{key}: present in B only")
            continue
        if key not in map_b:
            differences.append(f"{key}: present in A only")
            continue

        ha, hb = map_a[key], map_b[key]
        if ha.get("damaging") != hb.get("damaging"):
            differences.append(
                f"{key} damaging: A={ha.get('damaging')} B={hb.get('damaging')}"
            )
        for field in ("pos", "endPos", "size"):
            va, vb = ha.get(field), hb.get(field)
            delta = pos_diff(va, vb)
            if delta is None:
                if va != vb:
                    differences.append(f"{key} {field}: A={va} B={vb}")
            elif len(va) != len(vb) or max(delta, default=0) > eps:
                differences.append(f"{key} {field}: A={va} B={vb} delta={delta}")
        for field in ("radius", "length"):
            va, vb = ha.get(field), hb.get(field)
            if va is None or vb is None:
                if va != vb:
                    differences.append(f"{key} {field}: A={va} B={vb}")
            elif abs(va - vb) > eps:
                differences.append(f"{key} {field}: A={va} B={vb} delta={abs(va - vb)}")

    return "; ".join(differences) if differences else None


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
    if not hex_eq(ca.get("fl4"), cb.get("fl4")):
        diffs["fl4"] = f"A={ca.get('fl4')} B={cb.get('fl4')}"
    if not hex_eq(ca.get("fl8"), cb.get("fl8")):
        diffs["fl8"] = f"A={ca.get('fl8')} B={cb.get('fl8')}"
    if not hex_eq(ca.get("mHeld"), cb.get("mHeld")):
        diffs["mHeld"] = f"A={ca.get('mHeld')} B={cb.get('mHeld')}"
    if not hex_eq(ca.get("mOld"), cb.get("mOld")):
        diffs["mOld"] = f"A={ca.get('mOld')} B={cb.get('mOld')}"
    if not hex_eq(ca.get("mPressed"), cb.get("mPressed")):
        diffs["mPressed"] = f"A={ca.get('mPressed')} B={cb.get('mPressed')}"
    if not hex_eq(ca.get("mReleased"), cb.get("mReleased")):
        diffs["mReleased"] = f"A={ca.get('mReleased')} B={cb.get('mReleased')}"
    if not hex_eq(ca.get("mMask"), cb.get("mMask")):
        diffs["mMask"] = f"A={ca.get('mMask')} B={cb.get('mMask')}"
    if not hex_eq(ca.get("mProcessed"), cb.get("mProcessed")):
        diffs["mProcessed"] = f"A={ca.get('mProcessed')} B={cb.get('mProcessed')}"
    if ca.get("fcnt") != cb.get("fcnt"):
        diffs["fcnt"] = f"A={ca.get('fcnt')} B={cb.get('fcnt')}"
    if ca.get("ast") != cb.get("ast"):
        diffs["ast"] = f"A={ca.get('ast')} B={cb.get('ast')}"
    if ca.get("mid") != cb.get("mid"):
        diffs["mid"] = f"A={ca.get('mid')} B={cb.get('mid')}"
    if not rng_eq(ca.get("rng"), cb.get("rng")):
        diffs["rng"] = f"A={ca.get('rng')} B={cb.get('rng')}"
    if not dirRing_eq(ca.get("dirRing"), cb.get("dirRing")):
        diffs["dirRing"] = f"A={ca.get('dirRing')} B={cb.get('dirRing')}"
    if ca.get("dirSlot") != cb.get("dirSlot"):
        diffs["dirSlot"] = f"A={ca.get('dirSlot')} B={cb.get('dirSlot')}"
    if not hex_eq(ca.get("dir0"), cb.get("dir0")):
        diffs["dir0"] = f"A={ca.get('dir0')} B={cb.get('dir0')}"
    if not hex_eq(ca.get("dir4"), cb.get("dir4")):
        diffs["dir4"] = f"A={ca.get('dir4')} B={cb.get('dir4')}"
    if not hex_eq(ca.get("h590"), cb.get("h590")):
        diffs["h590"] = f"A={ca.get('h590')} B={cb.get('h590')}"
    if not hex_eq(ca.get("h594"), cb.get("h594")):
        diffs["h594"] = f"A={ca.get('h594')} B={cb.get('h594')}"
    if ca.get("acc260") != cb.get("acc260"):
        diffs["acc260"] = f"A={ca.get('acc260')} B={cb.get('acc260')}"
    if ca.get("tap2e0") != cb.get("tap2e0"):
        diffs["tap2e0"] = f"A={ca.get('tap2e0')} B={cb.get('tap2e0')}"
    if ca.get("acc130") != cb.get("acc130"):
        diffs["acc130"] = f"A={ca.get('acc130')} B={cb.get('acc130')}"
    if not hex_eq(ca.get("m12c"), cb.get("m12c")):
        diffs["m12c"] = f"A={ca.get('m12c')} B={cb.get('m12c')}"
    if not hex_eq(ca.get("m230"), cb.get("m230")):
        diffs["m230"] = f"A={ca.get('m230')} B={cb.get('m230')}"
    if not hex_eq(ca.get("m238"), cb.get("m238")):
        diffs["m238"] = f"A={ca.get('m238')} B={cb.get('m238')}"
    if not hex_eq(ca.get("m250"), cb.get("m250")):
        diffs["m250"] = f"A={ca.get('m250')} B={cb.get('m250')}"
    hitboxes = hitbox_diff(ca.get("hitboxes"), cb.get("hitboxes"), pos_eps)
    if hitboxes:
        diffs["hitboxes"] = hitboxes
    return diffs


def diff_shot(sa, sb, pos_eps):
    diffs = {}
    if sa.get("slot") != sb.get("slot"):
        diffs["slot"] = f"A={sa.get('slot')} B={sb.get('slot')}"
    if sa.get("kind") != sb.get("kind"):
        diffs["kind"] = f"A={sa.get('kind')} B={sb.get('kind')}"
    # addr, owner, element, and init are allocator/process-local values. They
    # cannot be compared between host and client logs reliably.
    if not hex_eq(sa.get("state"), sb.get("state")):
        diffs["state"] = f"A={sa.get('state')} B={sb.get('state')}"
    # runtimePos is the effective uShot movement position. Keep the legacy
    # root pos comparison too, since it can expose transform/root divergence.
    for field in ("pos", "runtimePos", "collisionOrigin", "velocity"):
        va, vb = sa.get(field), sb.get(field)
        pd = pos_diff(va, vb)
        if pd is None:
            if va != vb:
                diffs[field] = f"A={va} B={vb}"
        elif len(pd) != len(va) or max(pd, default=0) > pos_eps:
            diffs[field] = f"A={va} B={vb} delta={pd}"
    for field in ("lifeFrame", "speed", "step", "timer"):
        va, vb = sa.get(field), sb.get(field)
        if va is None or vb is None:
            if va != vb:
                diffs[field] = f"A={va} B={vb}"
        elif abs(va - vb) > pos_eps:
            diffs[field] = f"A={va} B={vb} delta={abs(va - vb)}"
    hitboxes = hitbox_diff(sa.get("hitboxes"), sb.get("hitboxes"), pos_eps)
    if hitboxes:
        diffs["hitboxes"] = hitboxes
    return diffs


def fmt_entry_shots(entry):
    if entry is None:
        return "  (missing)"
    shots = entry.get("shots", [])
    if not shots:
        return "  (no shots)"
    return "\n".join(
        f"    list={s.get('list')} slot={s.get('slot')} kind={s.get('kind', '?')} "
        f"state={s.get('state')} pos={s.get('pos')} runtimePos={s.get('runtimePos')} "
        f"lifeFrame={s.get('lifeFrame')} velocity={s.get('velocity')} "
        f"speed={s.get('speed')} step={s.get('step')} timer={s.get('timer')}"
        for s in shots
    )


def fmt_entry_chars(entry, include_dir=False):
    if entry is None:
        return "  (missing)"
    lines = []
    for c in entry.get("chars", []):
        parts = (
            f"    team={c.get('team')} slot={c.get('slot')} mid={c.get('mid', '?')} plr={c.get('plr')} ast={c.get('ast')} "
            f"pos={c.get('pos')} act={c.get('act')} fl={c.get('fl')} fl4={c.get('fl4')} fl8={c.get('fl8')} mHeld={c.get('mHeld')} "
            f"mOld={c.get('mOld')} mPressed={c.get('mPressed')} mReleased={c.get('mReleased')} "
            f"mMask={c.get('mMask')} mProcessed={c.get('mProcessed')} "
            f"fcnt={c.get('fcnt')} rng={c.get('rng')}"
        )
        if include_dir:
            ds = c.get("dirSlot")
            d0 = c.get("dir0")
            d4 = c.get("dir4")
            if ds is not None:
                parts += f" dirSlot={ds} dir0={d0} dir4={d4}"
            dr = c.get("dirRing")
            if dr is not None:
                parts += f" dirRing[0..3]={dr[:4]}"
        lines.append(parts)
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
    if keys == {"hitboxes"}:
        return ("Hitbox-only divergence with character state still matching. Check restored "
                "cHitPrim geometry, active flags, parent transforms, and collision update order.")
    if "hitboxes" in keys:
        return ("Hitbox geometry diverges along with character state. The per-primitive details "
                "identify whether the split is position, size, activation, or shape membership.")
    if "fl8" in keys:
        return ("fl8 (uCharacter+0x14F8) differs — this word holds the face-left flag "
                "(bit 0x200000) read by cChrInput__swapLRIfFaceLeft to remap left/right "
                "input bits. If it diverges, identical mHeld produces OPPOSITE horizontal "
                "movement and different dash/walk decisions on each side. Check whether "
                "the snapshot restore covers +0x14F8 and what recomputes it post-restore.")
    if "act" in keys or "fl" in keys:
        return ("Action/flag divergence — the two simulations took a different state-machine "
                "branch this frame.")
    input_raw_keys = {"mHeld", "mOld", "mPressed", "mReleased", "mMask", "mProcessed"}
    if keys & input_raw_keys:
        return ("Raw input fields differ (mHeld/mOld/mPressed/mReleased/mMask/mProcessed) — "
                "the two sides disagree about what input was received or processed this frame. "
                "mOld=previous frame input, mPressed=rising edge, mReleased=falling edge, "
                "mMask=post-SOCD mask, mProcessed=final processed input. "
                "Check delay-buffer / remote-input resolution timing, SOCD cleaning, "
                "and input delivery order.")
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
    if "dirRing" in keys:
        return ("dirRing (direction input ring buffer) differs — the two sides disagree "
                "about the history of directional inputs received. This can cause motion "
                "input detection mismatches (e.g., one side sees a quarter-circle motion "
                "while the other does not), leading to different special moves or actions. "
                "Check remote input delivery and buffer timing.")
    dir_keys = {"dirSlot", "dir0", "dir4", "h590", "h594"}
    if keys & dir_keys:
        return ("Direction-related raw state differs — dirSlot/dir0/dir4/h590/h594 "
                "capture the direction input state. A divergence here "
                "means the input processing code saw a different direction value or "
                "slot this frame. Check remote input delivery timing, SOCD cleaning, "
                "and direction-ring buffer alignment.")
    dash_keys = {"acc260", "tap2e0", "acc130", "m12c", "m230", "m238", "m250"}
    if keys & dash_keys:
        return ("Dash/double-tap detection state differs — acc260/tap2e0/acc130 are "
                "per-bit elapsed-frame timers and tap counters; m12c/m230/m238/m250 are "
                "mask words for dash detection. A divergence here means the input "
                "processing code saw different tap timing or direction inputs, which can "
                "cause different dash/super-dash/wave-dash behavior. Check remote input "
                "delivery timing and direction-ring alignment.")
    return "Mixed divergence — see individual fields above."


def latest_nplive_paths():
    """Return (host_path, client_path) for the latest nplive_* pair in cwd.
    Host and client directories are found independently — they may have different timestamps."""
    import glob, os, re
    ts_pat = re.compile(r"nplive_(\d{8}_\d{6})_(host|client)$")
    host_dirs = [(m, os.path.join(d, "frames.ndjson"))
                 for d in glob.glob("nplive_*_host")
                 if (m := ts_pat.match(d))]
    client_dirs = [(m, os.path.join(d, "frames.ndjson"))
                   for d in glob.glob("nplive_*_client")
                   if (m := ts_pat.match(d))]
    if not host_dirs or not client_dirs:
        return None, None
    host = max(host_dirs, key=lambda p: p[0].group(1))[1]
    client = max(client_dirs, key=lambda p: p[0].group(1))[1]
    return host, client


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file_a", nargs="?", default=None, help="First ndjson log (e.g. host's nplive)")
    ap.add_argument("file_b", nargs="?", default=None, help="Second ndjson log (e.g. client's nplive)")
    ap.add_argument("--by-slot", action="store_true", help="Compare using (team,slot) key instead of (team,mid) — exposes slot assignment differences")
    ap.add_argument("--pos-eps", type=float, default=0.001, help="Position tolerance (default 0.001)")
    ap.add_argument("--context", type=int, default=3, help="Frames of context to print before first divergence (default 3)")
    ap.add_argument("--post-context", type=int, default=5, help="Frames of context to print after first divergence (default 5)")
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
    first_dirRing_only_divergence = None
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

        sa, sb = shots_by_key(ea), shots_by_key(eb)
        shot_diffs = {}
        for key in sorted(set(sa) | set(sb)):
            if key not in sa or key not in sb:
                shot_diffs[key] = {"_missing": f"shot present in {'A' if key in sa else 'B'} only"}
                continue
            d = diff_shot(sa[key], sb[key], args.pos_eps)
            if d:
                shot_diffs[key] = d
        if shot_diffs:
            this_frame_diffs["__shots__"] = shot_diffs

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
                if fields and all(k == "_missing" for k in fields):
                    only_uninit_pos = False
                    break
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
            if first_dirRing_only_divergence is None:
                dir_field_set = {"dirRing", "dirSlot", "dir0", "dir4", "h590", "h594"}
                if all(set(d.keys()) <= dir_field_set for d in this_frame_diffs.values()):
                    first_dirRing_only_divergence = frame

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
        if key == "__shots__":
            print("\n  [shots]")
            for shot_key, shot_fields in fields.items():
                print(f"    [list={shot_key[0]} ordinal={shot_key[1]}]")
                for fname, desc in shot_fields.items():
                    print(f"      {fname}: {desc}")
            print("\n    Likely cause: projectile lifecycle, position, kind, or shot-state divergence.")
            continue
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

    # Print details for frames following the first divergence
    post_frames = common[idx + 1: idx + 1 + args.post_context]
    if post_frames:
        print(f"\n--- Details of the next {len(post_frames)} frame(s) after first divergence ---")
        for pf in post_frames:
            ia, ib = a[pf].get("inputs", {}), b[pf].get("inputs", {})
            print(f"\nframe {pf}:  A.inputs={ia}  B.inputs={ib}")
            if pf in frame_diffs:
                ca_pf = chars_by_key(a[pf], by_mid=by_mid)
                cb_pf = chars_by_key(b[pf], by_mid=by_mid)
                for key, fields in frame_diffs[pf].items():
                    if key == "__shots__":
                        print("  [shots]")
                        for shot_key, shot_fields in fields.items():
                            print(f"    [list={shot_key[0]} ordinal={shot_key[1]}] {shot_fields}")
                        continue
                    if not isinstance(key, tuple):
                        continue
                    if by_mid:
                        team, mid = key
                        slot_a = ca_pf.get(key, {}).get("slot", "?")
                        slot_b = cb_pf.get(key, {}).get("slot", "?")
                        print(f"  [team={team} mid={mid} slot=({slot_a} vs {slot_b})]")
                    else:
                        team, slotv = key
                        print(f"  [team={team} slot={slotv}]")
                    for fname, desc in fields.items():
                        print(f"    {fname}: {desc}")
            else:
                print("  [No divergences on this frame]")

    if first_rng_only_divergence is not None and first_rng_only_divergence != first_any_divergence:
        print(f"\n{'='*70}")
        print(f"NOTE: an RNG-only divergence was found EARLIER, at frame {first_rng_only_divergence}")
        print(f"      (before the frame {first_any_divergence} that has visible pos/act/flag")
        print(f"      differences). This is almost certainly the true root-cause frame —")
        print(f"      start your investigation there, not at frame {first_any_divergence}.")
        print(f"{'='*70}")
    if first_dirRing_only_divergence is not None and first_dirRing_only_divergence != first_any_divergence:
        print(f"\n{'='*70}")
        print(f"NOTE: a dir-only divergence was found EARLIER, at frame {first_dirRing_only_divergence}")
        print(f"      (before the frame {first_any_divergence} that has visible pos/act/flag")
        print(f"      differences). The direction input state (dirRing/dirSlot/dir0/dir4/h590/h594)")
        print(f"      diverged first — this likely means a remote directional input was lost,")
        print(f"      delayed, or misdelivered, causing the motion input histories to differ.")
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
        for key, d in fields.items():
            if key == "__shots__":
                for shot_fields in d.values():
                    raw_fields.update(
                        "shots" if name == "_missing" else f"shot.{name}"
                        for name in shot_fields
                    )
                continue
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
