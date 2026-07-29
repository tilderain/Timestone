#!/usr/bin/env python3
"""Compare two UMVC3 rollback snapshot histories and emit an AI-readable report."""

import argparse
import bisect
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


MAGIC = 0x50445355
VERSIONS = {1, 2}
HEADER = struct.Struct("<IIIIiIQQQ41sB6s")
FRAME = struct.Struct("<i7I4QB7s")
REGION_V1 = struct.Struct("<5QB7s")
REGION_V2 = struct.Struct("<5QB7s64s")
ROLE_NAMES = {0: "offline", 1: "host", 2: "client"}


@dataclass
class Region:
    address: int
    size: int
    owner_slot: int
    expected_vtable: int
    stored_hash: int
    skip_vtable: bool
    data_offset: int = 0
    name: str = ""


@dataclass
class FrameRecord:
    frame: int
    local_input: int
    remote_input: int
    regions: list
    counts: dict
    snapshot_hash: int
    manifest_hash: int
    data: bytes


@dataclass
class Dump:
    path: str
    role: str
    trigger_frame: int
    image_base: int
    image_size: int
    git_commit: str
    git_dirty: bool
    frames: dict


class PointerResolver:
    def __init__(self, dump, frame):
        self.dump = dump
        self.frame = frame
        intervals = sorted(
            (region.address, region.address + region.size, index)
            for index, region in enumerate(frame.regions)
        )
        self.starts = [item[0] for item in intervals]
        self.intervals = intervals
        self.cache = {}

    def resolve(self, value):
        cached = self.cache.get(value)
        if cached is not None:
            return cached
        if value == 0:
            result = ("null", 0, 0)
        elif self.dump.image_base <= value < self.dump.image_base + self.dump.image_size:
            result = ("image_rva", value - self.dump.image_base, 0)
        else:
            position = bisect.bisect_right(self.starts, value) - 1
            if position >= 0:
                start, end, index = self.intervals[position]
                result = ("region", index, value - start) if value < end else None
            else:
                result = None
        if result is not None:
            self.cache[value] = result
        return result


def read_exact(file, size, context):
    data = file.read(size)
    if len(data) != size:
        raise ValueError(f"truncated {context}: wanted {size} bytes, got {len(data)}")
    return data


def decode_xor_rle(encoded, previous, output_size):
    if previous is None or len(previous) != output_size:
        raise ValueError("XOR-RLE frame has no same-sized predecessor")
    output = bytearray(previous)
    source = 0
    destination = 0
    while destination < output_size:
        tag = 0
        shift = 0
        while True:
            if source >= len(encoded) or shift > 35:
                raise ValueError("invalid XOR-RLE varint")
            byte = encoded[source]
            source += 1
            tag |= (byte & 0x7F) << shift
            if not byte & 0x80:
                break
            shift += 7
        run = tag >> 1
        if run == 0 or destination + run > output_size:
            raise ValueError("invalid XOR-RLE run length")
        if tag & 1:
            if source + run > len(encoded):
                raise ValueError("truncated XOR-RLE literal run")
            for index in range(run):
                output[destination + index] ^= encoded[source + index]
            source += run
        destination += run
    if source != len(encoded):
        raise ValueError("trailing XOR-RLE data")
    return bytes(output)


def load_dump(path):
    with open(path, "rb") as file:
        values = HEADER.unpack(read_exact(file, HEADER.size, "dump header"))
        (magic, version, header_size, frame_count, trigger_frame, role,
         image_base, image_size, _timestamp, commit, dirty, _reserved) = values
        if magic != MAGIC:
            raise ValueError(f"bad magic 0x{magic:08x}")
        if version not in VERSIONS:
            raise ValueError(f"unsupported dump version {version}")
        if header_size < HEADER.size:
            raise ValueError(f"invalid header size {header_size}")
        if header_size > HEADER.size:
            read_exact(file, header_size - HEADER.size, "extended header")

        frames = {}
        previous = None
        for frame_index in range(frame_count):
            values = FRAME.unpack(read_exact(file, FRAME.size, f"frame {frame_index} header"))
            (number, local_input, remote_input, region_count, shot_count,
             effect_count, clone_count, model_count, snapshot_hash,
             manifest_hash, raw_size, encoded_size, encoding, _reserved) = values
            regions = []
            offset = 0
            region_struct = REGION_V2 if version >= 2 else REGION_V1
            for region_index in range(region_count):
                raw = region_struct.unpack(read_exact(
                    file, region_struct.size, f"frame {number} region {region_index}"
                ))
                name = (raw[7].split(b"\0", 1)[0].decode("utf-8", "replace")
                        if version >= 2 else "")
                region = Region(
                    raw[0], raw[1], raw[2], raw[3], raw[4], bool(raw[5]), offset, name
                )
                offset += region.size
                regions.append(region)
            if offset != raw_size:
                raise ValueError(
                    f"frame {number}: region sizes total {offset}, snapshot says {raw_size}"
                )
            payload = read_exact(file, encoded_size, f"frame {number} payload")
            if encoding == 0:
                if encoded_size != raw_size:
                    raise ValueError(f"frame {number}: raw payload size mismatch")
                data = payload
            elif encoding == 1:
                data = decode_xor_rle(payload, previous, raw_size)
            else:
                raise ValueError(f"frame {number}: unknown encoding {encoding}")
            frames[number] = FrameRecord(
                number, local_input, remote_input, regions,
                {"shots": shot_count, "effects": effect_count,
                 "collision_clones": clone_count, "model_render_states": model_count},
                snapshot_hash, manifest_hash, data,
            )
            previous = data
        if file.read(1):
            raise ValueError("trailing bytes after final frame")
    return Dump(
        str(path), ROLE_NAMES.get(role, f"unknown({role})"), trigger_frame,
        image_base, image_size, commit.split(b"\0", 1)[0].decode("ascii", "replace"),
        bool(dirty), frames,
    )


def region_label(index, region, dump, resolver):
    if region.name:
        return region.name
    if dump.image_base <= region.address < dump.image_base + dump.image_size:
        rva = region.address - dump.image_base
        if rva == 0xD765D8:
            return "globalRng"
        return f"game_image+0x{rva:x}"
    owner = resolver.resolve(region.owner_slot)
    if owner and owner[0] == "region":
        return f"dynamic_owned_by_region[{owner[1]}]+0x{owner[2]:x}"
    if region.size == 0x20000:
        return "sAction.full"
    if region.size == 0xD80:
        return "sCamera"
    if region.size == 0x590:
        return "uActionSideCamera"
    if region.size == 0x300:
        return "sAction.compact"
    if region.size == 0x2C0:
        return "cChrTeam"
    if region.size == 0x6A60:
        return "uCharacter"
    return f"region[{index}]"


def differing_runs(data_a, data_b, ignored, limit):
    runs = []
    index = 0
    size = min(len(data_a), len(data_b))
    while index < size and len(runs) < limit:
        if data_a[index] == data_b[index] or index in ignored:
            index += 1
            continue
        start = index
        while (index < size and data_a[index] != data_b[index] and
               index not in ignored and index - start < 32):
            index += 1
        runs.append((start, index))
    return runs


def compare_region(index, region_a, region_b, dump_a, dump_b, frame_a, frame_b,
                   resolver_a, resolver_b, max_diffs):
    start_a, start_b = region_a.data_offset, region_b.data_offset
    data_a = frame_a.data[start_a:start_a + region_a.size]
    data_b = frame_b.data[start_b:start_b + region_b.size]
    ignored = set()
    relocations = []
    for offset in range(0, min(len(data_a), len(data_b)) - 7, 8):
        value_a = int.from_bytes(data_a[offset:offset + 8], "little")
        value_b = int.from_bytes(data_b[offset:offset + 8], "little")
        if value_a == value_b:
            continue
        pointer_a = resolver_a.resolve(value_a)
        pointer_b = resolver_b.resolve(value_b)
        if pointer_a is not None and pointer_a == pointer_b:
            ignored.update(range(offset, offset + 8))
            if len(relocations) < max_diffs:
                relocations.append({"offset": f"0x{offset:x}", "target": list(pointer_a)})

    runs = []
    for start, end in differing_runs(data_a, data_b, ignored, max_diffs):
        context_start = max(0, start - 8)
        context_end = min(len(data_a), end + 8)
        runs.append({
            "offset": f"0x{start:x}",
            "length": end - start,
            "a_hex": data_a[start:end].hex(),
            "b_hex": data_b[start:end].hex(),
            "context_offset": f"0x{context_start:x}",
            "a_context_hex": data_a[context_start:context_end].hex(),
            "b_context_hex": data_b[context_start:context_end].hex(),
        })
    raw_diff_bytes = sum(a != b for a, b in zip(data_a, data_b))
    meaningful_diff_bytes = sum(
        a != b and offset not in ignored
        for offset, (a, b) in enumerate(zip(data_a, data_b))
    )
    return {
        "region_index": index,
        "label": region_label(index, region_a, dump_a, resolver_a),
        "size": region_a.size,
        "raw_different_bytes": raw_diff_bytes,
        "relocation_only_bytes": raw_diff_bytes - meaningful_diff_bytes,
        "meaningful_different_bytes": meaningful_diff_bytes,
        "pointer_relocations_sample": relocations,
        "differences_sample": runs,
    }


def compare_frame(dump_a, dump_b, frame_a, frame_b, max_regions, max_diffs):
    resolver_a = PointerResolver(dump_a, frame_a)
    resolver_b = PointerResolver(dump_b, frame_b)
    result = {
        "frame": frame_a.frame,
        "inputs": {
            "a_local": f"0x{frame_a.local_input:08x}",
            "a_remote": f"0x{frame_a.remote_input:08x}",
            "b_local": f"0x{frame_b.local_input:08x}",
            "b_remote": f"0x{frame_b.remote_input:08x}",
            "cross_peer_consistent": (
                frame_a.local_input == frame_b.remote_input and
                frame_a.remote_input == frame_b.local_input
            ),
        },
        "structure": {
            "a_region_count": len(frame_a.regions),
            "b_region_count": len(frame_b.regions),
            "a_counts": frame_a.counts,
            "b_counts": frame_b.counts,
        },
        "regions": [],
    }
    region_count = min(len(frame_a.regions), len(frame_b.regions))
    for index in range(region_count):
        region_a, region_b = frame_a.regions[index], frame_b.regions[index]
        address_a = resolver_a.resolve(region_a.address)
        address_b = resolver_b.resolve(region_b.address)
        owner_a = resolver_a.resolve(region_a.owner_slot)
        owner_b = resolver_b.resolve(region_b.owner_slot)
        vtable_a = resolver_a.resolve(region_a.expected_vtable)
        vtable_b = resolver_b.resolve(region_b.expected_vtable)
        metadata_matches = (
            address_a == address_b and owner_a == owner_b and vtable_a == vtable_b
        )
        if (region_a.size != region_b.size or
                region_a.skip_vtable != region_b.skip_vtable or
                not metadata_matches):
            result["regions"].append({
                "region_index": index,
                "label": region_label(index, region_a, dump_a, resolver_a),
                "structural_mismatch": {
                    "a_size": region_a.size, "b_size": region_b.size,
                    "a_skip_vtable": region_a.skip_vtable,
                    "b_skip_vtable": region_b.skip_vtable,
                    "a_address_identity": address_a,
                    "b_address_identity": address_b,
                    "a_owner_identity": owner_a,
                    "b_owner_identity": owner_b,
                    "a_vtable_identity": vtable_a,
                    "b_vtable_identity": vtable_b,
                },
            })
        else:
            comparison = compare_region(
                index, region_a, region_b, dump_a, dump_b, frame_a, frame_b,
                resolver_a, resolver_b, max_diffs,
            )
            if comparison["meaningful_different_bytes"]:
                result["regions"].append(comparison)
        if len(result["regions"]) >= max_regions:
            break
    result["meaningful_different_bytes"] = sum(
        region.get("meaningful_different_bytes", 1) for region in result["regions"]
    )
    result["structure_matches"] = (
        len(frame_a.regions) == len(frame_b.regions) and
        frame_a.counts == frame_b.counts and
        all(
            a.size == b.size and a.skip_vtable == b.skip_vtable and
            resolver_a.resolve(a.address) == resolver_b.resolve(b.address) and
            resolver_a.resolve(a.owner_slot) == resolver_b.resolve(b.owner_slot) and
            resolver_a.resolve(a.expected_vtable) == resolver_b.resolve(b.expected_vtable)
            for a, b in zip(frame_a.regions, frame_b.regions)
        )
    )
    return result


def diagnose(first):
    if not first["inputs"]["cross_peer_consistent"]:
        return (
            "The peers disagree on the inputs assigned to this snapshot frame. "
            "Investigate input scheduling, packet correction, or frame labeling before state restore."
        )
    if not first["structure_matches"]:
        return (
            "The rollback object graph differs at the first divergent frame: region layout or "
            "dynamic recipe counts do not match. Inspect the first structural region mismatch "
            "and projectile/effect creation or destruction immediately before this frame."
        )
    return (
        "Inputs and snapshot structure agree, but authoritative captured state bytes differ. "
        "The first listed region and offset are the earliest concrete state evidence. Map that "
        "address/offset to its owning game type in Ghidra; later frame differences are propagation."
    )


def build_report(dump_a, dump_b, max_regions, max_diffs, include_timeline=False):
    common = sorted(set(dump_a.frames) & set(dump_b.frames))
    frame_results = []
    first = None
    for number in common:
        result = compare_frame(
            dump_a, dump_b, dump_a.frames[number], dump_b.frames[number],
            max_regions, max_diffs,
        )
        if result["meaningful_different_bytes"] or not result["structure_matches"] or not result["inputs"]["cross_peer_consistent"]:
            if first is None:
                first = result
            frame_results.append({
                "frame": number,
                "meaningful_different_bytes": result["meaningful_different_bytes"],
                "differing_regions": len(result["regions"]),
                "structure_matches": result["structure_matches"],
                "inputs_match_cross_peer": result["inputs"]["cross_peer_consistent"],
            })
            if not include_timeline:
                break
    metadata = lambda dump: {
        "path": dump.path, "role": dump.role, "trigger_frame": dump.trigger_frame,
        "git_commit": dump.git_commit, "git_dirty": dump.git_dirty,
        "frame_count": len(dump.frames), "first_frame": min(dump.frames, default=None),
        "last_frame": max(dump.frames, default=None),
    }
    report = {
        "schema": "umvc3_desync_comparison_v1",
        "dump_a": metadata(dump_a),
        "dump_b": metadata(dump_b),
        "coverage": {
            "common_frame_count": len(common),
            "common_first_frame": common[0] if common else None,
            "common_last_frame": common[-1] if common else None,
            "frames_only_a": sorted(set(dump_a.frames) - set(dump_b.frames)),
            "frames_only_b": sorted(set(dump_b.frames) - set(dump_a.frames)),
        },
        "verdict": "no_common_frames" if not common else ("match" if first is None else "diverged"),
        "first_divergence": first,
        "diagnosis": None if first is None else diagnose(first),
        "divergence_timeline": frame_results,
    }
    return report


def print_text_summary(report):
    print(f"Verdict: {report['verdict']}")
    coverage = report["coverage"]
    print(f"Common snapshots: {coverage['common_frame_count']} "
          f"({coverage['common_first_frame']}..{coverage['common_last_frame']})")
    first = report["first_divergence"]
    if not first:
        return
    print(f"First divergence: frame {first['frame']}")
    print(f"Inputs cross-peer consistent: {first['inputs']['cross_peer_consistent']}")
    print(f"Snapshot structure matches: {first['structure_matches']}")
    print(f"Meaningful differing bytes: {first['meaningful_different_bytes']}")
    print(f"Diagnosis: {report['diagnosis']}")
    for region in first["regions"]:
        print(f"  region {region['region_index']} ({region['label']}): ", end="")
        if "structural_mismatch" in region:
            print(f"structural mismatch {region['structural_mismatch']}")
        else:
            print(f"{region['meaningful_different_bytes']} differing byte(s)")
            for difference in region["differences_sample"][:3]:
                print(f"    +{difference['offset']}: A={difference['a_hex']} B={difference['b_hex']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_a", type=Path)
    parser.add_argument("dump_b", type=Path)
    parser.add_argument("--output", "-o", type=Path, help="write full JSON report to this path")
    parser.add_argument("--json", action="store_true", help="print the full JSON report")
    parser.add_argument("--max-regions", type=int, default=32,
                        help="maximum differing regions retained for the first frame")
    parser.add_argument("--max-diffs", type=int, default=16,
                        help="maximum byte runs retained per region")
    parser.add_argument("--timeline", action="store_true",
                        help="scan all frames after finding the first divergence")
    args = parser.parse_args()
    try:
        dump_a = load_dump(args.dump_a)
        dump_b = load_dump(args.dump_b)
        report = build_report(
            dump_a, dump_b, args.max_regions, args.max_diffs, args.timeline
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    rendered = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    if args.json:
        print(rendered)
    else:
        print_text_summary(report)
        if args.output:
            print(f"Full report: {args.output}")
    return 1 if report["verdict"] == "diverged" else 0


if __name__ == "__main__":
    raise SystemExit(main())
