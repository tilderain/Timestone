#!/usr/bin/env python3

import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("compare_desync_dumps.py")
SPEC = importlib.util.spec_from_file_location("compare_desync_dumps", MODULE_PATH)
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def write_dump(path, role, image_base, frames, version=1, region_name=""):
    commit = b"0123456789abcdef0123456789abcdef01234567\0"
    header = compare.HEADER.pack(
        compare.MAGIC, version, compare.HEADER.size, len(frames),
        frames[-1][0], role, image_base, 0x2000000, 0, commit, 0, b"\0" * 6,
    )
    with open(path, "wb") as file:
        file.write(header)
        for number, local_input, remote_input, address, data in frames:
            if version >= 2:
                encoded_name = region_name.encode("utf-8")[:63].ljust(64, b"\0")
                region = compare.REGION_V2.pack(
                    address, len(data), 0, image_base + 0x1234, 0, 0,
                    b"\0" * 7, encoded_name,
                )
            else:
                region = compare.REGION_V1.pack(
                    address, len(data), 0, image_base + 0x1234, 0, 0, b"\0" * 7,
                )
            frame = compare.FRAME.pack(
                number, local_input, remote_input, 1, 0, 0, 0, 0,
                0, 0, len(data), len(data), 0, b"\0" * 7,
            )
            file.write(frame)
            file.write(region)
            file.write(data)


class CompareSnapDumpsTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.path_a = Path(self.tempdir.name) / "a.usdp"
        self.path_b = Path(self.tempdir.name) / "b.usdp"

    def tearDown(self):
        self.tempdir.cleanup()

    def test_identical_state_with_aslr_and_crossed_inputs_matches(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = struct.pack("<Q8s", base_a + 0x4567, b"samegame")
        data_b = struct.pack("<Q8s", base_b + 0x4567, b"samegame")
        write_dump(self.path_a, 1, base_a, [(10, 0x11, 0x22, base_a + 0x1000, data_a)])
        write_dump(self.path_b, 2, base_b, [(10, 0x22, 0x11, base_b + 0x1000, data_b)])

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

    def test_reports_first_meaningful_byte_difference(self):
        base_a, base_b = 0x140000000, 0x150000000
        frame_9_a = struct.pack("<Q8s", base_a + 0x4567, b"samegame")
        frame_9_b = struct.pack("<Q8s", base_b + 0x4567, b"samegame")
        frame_10_a = struct.pack("<Q8s", base_a + 0x4567, b"state-A!")
        frame_10_b = struct.pack("<Q8s", base_b + 0x4567, b"state-B!")
        write_dump(self.path_a, 1, base_a, [
            (9, 0x11, 0x22, base_a + 0x1000, frame_9_a),
            (10, 0x11, 0x22, base_a + 0x1000, frame_10_a),
        ])
        write_dump(self.path_b, 2, base_b, [
            (9, 0x22, 0x11, base_b + 0x1000, frame_9_b),
            (10, 0x22, 0x11, base_b + 0x1000, frame_10_b),
        ])

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(10, report["first_divergence"]["frame"])
        difference = report["first_divergence"]["regions"][0]["differences_sample"][0]
        self.assertEqual("0xe", difference["offset"])

    def test_rejects_truncated_dump(self):
        self.path_a.write_bytes(b"USDP")
        with self.assertRaisesRegex(ValueError, "truncated dump header"):
            compare.load_dump(self.path_a)

    def test_decodes_xor_rle_payload(self):
        previous = bytes([0, 1, 2, 3, 4, 5])
        # Three unchanged bytes, two XOR literals (7 and 9), one unchanged byte.
        encoded = bytes([(3 << 1), (2 << 1) | 1, 7, 9, (1 << 1)])
        expected = bytes([0, 1, 2, 3 ^ 7, 4 ^ 9, 5])
        self.assertEqual(expected, compare.decode_xor_rle(encoded, previous, 6))

    def test_version_two_region_name_is_reported(self):
        base_a, base_b = 0x140000000, 0x150000000
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, base_a + 0x1000, b"state-A")],
            version=2, region_name="uCharacter.motionChannels",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, base_b + 0x1000, b"state-B")],
            version=2, region_name="uCharacter.motionChannels",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual(
            "uCharacter.motionChannels",
            report["first_divergence"]["regions"][0]["label"],
        )

    def test_annotates_difference_with_ghidra_field(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = struct.pack("<If", 7, 1.5)
        data_b = struct.pack("<If", 7, 2.5)
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, base_a + 0x1000, data_a)],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, base_b + 0x1000, data_b)],
            version=2, region_name="uCharacter.team0",
        )
        structs_path = Path(self.tempdir.name) / "structs.json"
        structs_path.write_text(json.dumps({
            "schema": "umvc3_snapdump_structs_v2",
            "classes": [{
                "name": "uTestCharacter",
                "family": "character",
                "size": 8,
                "vtable_rvas": ["0x1234"],
                "fields": [
                    {"offset": 0, "size": 4, "name": "mState", "type": "uint"},
                    {"offset": 4, "size": 4, "name": "mSpeed", "type": "float"},
                ],
            }],
        }), encoding="utf-8")
        structs = compare.load_struct_database(structs_path)
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b),
            32, 16, structs=structs,
        )
        region = report["first_divergence"]["regions"][0]
        self.assertEqual("uTestCharacter", region["concrete_type"])
        field = region["differences_sample"][0]["known_fields"][0]
        self.assertEqual("mSpeed", field["name"])
        self.assertEqual(1.5, field["a_value"])
        self.assertEqual(2.5, field["b_value"])


if __name__ == "__main__":
    unittest.main()
