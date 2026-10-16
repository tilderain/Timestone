#!/usr/bin/env python3
"""Inventory native CAC motion/blend/collision commands, not execute scripts."""
import argparse
import json
import struct
import zlib
from pathlib import Path


FORMATS = {
    (0, 0x21): "<iifi", (0, 0x22): "<iiffi",
    (0, 0x23): "<iiffIi", (0, 0x24): "<iiiffi",
    (1, 0x81): "<IfiiBIfii",
    (1, 0x99): "<ii", (1, 0x9A): "<iii", (1, 0x9C): "<i",
    (1, 0x9F): "<i", (1, 0xA0): "<i",
    (1, 0x29): "<I", (1, 0x2A): "<I", (1, 0x2B): "<I", (1, 0x2C): "<I",
    (1, 0x41): "<I", (1, 0x42): "<I", (1, 0x43): "<I", (1, 0x44): "<I",
}


def animation_resources(path):
    with path.open("rb") as stream:
        magic, version, count = struct.unpack("<4sHH", stream.read(8))
        if magic != b"ARC\0" or version != 7:
            raise ValueError("unsupported ARC header")
        table = [struct.unpack("<64s4I", stream.read(80)) for _ in range(count)]
        for raw_name, kind, size, raw_size, offset in table:
            if kind != 0x5A7E5D8A:
                continue
            stream.seek(offset)
            data = stream.read(size)
            if raw_size & 0x40000000:
                data = zlib.decompress(data)
            if len(data) != (raw_size & 0x3FFFFFFF):
                raise ValueError("ARC payload size mismatch")
            yield raw_name.split(b"\0", 1)[0].decode("ascii"), data


def commands(data):
    def integer(pos, signed=False):
        return struct.unpack_from("<i" if signed else "<I", data, pos)[0]

    if data[:4] != b"CAC\0" or integer(4) != 0x473EA35B:
        raise ValueError("unsupported CAC header")
    count = integer(8, True)
    if count < 0 or 16 + count * 8 > len(data):
        raise ValueError("CAC directory outside payload")
    for index in range(count):
        action, base = struct.unpack_from("<iI", data, 16 + index * 8)
        keys = integer(base, True)
        if keys < 0 or base + 24 + keys * 8 > len(data):
            raise ValueError("CAC block directory outside payload")
        blocks = [("simple", -1, base + integer(base + 20))]
        blocks.extend(("timed", k, base + integer(base + 28 + k * 8))
                      for k in range(keys))
        for kind, key, block in blocks:
            frame, size = struct.unpack_from("<Ii", data, block)
            if size < 0 or block + 16 + size * 8 > len(data):
                raise ValueError("CAC command directory outside payload")
            for slot in range(size):
                pos = block + integer(block + 16 + slot * 8)
                group, opcode, descriptors = struct.unpack_from("<IIi", data, pos)
                payload = pos + 16 + descriptors * 4
                if descriptors < 0 or payload > len(data):
                    raise ValueError("CAC descriptors outside payload")
                schema = FORMATS.get((group, opcode))
                yield {"action": action, "block": kind, "key": key,
                       "frame": frame, "slot": slot, "offset": pos,
                       "group": group, "opcode": opcode,
                       "values": list(struct.unpack_from(schema, data, payload)) if schema else None,
                       "native_action_valid": keys != 0}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    output = []
    for archive in args.archives:
        for name, data in animation_resources(archive):
            all_commands = list(commands(data))
            output.append({"archive": archive.name, "resource": name,
                           "command_count": len(all_commands),
                           "commands": [c for c in all_commands if c["values"] is not None]})
    print(json.dumps(output, indent=2))
