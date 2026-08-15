#!/usr/bin/env python3
import struct
import sys
import unicodedata
import zlib
from pathlib import Path


def read_arc(path):
    data = Path(path).read_bytes()
    magic, version, count = struct.unpack_from("<4sHH", data)
    if magic != b"ARC\0":
        raise ValueError(f"{path}: not an MT Framework ARC")

    entries = []
    for index in range(count):
        pos = 8 + index * 0x50
        raw_name, type_hash, packed_size, raw_size, offset = struct.unpack_from(
            "<64sIIII", data, pos
        )
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        compressed = bool(raw_size & 0x40000000)
        raw_size &= 0x3FFFFFFF
        payload = data[offset:offset + packed_size]
        if compressed:
            payload = zlib.decompress(payload)
        if len(payload) != raw_size:
            raise ValueError(
                f"{path}:{name}: expected {raw_size} bytes, got {len(payload)}"
            )
        entries.append((name, type_hash, payload))
    return version, entries


def get_entry(path, wanted_name):
    _, entries = read_arc(path)
    for name, _, payload in entries:
        if name.lower() == wanted_name.lower():
            return payload
    raise KeyError(f"{path}: missing {wanted_name}")


def read_msd(payload):
    if payload[:4] != b"MSD\0":
        raise ValueError("not an rMsgData MSD payload")
    count = struct.unpack_from("<I", payload, 4)[0]
    pos = 8
    messages = []
    for _ in range(count):
        length = struct.unpack_from("<I", payload, pos)[0]
        pos += 4
        units = list(struct.unpack_from(f"<{length}H", payload, pos))
        pos += length * 2
        if units and units[-1] == 0xFFFF:
            units.pop()
        decoded = []
        for unit in units:
            if 0x21 <= unit <= 0x3A:
                unit += 0x20
            if unit >= 0x20:
                decoded.append(chr(unit))
            elif unit in (0, 9):
                decoded.append(" ")
        text = "".join(decoded)
        text = " ".join(text.replace("\0", " ").replace("\t", " ").split())
        text = unicodedata.normalize("NFKC", text)
        text = text.encode("ascii", "replace").decode("ascii")
        messages.append(text)
    return messages


def cpp_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def clean_name(text):
    replacements = {
        "?A": "A",
        "?B": "B",
        "?C": "C",
        "SAMURAI EDGE LOWER SHOT": "SAMURAI EDGE (LOWER SHOT)",
        "TRICK HOPSCOTCH": 'TRICK "HOPSCOTCH"',
        "TRICK PEEKABOO": 'TRICK "PEEKABOO"',
        "WIRE GRAPPLE H SHOT": "WIRE GRAPPLE (H SHOT)",
        "WIRE GRAPPLE SLANT SHOT": "WIRE GRAPPLE (SLANT SHOT)",
        "ROLLING BUCKLER ?ROLLING SLIDE": "ROLLING BUCKLER (ROLLING SLIDE)",
        "GET EM MISSILE": "GET 'EM MISSILE",
        "DEMON MISSILE CHARGE": "DEMON MISSILE (CHARGE)",
        "DEMON MISSILE SWOOP": "DEMON MISSILE (SWOOP)",
        "STARS STRIPES": "STARS & STRIPES",
        "ANTIAIR GAMMA CHARGE": "ANTI-AIR GAMMA CHARGE",
        "AIM MASTER H SHOT": "AIM MASTER (H SHOT)",
        "AIM MASTER PARABOLIC SHOT": "AIM MASTER (PARABOLIC SHOT)",
        "AIM MASTER VERTICAL SHOT": "AIM MASTER (VERTICAL SHOT)",
        "KATANARAMA": "KATANA-RAMA",
        "SENTINEL FORCE CHARGE": "SENTINEL FORCE (CHARGE)",
        "SENTINEL FORCE BOMB": "SENTINEL FORCE (BOMB)",
        "DRAGONS TOUCH": "DRAGON'S TOUCH",
        "QUICK SHOT GREYHOUND": "QUICK SHOT (GREYHOUND)",
        "TRICK SHOT VIOLET FIZZ": "TRICK SHOT (VIOLET FIZZ)",
        "RAG TIME SHOT KAMIKAZE": "RAG TIME SHOT (KAMIKAZE)",
    }
    return replacements.get(text, text)


def generate(game_root, output):
    ui = Path(game_root) / "nativePCx64" / "ui"
    mapping = get_entry(ui / "mnchs_en.arc", "ui\\chs\\chs_assist\\0000")
    if mapping[:4] != b"CSA\0":
        raise ValueError("not an rChrSelAssist CSA payload")
    entry_count = struct.unpack_from("<I", mapping, 4)[0]
    records = [struct.unpack_from("<4I", mapping, 8 + index * 0x10)
               for index in range(entry_count)]

    chrsel = read_msd(get_entry(
        ui / "mnchs_msg.arc", "ui\\font\\msg\\msg_chrsel_assist_chrsel_en"
    ))

    names = []
    for character_id in range(52):
        assists = []
        for assist in range(3):
            first_id, second_id, _, _ = records[character_id * 3 + assist]
            parts = [chrsel[index] for index in (first_id, second_id)
                     if index < len(chrsel) and chrsel[index]]
            assists.append(clean_name(" ".join(parts)))
        names.append(assists)

    lines = [
        "#pragma once",
        "",
        "// Generated from UMVC3's native English character-select resources.",
        "struct AssistMoveNames {",
        "    const char* names[3];",
        "};",
        "",
        "inline constexpr AssistMoveNames kAssistMoveNames[] = {",
    ]
    for character_id, assists in enumerate(names):
        rendered = ", ".join(cpp_string(name) for name in assists)
        lines.append(f"    {{{{{rendered}}}}}, // character {character_id}")
    lines.extend(["};", ""])
    Path(output).write_text("\n".join(lines), encoding="ascii")

    for character_id, assists in enumerate(names):
        print(f"{character_id:2}: " + " | ".join(assists))


if len(sys.argv) == 4 and sys.argv[1] == "--generate":
    generate(sys.argv[2], sys.argv[3])
    raise SystemExit


for arc_path in sys.argv[1:]:
    version, entries = read_arc(arc_path)
    print(f"{arc_path}: ARC v{version}, {len(entries)} entries")
    for name, type_hash, payload in entries:
        lower_name = name.lower()
        if ("chs_assist\\0000" in lower_name or
                "msg_sys_menu_" in lower_name or
                "msg_sys_menu2_" in lower_name or
                "msg_chrsel_assist_chrsel_" in lower_name):
            print(f"  {name} hash={type_hash:08x} size={len(payload)}")
            out = Path("/tmp/opencode") / (Path(arc_path).stem + "_" + name.replace("\\", "_") + ".bin")
            out.write_bytes(payload)
            print(f"    -> {out}")
