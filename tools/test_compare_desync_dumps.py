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


def write_dump(path, role, image_base, frames, version=1, region_name="",
               region_names=None):
    commit = b"0123456789abcdef0123456789abcdef01234567\0"
    header = compare.HEADER.pack(
        compare.MAGIC, version, compare.HEADER.size, len(frames),
        frames[-1][0], role, image_base, 0x2000000, 0, commit, 0, b"\0" * 6,
    )
    with open(path, "wb") as file:
        file.write(header)
        for frame_index, (number, local_input, remote_input, address, data) in enumerate(frames):
            if version >= 2:
                name = region_names[frame_index] if region_names else region_name
                encoded_name = name.encode("utf-8")[:63].ljust(64, b"\0")
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

    def test_can_compare_only_selected_frames(self):
        base_a, base_b = 0x140000000, 0x150000000
        same_a = struct.pack("<Q4s", base_a + 0x4567, b"same")
        same_b = struct.pack("<Q4s", base_b + 0x4567, b"same")
        different_a = struct.pack("<Q4s", base_a + 0x4567, b"host")
        different_b = struct.pack("<Q4s", base_b + 0x4567, b"client")
        write_dump(self.path_a, 1, base_a, [
            (54, 0x11, 0x22, base_a + 0x1000, same_a),
            (55, 0x11, 0x22, base_a + 0x1000, different_a),
        ])
        write_dump(self.path_b, 2, base_b, [
            (54, 0x22, 0x11, base_b + 0x1000, same_b),
            (55, 0x22, 0x11, base_b + 0x1000, different_b),
        ])

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b),
            32, 16, selected_frames=[54],
        )
        self.assertEqual("match", report["verdict"])
        self.assertEqual([54], report["coverage"]["selected_frames"])

    def test_rejects_truncated_dump(self):
        self.path_a.write_bytes(b"USDP")
        with self.assertRaisesRegex(ValueError, "truncated dump header"):
            compare.load_dump(self.path_a)

    def test_loads_v3_complete_shot_body_and_collision(self):
        image_base = 0x140000000
        header = compare.HEADER.pack(
            compare.MAGIC, 3, compare.HEADER.size, 1, 10, 1,
            image_base, 0x2000000, 0, b"test\0".ljust(41, b"\0"), 0, b"\0" * 6,
        )
        body = bytes(range(64))
        primitive_data = bytes(range(compare.SHOT_PRIMITIVE_DATA_SIZE))
        primitive = compare.SHOT_PRIMITIVE_V3.pack(
            2, 3, 1, 7, 1, b"\0" * 2, 9, 0x10,
            0x1111, 0x2222, 0x20, -1,
        ) + primitive_data
        record_size = compare.SHOT_V3.size + 48 + len(body) + len(primitive)
        shot = compare.SHOT_V3.pack(
            record_size, 4, 1, 1, 0, 0, 99, 0x1000, image_base + 0x1230,
            image_base + 0x4560, 0x55, 0x2000, 0x3000, 0x4000,
            0x5000, image_base + 0x6000, 1, b"\0" * 7,
            0x7000, image_base + 0x8000, 0, b"\0" * 7,
            -2, 0x9000, 3, 0xA000, 0, len(body), 1, 0,
        ) + bytes(range(48)) + body + primitive
        frame = compare.FRAME.pack(
            10, 0x11, 0x22, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, b"\0" * 7,
        )
        self.path_a.write_bytes(header + frame + shot)

        dump = compare.load_dump(self.path_a)
        parsed = dump.frames[10].shots[0]
        self.assertEqual(99, parsed["spawn_sequence"])
        self.assertEqual(body, parsed["body"])
        self.assertEqual(1, len(parsed["collision_primitives"]))
        self.assertEqual(primitive_data[:0x24],
                         parsed["collision_primitives"][0]["geometry"])

    def test_finds_latest_complete_snapdump_pair(self):
        directory = Path(self.tempdir.name)
        old_host = directory / "desync_20260729_062239_host_f425.usdp"
        old_client = directory / "desync_20260729_062239_client_f425.usdp"
        new_host = directory / "desync_20260729_062652_host_f367.usdp"
        new_client = directory / "desync_20260729_062652_client_f367.usdp"
        incomplete = directory / "desync_20260729_070000_host_f500.usdp"
        for path in (old_host, old_client, new_host, new_client, incomplete):
            path.touch()

        self.assertEqual(
            (new_host, new_client), compare.latest_snapdump_paths(directory)
        )

    def test_latest_snapdump_pair_requires_matching_frame(self):
        directory = Path(self.tempdir.name)
        (directory / "desync_20260729_062652_host_f367.usdp").touch()
        (directory / "desync_20260729_062652_client_f368.usdp").touch()

        self.assertEqual((None, None), compare.latest_snapdump_paths(directory))

    def test_latest_snapdump_pair_allows_peer_write_time_skew(self):
        directory = Path(self.tempdir.name)
        host = directory / "desync_20260729_153535_host_f445.usdp"
        client = directory / "desync_20260729_153534_client_f445.usdp"
        host.touch()
        client.touch()

        self.assertEqual((host, client), compare.latest_snapdump_paths(directory))

    def test_finds_latest_pair_across_search_directories(self):
        first = Path(self.tempdir.name) / "first"
        second = Path(self.tempdir.name) / "second"
        first.mkdir()
        second.mkdir()
        (first / "desync_20260729_062239_host_f425.usdp").touch()
        (first / "desync_20260729_062239_client_f425.usdp").touch()
        new_host = second / "desync_20260729_062652_host_f367.usdp"
        new_client = second / "desync_20260729_062652_client_f367.usdp"
        new_host.touch()
        new_client.touch()

        self.assertEqual(
            (new_host, new_client),
            compare.latest_snapdump_paths((first, second)),
        )

    def test_decodes_xor_rle_payload(self):
        previous = bytes([0, 1, 2, 3, 4, 5])
        # Three unchanged bytes, two XOR literals (7 and 9), one unchanged byte.
        encoded = bytes([(3 << 1), (2 << 1) | 1, 7, 9, (1 << 1)])
        expected = bytes([0, 1, 2, 3 ^ 7, 4 ^ 9, 5])
        self.assertEqual(expected, compare.decode_xor_rle(encoded, previous, 6))

    def test_counts_differing_bytes(self):
        self.assertEqual(0, compare.differing_byte_count(b"same", b"same"))
        self.assertEqual(2, compare.differing_byte_count(b"\x00\xff\x10", b"\x01\xff\x11"))
        self.assertEqual(2, compare.differing_byte_count(b"\x00\xff\x00", b"\x01\x00\x00"))

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

    def test_repeats_ghidra_fields_for_dynamic_array_elements(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x180)
        data_b = bytearray(data_a)
        data_a[0xC0 + 0x14:0xC0 + 0x18] = struct.pack("<I", 3)
        data_b[0xC0 + 0x14:0xC0 + 0x18] = struct.pack("<I", 4)
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, 0xF119E50, bytes(data_a))],
            version=2, region_name="uCharacter.jointStates",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, 0xF0D9E50, bytes(data_b))],
            version=2, region_name="uCharacter.jointStates",
        )
        structs_path = Path(self.tempdir.name) / "structs.json"
        structs_path.write_text(json.dumps({
            "schema": "umvc3_snapdump_structs_v2",
            "classes": [{
                "name": "RollbackJointWorkRecord",
                "family": "other",
                "size": 0xC0,
                "vtable_rvas": [],
                "fields": [
                    {"offset": 0x14, "size": 4, "name": "mDepth", "type": "uint"},
                ],
            }],
        }), encoding="utf-8")

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b),
            32, 16, structs=compare.load_struct_database(structs_path),
            skip_frame0_pointers=False,
        )
        difference = report["first_divergence"]["regions"][0]["differences_sample"][0]
        self.assertEqual(1, difference["element_index"])
        self.assertEqual("0x14", difference["object_offset"])
        field = difference["known_fields"][0]
        self.assertEqual("mDepth", field["name"])
        self.assertEqual(1, field["element_index"])
        self.assertEqual(3, field["a_value"])
        self.assertEqual(4, field["b_value"])

    def test_skips_frame0_pointer_slots_by_default(self):
        base_a, base_b = 0x140000000, 0x150000000
        pointers_a = [0x5DFACB0, 0x5DFACE0, 0x5DFE7D0, 0x5DFAD10]
        pointers_b = [value - 0x70000 for value in pointers_a]
        data_a = b"".join(struct.pack("<Q", value) for value in pointers_a)
        data_b = b"".join(struct.pack("<Q", value) for value in pointers_b)
        write_dump(
            self.path_a, 1, base_a,
            [(0, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="uCharacter.motionBlend",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(0, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="uCharacter.motionBlend",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])
        self.assertGreater(report["frame0_pointer_override"]["slot_count"], 0)

    def test_skips_entire_motion_blend_region_across_frames(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytes(range(0x68))
        data_b = bytes(reversed(range(0x68)))
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="uCharacter.motionBlend",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="uCharacter.motionBlend",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_can_disable_frame0_pointer_override(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = struct.pack("<Q", 0x5DFACB0)
        data_b = struct.pack("<Q", 0x5D8ACB0)
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="unknown",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="unknown",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", report["verdict"])

    def test_skips_external_animation_region_across_frames(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0xE0)
        data_b = bytearray(data_a)
        data_a[0x32] = 0x1F
        data_b[0x32] = 0x1B
        data_a[0x92] = 0x89
        data_b[0x92] = 0x85
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0xF119E50, bytes(data_a))],
            version=2, region_name="uCharacter.externalAnimation",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0xF0D9E50, bytes(data_b))],
            version=2, region_name="uCharacter.externalAnimation",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_joint_state_region_across_frames(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytes(range(0xC0))
        data_b = bytes(reversed(range(0xC0)))
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="uCharacter.jointStates",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="uCharacter.jointStates",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_saction_cref_and_unaligned_spill_pointers(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x1800)
        data_b = bytearray(data_a)
        data_a[0x1F0:0x1F8] = struct.pack("<Q", base_a + 0x112478)
        data_b[0x1F0:0x1F8] = struct.pack("<Q", base_b + 0x112478)
        data_a[0x208:0x20C] = struct.pack("<I", 0x1230)
        data_b[0x208:0x20C] = struct.pack("<I", 0x122D)
        data_a[0x163C:0x1644] = struct.pack("<Q", 0x0F3ADFC0)
        data_b[0x163C:0x1644] = struct.pack("<Q", 0x0F36DFC0)
        write_dump(
            self.path_a, 1, base_a,
            [(0, 0x11, 0x22, 0x0D72AAC0, bytes(data_a))],
            version=2, region_name="sAction.full",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(0, 0x22, 0x11, 0x0D6EAAC0, bytes(data_b))],
            version=2, region_name="sAction.full",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_late_camera_pointer(self):
        base_a, base_b = 0x140000000, 0x150000000
        camera_a = bytearray(0x590)
        camera_b = bytearray(camera_a)
        camera_a[0x18:0x20] = struct.pack("<Q", 0x0F1400A0)
        camera_b[0x18:0x20] = struct.pack("<Q", 0x0F1000A0)
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0x0F140000, bytes(camera_a))],
            version=2, region_name="uActionSideCamera",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0x0F100000, bytes(camera_b))],
            version=2, region_name="uActionSideCamera",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_animation_command_and_chunli_spill_pointers(self):
        base_a, base_b = 0x140000000, 0x150000000
        character_a = bytearray(0x6A60)
        character_b = bytearray(character_a)
        character_a[:8] = struct.pack("<Q", base_a + 0xA71F90)
        character_b[:8] = struct.pack("<Q", base_b + 0xA71F90)
        for offset, value_a, value_b in (
                (0x13B0, 0x05F70538, 0x05F10538),
                (0x6A10, 0x04751010, 0x04711010),
                (0x6A20, 0x04751098, 0x04711098)):
            character_a[offset:offset + 8] = struct.pack("<Q", value_a)
            character_b[offset:offset + 8] = struct.pack("<Q", value_b)
        write_dump(
            self.path_a, 1, base_a,
            [(0, 0x11, 0x22, 0x0F750000, bytes(character_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(0, 0x22, 0x11, 0x0F710000, bytes(character_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_chunli_tail_without_frame_zero(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x6A60)
        data_b = bytearray(data_a)
        data_a[:8] = struct.pack("<Q", base_a + 0xA71F90)
        data_b[:8] = struct.pack("<Q", base_b + 0xA71F90)
        data_a[0x693A] = 0xA1
        data_b[0x693A] = 0xB2
        write_dump(
            self.path_a, 7, base_a,
            [(0, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(0, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_keeps_animation_serial_difference(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x1400)
        data_b = bytearray(data_a)
        data_a[0x133C:0x1340] = struct.pack("<I", 0x130)
        data_b[0x133C:0x1340] = struct.pack("<I", 0x12B)
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x133c",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

    def test_skips_character_render_frame_stamp(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x200)
        data_b = bytearray(data_a)
        data_a[0x18C:0x190] = struct.pack("<I", 61)
        data_b[0x18C:0x190] = struct.pack("<I", 66)
        write_dump(
            self.path_a, 1, base_a,
            [(2, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team1",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(2, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team1",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_character_model_live_cache_fields(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x100)
        data_b = bytearray(data_a)
        data_a[0x93] = 0x80
        data_a[0x96:0x98] = bytes.fromhex("0000")
        data_b[0x96:0x98] = bytes.fromhex("803f")
        write_dump(
            self.path_a, 1, base_a,
            [(51, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team1",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(51, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team1",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_character_model_visual_state(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x1300)
        data_b = bytearray(data_a)
        data_a[0x190:0x198] = bytes.fromhex("01000f0001000000")
        data_b[0x190:0x198] = bytes.fromhex("01000f0000000000")
        data_a[0x12D0:0x12DC] = bytes.fromhex("27068140f40a264190c23541")
        data_b[0x12D0:0x12DC] = bytes.fromhex("9c9999400000404100004041")
        write_dump(
            self.path_a, 1, base_a,
            [(51, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(51, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_character_face_and_model_state(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x3F38)
        data_b = bytearray(data_a)
        for offset, size in (
                (0x13D8, 4), (0x3660, 4), (0x3664, 4),
                (0x368C, 4), (0x3EBC, 4), (0x3ED0, 4),
                (0x3ED8, 0x60)):
            data_a[offset:offset + size] = bytes([0xA1] * size)
            data_b[offset:offset + size] = bytes([0xB2] * size)
        write_dump(
            self.path_a, 1, base_a,
            [(0, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(0, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_skips_character_model_resource_pointers(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x548)
        data_b = bytearray(data_a)
        for offset, value_a, value_b in (
                (0x100, 0x05F70538, 0x05F10538),
                (0x110, 0x05F70620, 0x05F10620),
                (0x540, 0x05F70708, 0x05F10708)):
            data_a[offset:offset + 8] = struct.pack("<Q", value_a)
            data_b[offset:offset + 8] = struct.pack("<Q", value_b)
        data_a[0x120] = 0xA1
        data_b[0x120] = 0xB2
        write_dump(
            self.path_a, 1, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x120",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x120] = 0xA1
        write_dump(
            self.path_b, 2, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_motion_resource_bindings_and_list(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x1240)
        data_b = bytearray(data_a)
        for offset in (0x640, 0x660, 0x680, 0x688, 0x690, 0x698,
                       0x6C0, 0x6D0, 0x6D8, 0x11B8, 0x11C0, 0x11D0):
            data_a[offset:offset + 8] = struct.pack("<Q", 0x05F70000 + offset)
            data_b[offset:offset + 8] = struct.pack("<Q", 0x05F10000 + offset)
        data_a[0x620] = 0xA1
        data_b[0x620] = 0xB2
        write_dump(
            self.path_a, 1, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x620",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x620] = 0xA1
        write_dump(
            self.path_b, 2, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_animation_wrapper_pointers_but_keeps_scalars(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x13C0)
        data_b = bytearray(data_a)
        for offset in (0x1360, 0x1368, 0x1370, 0x1380, 0x1390, 0x1398,
                       0x13A0, 0x13A8, 0x13B0):
            data_a[offset:offset + 8] = struct.pack("<Q", 0x05F70000 + offset)
            data_b[offset:offset + 8] = struct.pack("<Q", 0x05F10000 + offset)
        data_a[0x134C] = 0xA1
        data_b[0x134C] = 0xB2
        write_dump(
            self.path_a, 7, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x134c",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x134C] = 0xA1
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_character_constraint_resource_pointers(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x1550)
        data_b = bytearray(data_a)
        for offset in (0x1468, 0x1478, 0x1540):
            data_a[offset:offset + 8] = struct.pack("<Q", 0x05F70000 + offset)
            data_b[offset:offset + 8] = struct.pack("<Q", 0x05F10000 + offset)
        data_a[0x1450] = 0xA1
        data_b[0x1450] = 0xB2
        write_dump(
            self.path_a, 7, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x1450",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x1450] = 0xA1
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_repeated_character_reference_tables(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x3600)
        data_b = bytearray(data_a)
        for offset in (0x1898, 0x28A0, 0x2AB8, 0x2DC8, 0x2DE8, 0x2DF0,
                       0x2DF8, 0x2E00, 0x2F90, 0x2FA0, 0x2FF0, 0x3000,
                       0x3098, 0x34F0, 0x34F8, 0x3500, 0x3560, 0x35A8,
                       0x35B0, 0x35B8, 0x35C0, 0x35D8, 0x35F8):
            data_a[offset:offset + 8] = struct.pack("<Q", 0x05F70000 + offset)
            data_b[offset:offset + 8] = struct.pack("<Q", 0x05F10000 + offset)
        data_a[0x2F88] = 0xA1
        data_b[0x2F88] = 0xB2
        write_dump(
            self.path_a, 7, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x2f88",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x2F88] = 0xA1
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_remaining_pointer_shaped_character_slots(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x6800)
        data_b = bytearray(data_a)
        for offset in (0x3270, 0x3608, 0x3618, 0x3628, 0x3C40, 0x3DB0,
                       0x6460, 0x6740, 0x67F0):
            data_a[offset:offset + 8] = struct.pack("<Q", 0x05F70000 + offset)
            data_b[offset:offset + 8] = struct.pack("<Q", 0x05F10000 + offset)
        data_a[0x3278] = 0xA1
        data_b[0x3278] = 0xB2
        write_dump(
            self.path_a, 7, base_a,
            [(11, 0x11, 0x22, 0x0F750000, bytes(data_a))],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("diverged", report["verdict"])
        self.assertEqual(
            "0x3278",
            report["first_divergence"]["regions"][0]["differences_sample"][0]["offset"],
        )

        data_b[0x3278] = 0xA1
        write_dump(
            self.path_b, 7, base_b,
            [(11, 0x22, 0x11, 0x0F710000, bytes(data_b))],
            version=2, region_name="uCharacter.team0",
        )
        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
        )
        self.assertEqual("match", report["verdict"])

    def test_skips_unrestored_saction_full_spill_byte(self):
        base_a, base_b = 0x140000000, 0x150000000
        data_a = bytearray(0x3635)
        data_b = bytearray(data_a)
        data_a[0x3634] = 0x40
        write_dump(
            self.path_a, 1, base_a,
            [(51, 0x11, 0x22, 0x0D72AAC0, bytes(data_a))],
            version=2, region_name="sAction.full",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(51, 0x22, 0x11, 0x22, bytes(data_b))],
            version=2, region_name="sAction.full",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("match", report["verdict"])

        conservative = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16,
            skip_frame0_pointers=False,
        )
        self.assertEqual("diverged", conservative["verdict"])

    def test_keeps_scalar_difference_despite_pointer_relocations(self):
        base_a, base_b = 0x140000000, 0x150000000
        pointers_a = [0x5DFACB0, 0x5DFACE0, 0x5DFE7D0, 0x5DFAD10]
        pointers_b = [value - 0x70000 for value in pointers_a]
        data_a = b"".join(struct.pack("<Q", value) for value in pointers_a) + b"state-A!"
        data_b = b"".join(struct.pack("<Q", value) for value in pointers_b) + b"state-B!"
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="unknown",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="unknown",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("diverged", report["verdict"])
        differences = report["first_divergence"]["regions"][0]["differences_sample"]
        self.assertIn("0x26", [difference["offset"] for difference in differences])

    def test_does_not_override_tagged_non_pointer_qword(self):
        base_a, base_b = 0x140000000, 0x150000000
        pointers_a = [0x5DFACB0, 0x5DFACE0, 0x5DFE7D0, 0x5DFAD10]
        pointers_b = [value - 0x70000 for value in pointers_a]
        tagged_a = (0x1143560 << 8) | 0x16
        tagged_b = ((0x1143560 - 0x40000) << 8) | 0x16
        data_a = b"".join(struct.pack("<Q", value) for value in pointers_a)
        data_b = b"".join(struct.pack("<Q", value) for value in pointers_b)
        data_a += struct.pack("<Q", tagged_a)
        data_b += struct.pack("<Q", tagged_b)
        write_dump(
            self.path_a, 1, base_a,
            [(10, 0x11, 0x22, 0xF119E50, data_a)],
            version=2, region_name="uCharacter.team0",
        )
        write_dump(
            self.path_b, 2, base_b,
            [(10, 0x22, 0x11, 0xF0D9E50, data_b)],
            version=2, region_name="uCharacter.team0",
        )

        report = compare.build_report(
            compare.load_dump(self.path_a), compare.load_dump(self.path_b), 32, 16
        )
        self.assertEqual("diverged", report["verdict"])


if __name__ == "__main__":
    unittest.main()
