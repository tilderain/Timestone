#!/usr/bin/env python3
"""compare2.py — dump and diff mHeld for every frame between two NDJSON logs."""
import json, sys
from collections import OrderedDict

def load_ndjson(path):
    by_frame = OrderedDict()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line: continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            frame = entry.get("frame")
            side = entry.get("side")
            if frame is None or "chars" not in entry:
                continue
            existing = by_frame.get(frame)
            if existing is None:
                by_frame[frame] = entry
            else:
                if existing.get("side") in ("resim",) and side not in ("resim",):
                    by_frame[frame] = entry
    return by_frame

def chars_by_key(entry):
    return {(c.get("team"), c.get("slot")): c for c in entry.get("chars", []) if c.get("plr", -1) >= 0}

def latest_nplive_paths():
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
    if len(sys.argv) >= 3:
        path_a, path_b = sys.argv[1], sys.argv[2]
    else:
        host_path, client_path = latest_nplive_paths()
        if host_path is None or client_path is None:
            print("error: no nplive_*_host/_client dirs in cwd and no file args given", file=sys.stderr)
            sys.exit(1)
        path_a, path_b = host_path, client_path
        print(f"Auto-detected latest nplive pair:\n  host   = {path_a}\n  client = {path_b}\n")

    a = load_ndjson(path_a)
    b = load_ndjson(path_b)
    common = sorted(set(a.keys()) & set(b.keys()))

    print(f"{'frame':>6} | {'team':>4} {'mid':>4} | {'A.mHeld':>12} {'B.mHeld':>12} | {'A.local':>12} {'A.usedR':>12} {'A.recvR':>12} {'B.local':>12} {'B.usedR':>12} {'B.recvR':>12} | match | notes")
    print("-" * 120)

    for frame in common:
        ca = chars_by_key(a[frame])
        cb = chars_by_key(b[frame])
        ia = a[frame].get("inputs", {})
        ib = b[frame].get("inputs", {})
        keys = sorted(set(ca.keys()) | set(cb.keys()))
        for key in keys:
            team, mid = key
            va = ca.get(key, {}).get("mHeld", "N/A")
            vb = cb.get(key, {}).get("mHeld", "N/A")
            match = "OK" if va == vb else "DIFF"
            notes = ""

            def hexval(v):
                try: return int(v, 16)
                except: return None

            ha, hb = hexval(va), hexval(vb)
            if ha is not None and hb is not None:
                delta = ha ^ hb
                if delta:
                    notes = f"xor=0x{delta:08X}"
                    if delta == ha:
                        notes += f" (A.z B.nz)"
                    elif delta == hb:
                        notes += f" (A.z B.nz)"

            show = False
            if match == "DIFF":
                show = True
            elif frame >= 68 and frame <= 75:
                show = True
            elif frame % 200 == 0:
                show = True

            if show:
                al = ia.get("local", "-")
                au = ia.get("usedRemote", "-")
                ar = ia.get("recvRemote", "-")
                bl = ib.get("local", "-")
                bu = ib.get("usedRemote", "-")
                br = ib.get("recvRemote", "-")
                print(f"{frame:>6} | {team:>4} {mid:>4} | {va:>12} {vb:>12} | {al:>12} {au:>12} {ar:>12} {bl:>12} {bu:>12} {br:>12} | {match:>5} | {notes}")

if __name__ == "__main__":
    main()
