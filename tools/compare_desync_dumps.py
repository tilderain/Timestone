#!/usr/bin/env python3
"""Compare two UMVC3 rollback snapshot histories and emit an AI-readable report."""

import argparse
import bisect
import json
import re
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
SNAPDUMP_NAME = re.compile(
    r"desync_(\d{8}_\d{6})_(host|client)_f(-?\d+)\.usdp$"
)
NONZERO_BYTE_TABLE = bytes(value != 0 for value in range(256))
DEFAULT_SKIP_FRAME0_POINTERS = True
EXPLICIT_REGION_OVERRIDES = {
    "uCharacter.externalAnimation",
    "uCharacter.jointStates",
    "uCharacter.motionBlend",
}
EXPLICIT_BYTE_OVERRIDES = {
    ("sAction.full", 0x3634, 1),
    # uBaseModel render/cache state retained on the live timeline by restore.
    ("uCharacter.team0", 0x93, 1),
    ("uCharacter.team0", 0x96, 2),
    ("uCharacter.team0", 0x190, 8),
    ("uCharacter.team0", 0x12D0, 0xC),
    ("uCharacter.team0", 0x18C, 4),
    ("uCharacter.team1", 0x190, 8),
    ("uCharacter.team1", 0x93, 1),
    ("uCharacter.team1", 0x96, 2),
    ("uCharacter.team1", 0x12D0, 0xC),
    ("uCharacter.team1", 0x18C, 4),
}
FRAME0_EXPLICIT_BYTE_OVERRIDES = {
    ("uActionSideCamera", 0x18, 8),
    ("uCharacter.team0", 0x13B0, 8),
    ("uCharacter.team1", 0x13B0, 8),
    ("sAction.full", 0x1F0, 0x20),
    ("sAction.compact", 0x1F0, 0x20),
    *(("sAction.full", 0x163C + index * 0x20, 8) for index in range(12)),
}
EXPLICIT_VTABLE_BYTE_OVERRIDES = {
    # The broad character region extends 0x16c bytes beyond uChunLi's DTI size.
    0xA71F90: ((0x68F4, 0x16C),),
}


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


@dataclass
class TypeLayout:
    name: str
    family: str
    size: int
    fields: list
    vtable_rvas: list


class StructDatabase:
    REGION_BINDINGS = {
        "sReplay.frameCounter": ("sReplay", 0x54),
        "sEffect.rng": ("sEffect", 0x3C),
        "sShot": ("sShot", 0),
        "sGameConfig.shotSlotFlags": ("sGameConfig", 0x4A89C),
        "sAction.full": ("sAction", 0),
        "sAction.compact": ("sAction", 0),
        "sCamera": ("sCamera", 0),
        "uActionSideCamera": ("uActionSideCamera", 0),
        "cChrTeam[0]": ("cChrTeam", 0),
        "cChrTeam[1]": ("cChrTeam", 0),
        "uCharacter.motionBlend": ("RollbackMotionBlendRecord", 0),
        "uCharacter.jointStates": ("RollbackJointWorkRecord", 0),
    }
    REPEATING_REGION_BINDINGS = {
        "uCharacter.motionBlend",
        "uCharacter.jointStates",
    }

    def __init__(self, path, layouts):
        self.path = str(path)
        self.layouts = layouts
        self.by_name = {layout.name: layout for layout in layouts}
        self.by_vtable = {}
        for layout in layouts:
            for rva in layout.vtable_rvas:
                self.by_vtable.setdefault(rva, []).append(layout)

    def resolve(self, region, label, dump, saved_vtable=0):
        vtable = region.expected_vtable or saved_vtable
        if vtable and dump.image_base <= vtable < dump.image_base + dump.image_size:
            rva = vtable - dump.image_base
            candidates = self.by_vtable.get(rva, [])
            if len(candidates) == 1:
                source = "manifest_vtable" if region.expected_vtable else "saved_vtable"
                return candidates[0], 0, source
            exact_size = [layout for layout in candidates if layout.size == region.size]
            if len(exact_size) == 1:
                source = "manifest_vtable_size" if region.expected_vtable else "saved_vtable_size"
                return exact_size[0], 0, source
        binding = self.REGION_BINDINGS.get(label)
        if binding:
            layout = self.by_name.get(binding[0])
            if layout:
                return layout, binding[1], "region_binding"
        return None, 0, None


def parse_int(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def load_struct_database(path):
    with open(path, "r", encoding="utf-8") as file:
        document = json.load(file)
    if document.get("schema") not in {
            "umvc3_snapdump_structs_v1", "umvc3_snapdump_structs_v2"}:
        raise ValueError(f"unsupported struct schema {document.get('schema')!r}")
    layouts = []
    for record in document.get("classes", []):
        fields = sorted(record.get("fields", []), key=lambda field: field["offset"])
        layouts.append(TypeLayout(
            record["name"], record.get("family", "other"), int(record["size"]),
            fields, [parse_int(value) for value in record.get("vtable_rvas", [])],
        ))
    return StructDatabase(path, layouts)


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


def latest_snapdump_paths(directories=None):
    """Return the newest complete host/client snapdump pair in search directories."""
    if directories is None:
        game_directory = (
            Path(__file__).resolve().parent / ".." / ".." /
            "ULTIMATE MARVEL VS. CAPCOM 3"
        ).resolve()
        directories = (Path("."), game_directory)
    elif isinstance(directories, (str, Path)):
        directories = (Path(directories),)
    pairs = {}
    for directory in directories:
        for path in directory.glob("desync_*.usdp"):
            match = SNAPDUMP_NAME.fullmatch(path.name)
            if not match:
                continue
            timestamp, role, frame = match.groups()
            pairs.setdefault((timestamp, int(frame)), {})[role] = path
    complete = [(key, paths) for key, paths in pairs.items()
                if "host" in paths and "client" in paths]
    if not complete:
        return None, None
    _, paths = max(complete, key=lambda item: item[0])
    return paths["host"], paths["client"]


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


def looks_like_user_pointer(value):
    return 0x10000 <= value < 0x0000800000000000


def differing_byte_count(data_a, data_b):
    """Count unequal byte positions without iterating through Python integers."""
    size = min(len(data_a), len(data_b))
    if size == 0:
        return 0
    value = int.from_bytes(data_a[:size], "little") ^ int.from_bytes(data_b[:size], "little")
    return value.to_bytes(size, "little").translate(NONZERO_BYTE_TABLE).count(1)


def collect_frame0_pointer_overrides(dump_a, dump_b):
    if 0 not in dump_a.frames or 0 not in dump_b.frames:
        return set()
    frame_a, frame_b = dump_a.frames[0], dump_b.frames[0]
    resolver_a = PointerResolver(dump_a, frame_a)
    resolver_b = PointerResolver(dump_b, frame_b)
    overrides = set()
    for index, (region_a, region_b) in enumerate(zip(frame_a.regions, frame_b.regions)):
        if region_a.size != region_b.size:
            continue
        label_a = region_label(index, region_a, dump_a, resolver_a)
        label_b = region_label(index, region_b, dump_b, resolver_b)
        if label_a != label_b:
            continue
        data_a = frame_a.data[region_a.data_offset:region_a.data_offset + region_a.size]
        data_b = frame_b.data[region_b.data_offset:region_b.data_offset + region_b.size]
        for offset in range(0, min(len(data_a), len(data_b)) - 7, 8):
            value_a = int.from_bytes(data_a[offset:offset + 8], "little")
            value_b = int.from_bytes(data_b[offset:offset + 8], "little")
            if (value_a != value_b and
                    looks_like_user_pointer(value_a) and
                    looks_like_user_pointer(value_b)):
                overrides.add((index, label_a, offset, 8))
        for override_label, offset, size in FRAME0_EXPLICIT_BYTE_OVERRIDES:
            if label_a == override_label and offset + size <= len(data_a):
                overrides.add((index, label_a, offset, size))
        saved_vtable = int.from_bytes(data_a[:8], "little") if len(data_a) >= 8 else 0
        vtable_rva = saved_vtable - dump_a.image_base
        for offset, size in EXPLICIT_VTABLE_BYTE_OVERRIDES.get(vtable_rva, ()):
            if offset + size <= len(data_a):
                overrides.add((index, label_a, offset, size))
    return overrides


def collect_explicit_region_overrides(dump_a, dump_b):
    common = sorted(set(dump_a.frames) & set(dump_b.frames))
    if not common:
        return set()
    frame_a, frame_b = dump_a.frames[common[0]], dump_b.frames[common[0]]
    resolver_a = PointerResolver(dump_a, frame_a)
    resolver_b = PointerResolver(dump_b, frame_b)
    overrides = set()
    for index, (region_a, region_b) in enumerate(zip(frame_a.regions, frame_b.regions)):
        if region_a.size != region_b.size:
            continue
        label_a = region_label(index, region_a, dump_a, resolver_a)
        label_b = region_label(index, region_b, dump_b, resolver_b)
        if label_a != label_b:
            continue
        if label_a in EXPLICIT_REGION_OVERRIDES:
            overrides.add((index, label_a, 0, region_a.size))
        for override_label, offset, size in EXPLICIT_BYTE_OVERRIDES:
            if label_a == override_label and offset + size <= region_a.size:
                overrides.add((index, label_a, offset, size))
    return overrides


def ignored_pointer_bytes(index, label, data_a, data_b, pointer_overrides):
    ignored = set()
    overridden = []
    if not pointer_overrides:
        return ignored, overridden
    for offset, size in pointer_overrides.get((index, label), ()):
        value_a = int.from_bytes(data_a[offset:offset + size], "little")
        value_b = int.from_bytes(data_b[offset:offset + size], "little")
        ignored.update(range(offset, offset + size))
        overridden.append({
            "offset": f"0x{offset:x}",
            "size": size,
            "a_value": f"0x{value_a:x}",
            "b_value": f"0x{value_b:x}",
        })
    return ignored, overridden


def decode_field_value(data, field_type):
    normalized = field_type.lower()
    if not data:
        return None
    if normalized in {"float", "f32"} and len(data) == 4:
        return struct.unpack("<f", data)[0]
    if normalized in {"double", "f64"} and len(data) == 8:
        return struct.unpack("<d", data)[0]
    if normalized in {"bool", "boolean"} and len(data) == 1:
        return bool(data[0])
    if normalized in {"char", "byte", "uchar", "uint8_t", "u8"}:
        return int.from_bytes(data, "little")
    if normalized in {"int", "long", "s32", "int32_t"} and len(data) == 4:
        return int.from_bytes(data, "little", signed=True)
    if normalized in {"short", "s16", "int16_t"} and len(data) == 2:
        return int.from_bytes(data, "little", signed=True)
    if normalized in {"longlong", "s64", "int64_t"} and len(data) == 8:
        return int.from_bytes(data, "little", signed=True)
    if normalized in {
            "uint", "ulong", "dword", "u32", "uint32_t", "undefined4"} and len(data) == 4:
        return int.from_bytes(data, "little")
    if normalized in {
            "ushort", "word", "u16", "uint16_t", "undefined2"} and len(data) == 2:
        return int.from_bytes(data, "little")
    if normalized in {
            "ulonglong", "qword", "u64", "uint64_t", "undefined8", "pointer"} and len(data) == 8:
        return f"0x{int.from_bytes(data, 'little'):x}"
    return data.hex()


def annotate_run(layout, object_base, start, end, data_a, data_b, repeat_size=0):
    annotations = []
    first_element = start // repeat_size if repeat_size else 0
    last_element = (end - 1) // repeat_size if repeat_size else 0
    for element_index in range(first_element, last_element + 1):
        element_base = element_index * repeat_size if repeat_size else 0
        object_start = object_base + max(start, element_base) - element_base
        object_end = object_base + min(end, element_base + repeat_size) - element_base \
            if repeat_size else object_base + end
        for field in layout.fields:
            field_start = int(field["offset"])
            field_end = field_start + int(field["size"])
            if field_end <= object_start or field_start >= object_end:
                continue
            region_field_start = element_base + field_start - object_base
            region_field_end = region_field_start + int(field["size"])
            annotation = {
                "name": field["name"],
                "type": field["type"],
                "offset": f"0x{field_start:x}",
                "size": int(field["size"]),
            }
            if repeat_size:
                annotation["element_index"] = element_index
            if region_field_start >= 0 and region_field_end <= len(data_a):
                annotation["a_value"] = decode_field_value(
                    data_a[region_field_start:region_field_end], field["type"]
                )
                annotation["b_value"] = decode_field_value(
                    data_b[region_field_start:region_field_end], field["type"]
                )
            annotations.append(annotation)
    return annotations


def compare_region(index, region_a, region_b, dump_a, dump_b, frame_a, frame_b,
                   resolver_a, resolver_b, max_diffs, structs=None,
                   pointer_overrides=None, details=True):
    start_a, start_b = region_a.data_offset, region_b.data_offset
    data_a = frame_a.data[start_a:start_a + region_a.size]
    data_b = frame_b.data[start_b:start_b + region_b.size]
    label = region_label(index, region_a, dump_a, resolver_a)
    region_overrides = pointer_overrides.get((index, label), ()) if pointer_overrides else ()
    fully_ignored = (0, region_a.size) in region_overrides
    if fully_ignored or data_a == data_b:
        return {
            "region_index": index,
            "label": label,
            "concrete_type": None,
            "type_source": None,
            "object_base_offset": None,
            "size": region_a.size,
            "raw_different_bytes": 0,
            "relocation_only_bytes": 0,
            "meaningful_different_bytes": 0,
            "pointer_relocations_sample": [],
            "frame0_pointer_overrides_sample": [],
            "differences_sample": [],
        }
    ignored, overridden_pointers = ignored_pointer_bytes(
        index, label, data_a, data_b, pointer_overrides
    )
    relocations = []
    for offset in range(0, min(len(data_a), len(data_b)) - 7, 8):
        value_a = int.from_bytes(data_a[offset:offset + 8], "little")
        value_b = int.from_bytes(data_b[offset:offset + 8], "little")
        if value_a == value_b:
            continue
        if (offset, 8) in region_overrides:
            continue
        pointer_a = resolver_a.resolve(value_a)
        pointer_b = resolver_b.resolve(value_b)
        if pointer_a is not None and pointer_a == pointer_b:
            ignored.update(range(offset, offset + 8))
            if len(relocations) < max_diffs:
                relocations.append({"offset": f"0x{offset:x}", "target": list(pointer_a)})
    if not details:
        raw_diff_bytes = differing_byte_count(data_a, data_b)
        ignored_diff_bytes = sum(
            data_a[offset] != data_b[offset] for offset in ignored
        )
        return {
            "region_index": index,
            "label": label,
            "concrete_type": None,
            "type_source": None,
            "object_base_offset": None,
            "size": region_a.size,
            "raw_different_bytes": raw_diff_bytes,
            "relocation_only_bytes": ignored_diff_bytes,
            "meaningful_different_bytes": raw_diff_bytes - ignored_diff_bytes,
            "pointer_relocations_sample": relocations,
            "frame0_pointer_overrides_sample": overridden_pointers[:max_diffs],
            "differences_sample": [],
        }
    layout = None
    object_base = 0
    type_source = None
    if details and structs:
        saved_vtable = int.from_bytes(data_a[:8], "little") if len(data_a) >= 8 else 0
        layout, object_base, type_source = structs.resolve(
            region_a, label, dump_a, saved_vtable
        )
    repeat_size = (layout.size if layout and structs and
                   label in structs.REPEATING_REGION_BINDINGS else 0)
    raw_diff_bytes = 0
    meaningful_diff_bytes = 0
    run_bounds = []
    run_start = None
    for offset, (value_a, value_b) in enumerate(zip(data_a, data_b)):
        different = value_a != value_b
        meaningful = different and offset not in ignored
        if different:
            raw_diff_bytes += 1
        if meaningful:
            meaningful_diff_bytes += 1
            if details and run_start is None and len(run_bounds) < max_diffs:
                run_start = offset
            if run_start is not None and offset - run_start == 31:
                run_bounds.append((run_start, offset + 1))
                run_start = offset + 1 if len(run_bounds) < max_diffs else None
        elif details and run_start is not None:
            run_bounds.append((run_start, offset))
            run_start = None
    if details and run_start is not None:
        run_bounds.append((run_start, len(data_a)))

    runs = []
    for start, end in run_bounds:
        context_start = max(0, start - 8)
        context_end = min(len(data_a), end + 8)
        difference = {
            "offset": f"0x{start:x}",
            "object_offset": (f"0x{object_base + start % repeat_size:x}"
                              if repeat_size else
                              f"0x{object_base + start:x}" if layout else None),
            "length": end - start,
            "a_hex": data_a[start:end].hex(),
            "b_hex": data_b[start:end].hex(),
            "context_offset": f"0x{context_start:x}",
            "a_context_hex": data_a[context_start:context_end].hex(),
            "b_context_hex": data_b[context_start:context_end].hex(),
        }
        if layout:
            if repeat_size:
                difference["element_index"] = start // repeat_size
            difference["known_fields"] = annotate_run(
                layout, object_base, start, end, data_a, data_b, repeat_size
            )
        runs.append(difference)
    return {
        "region_index": index,
        "label": label,
        "concrete_type": layout.name if layout else None,
        "type_source": type_source,
        "object_base_offset": f"0x{object_base:x}" if layout else None,
        "size": region_a.size,
        "raw_different_bytes": raw_diff_bytes,
        "relocation_only_bytes": raw_diff_bytes - meaningful_diff_bytes,
        "meaningful_different_bytes": meaningful_diff_bytes,
        "pointer_relocations_sample": relocations,
        "frame0_pointer_overrides_sample": overridden_pointers[:max_diffs],
        "differences_sample": runs,
    }


def compare_frame(dump_a, dump_b, frame_a, frame_b, max_regions, max_diffs,
                    structs=None, pointer_overrides=None, details=True):
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
                resolver_a, resolver_b, max_diffs, structs, pointer_overrides, details,
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
        "The first listed region and offset are the earliest concrete state evidence. Current "
        "Ghidra fields are shown where known; unannotated offsets still require reverse engineering. "
        "Later frame differences are propagation."
    )


def build_report(dump_a, dump_b, max_regions, max_diffs, include_timeline=False,
                   structs=None, skip_frame0_pointers=DEFAULT_SKIP_FRAME0_POINTERS):
    common = sorted(set(dump_a.frames) & set(dump_b.frames))
    pointer_overrides = (
        collect_frame0_pointer_overrides(dump_a, dump_b) |
        collect_explicit_region_overrides(dump_a, dump_b)
        if skip_frame0_pointers else set()
    )
    overrides_by_region = {}
    for index, label, offset, size in pointer_overrides:
        overrides_by_region.setdefault((index, label), set()).add((offset, size))
    frame_results = []
    first = None
    for number in common:
        result = compare_frame(
            dump_a, dump_b, dump_a.frames[number], dump_b.frames[number],
            max_regions, max_diffs, structs, overrides_by_region, False,
        )
        if result["meaningful_different_bytes"] or not result["structure_matches"] or not result["inputs"]["cross_peer_consistent"]:
            if first is None:
                first = compare_frame(
                    dump_a, dump_b, dump_a.frames[number], dump_b.frames[number],
                    max_regions, max_diffs, structs, overrides_by_region,
                )
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
        "struct_database": structs.path if structs else None,
        "frame0_pointer_override": {
            "enabled": skip_frame0_pointers,
            "slot_count": sum(size == 8 for _, _, _, size in pointer_overrides),
            "explicit_byte_count": sum(size for _, _, _, size in pointer_overrides if size != 8),
        },
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
            concrete = (f", {region['concrete_type']}" if region.get("concrete_type")
                        else "")
            print(f"{region['meaningful_different_bytes']} differing byte(s){concrete}")
            for difference in region["differences_sample"][:3]:
                fields = difference.get("known_fields", [])
                field_text = ""
                if fields:
                    field_text = " [" + ", ".join(
                        f"{field['name']}:{field['type']}" for field in fields
                    ) + "]"
                print(f"    +{difference['offset']}: A={difference['a_hex']} "
                      f"B={difference['b_hex']}{field_text}")
                for field in fields:
                    if "a_value" in field:
                        print(f"      {field['name']} @ {field['offset']}: "
                              f"A={field['a_value']} B={field['b_value']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_a", type=Path, nargs="?",
                        help="first dump (defaults to latest host dump in cwd)")
    parser.add_argument("dump_b", type=Path, nargs="?",
                        help="second dump (defaults to latest client dump in cwd)")
    parser.add_argument("--output", "-o", type=Path, help="write full JSON report to this path")
    parser.add_argument("--json", action="store_true", help="print the full JSON report")
    parser.add_argument("--max-regions", type=int, default=32,
                        help="maximum differing regions retained for the first frame")
    parser.add_argument("--max-diffs", type=int, default=16,
                        help="maximum byte runs retained per region")
    parser.add_argument("--timeline", action="store_true",
                        help="scan all frames after finding the first divergence")
    default_structs = Path(__file__).resolve().parents[1] / "ghidra_export" / "snapdump_structs.json"
    parser.add_argument("--structs", type=Path,
                        default=default_structs if default_structs.is_file() else None,
                        help="Ghidra structure schema (default: ghidra_export/snapdump_structs.json)")
    parser.add_argument("--no-structs", action="store_true",
                        help="disable Ghidra field annotation")
    parser.add_argument(
        "--skip-frame0-pointers", action=argparse.BooleanOptionalAction,
        default=DEFAULT_SKIP_FRAME0_POINTERS,
        help="ignore exact qword slots exposed as peer-specific pointers at frame 0 (default: on)",
    )
    args = parser.parse_args()
    if args.dump_a is None or args.dump_b is None:
        args.dump_a, args.dump_b = latest_snapdump_paths()
        if args.dump_a is None:
            parser.error(
                "no complete desync_*_host/_client_fN.usdp pair found in cwd or "
                "../../ULTIMATE MARVEL VS. CAPCOM 3 and two dump paths were not provided"
            )
    try:
        dump_a = load_dump(args.dump_a)
        dump_b = load_dump(args.dump_b)
        structs = None if args.no_structs or args.structs is None else load_struct_database(args.structs)
        report = build_report(
            dump_a, dump_b, args.max_regions, args.max_diffs, args.timeline, structs,
            args.skip_frame0_pointers,
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
