#!/usr/bin/env python3
"""Measure exact-byte snapshot reuse in a recorded USDP history.

This is a workload audit, not a timing benchmark or proof that unchanged state
may be omitted. Uses the existing dump decoder; does not modify the dump.
"""

import argparse
from collections import Counter, defaultdict
import json

from compare_desync_dumps import load_dump


def identity(region):
    return (region.address, region.size, region.owner_slot,
            region.expected_vtable, region.skip_vtable, region.name)


def compare_regions(current, previous, totals, chunk_size):
    for key, data in current.items():
        before = previous.get(key)
        if before is None:
            continue
        stats = totals[key[-1]]
        stats['compared_bytes'] += len(data)
        stats['compared_regions'] += 1
        if data == before:
            stats['equal_region_bytes'] += len(data)
            stats['equal_regions'] += 1
        for offset in range(0, len(data), chunk_size):
            chunk = data[offset:offset + chunk_size]
            if chunk == before[offset:offset + chunk_size]:
                stats['equal_chunk_bytes'] += len(chunk)


def describe(stats):
    result = {name: stats[name] for name in (
        'compared_bytes', 'compared_regions', 'equal_region_bytes',
        'equal_regions', 'equal_chunk_bytes')}
    count = result['compared_bytes']
    result['equal_region_byte_percent'] = (
        100 * result['equal_region_bytes'] / count if count else None)
    result['equal_chunk_byte_percent'] = (
        100 * result['equal_chunk_bytes'] / count if count else None)
    return result


def summarize(totals):
    return {
        'total': describe(sum(totals.values(), Counter())),
        'regions': {name: describe(stats) for name, stats in sorted(
            totals.items(), key=lambda item: -item[1]['compared_bytes'])},
    }


def audit(path, ring_size, chunk_size):
    dump = load_dump(path)
    frames = sorted(dump.frames.values(), key=lambda frame: frame.frame)
    adjacent = defaultdict(Counter)
    same_slot = defaultdict(Counter)
    previous = {}
    previous_frame = None
    ring = {}
    manifests = set()
    for frame in frames:
        current = {}
        for region in frame.regions:
            key = identity(region)
            if key in current:
                raise ValueError('duplicate region identity; cannot audit without occurrence IDs')
            current[key] = frame.data[region.data_offset:region.data_offset + region.size]
        manifests.add((len(frame.regions), len(frame.data)))
        if previous_frame is not None and frame.frame == previous_frame + 1:
            compare_regions(current, previous, adjacent, chunk_size)
        slot = frame.frame % ring_size
        compare_regions(current, ring.get(slot, {}), same_slot, chunk_size)
        ring[slot] = current
        previous = current
        previous_frame = frame.frame
    return {
        'schema': 'umvc3_snapshot_copy_reuse_audit_v1',
        'kind': 'historical_byte_equality_only_not_performance_verification',
        'path': dump.path, 'role': dump.role,
        'git_commit': dump.git_commit, 'git_dirty': dump.git_dirty,
        'frame_count': len(frames),
        'first_frame': frames[0].frame if frames else None,
        'last_frame': frames[-1].frame if frames else None,
        'region_count_and_bytes': sorted(manifests),
        'ring_size': ring_size, 'chunk_size': chunk_size,
        'adjacent_frames': summarize(adjacent),
        'same_ring_slot_chronological_history': summarize(same_slot),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump')
    parser.add_argument('--ring-size', type=int, default=16)
    parser.add_argument('--chunk-size', type=int, default=4096)
    args = parser.parse_args()
    if args.ring_size <= 0 or args.chunk_size <= 0:
        parser.error('ring and chunk sizes must be positive')
    print(json.dumps(audit(args.dump, args.ring_size, args.chunk_size), indent=2))


if __name__ == '__main__':
    main()
