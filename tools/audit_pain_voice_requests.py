#!/usr/bin/env python3
"""Read-only SRQR damage-voice audit; see doc/pain_voice_fix.md."""

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path


def read_requests(path):
    data = path.read_bytes()
    magic, version, count = struct.unpack_from("<4sHH", data)
    if magic != b"ARC\0" or version != 7 or 8 + count * 0x50 > len(data):
        raise ValueError(f"{path}: unsupported or truncated ARC")
    for index in range(count):
        raw_name, type_hash, packed, raw, offset = struct.unpack_from(
            "<64sIIII", data, 8 + index * 0x50
        )
        if type_hash != 0x1BCC4966:  # rSoundRequest
            continue
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        if offset + packed > len(data):
            raise ValueError(f"{path}:{name}: truncated payload")
        payload = data[offset:offset + packed]
        if raw & 0x40000000:
            payload = zlib.decompress(payload)
        if len(payload) != raw & 0x3FFFFFFF:
            raise ValueError(f"{path}:{name}: payload size mismatch")
        yield name, payload


def audit(path):
    for name, payload in read_requests(path):
        if payload[:4] != b"SRQR" or "_vo_" not in name:
            continue
        version, count = struct.unpack_from("<II", payload, 4)
        if version != 5 or 0x48 + count * 0x90 > len(payload):
            raise ValueError(f"{path}:{name}: unsupported or truncated SRQR")
        records = {}
        for index in range(count):
            offset = 0x48 + index * 0x90
            cue_id = struct.unpack_from("<H", payload, offset)[0]
            if cue_id not in (*range(0x15, 0x21), *range(0x61, 0x64)):
                continue
            kind, enabled = struct.unpack_from("<II", payload, offset + 4)
            field_1e = struct.unpack_from("<h", payload, offset + 0x1E)[0]
            records[f"{cue_id:02x}"] = {
                "type": kind,
                "enabled": enabled,
                "field_1e": field_1e,
                "passes_allocator_entry_checks": bool(enabled and field_1e < 1000),
            }
        yield {
            "archive": str(path),
            "resource": name,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "records": records,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.archive:
        for result in audit(path):
            print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
