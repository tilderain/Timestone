#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("audit_character_tail_ranges.py")
SPEC = importlib.util.spec_from_file_location("audit_character_tail_ranges", MODULE_PATH)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class CharacterTailAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[1]
        cls.report = audit.build_audit(cls.root)

    def test_static_policy_has_no_errors(self):
        self.assertEqual([], self.report["errors"])

    def test_audits_every_generated_character_dti(self):
        generated, _ = audit.parse_generated(
            self.root / "UMVC3Hook/code/generated_character_tail_fields.inc"
        )
        self.assertEqual(len({item.dti for item in generated}), self.report["character_count"])

    def test_arthur_cross_sword_latch_is_covered(self):
        generated, _ = audit.parse_generated(
            self.root / "UMVC3Hook/code/generated_character_tail_fields.inc"
        )
        arthur = [item for item in generated if item.dti == 0x140D44E48]
        self.assertTrue(audit.covers(arthur, 0x68F9, 1))

    def test_manual_zero_rva_layouts_are_audited(self):
        records = {record["dti"]: record for record in self.report["records"]}
        for dti in audit.MANUAL_GHIDRA_LAYOUTS:
            self.assertNotIn("schema_warning", records[dti])
            self.assertEqual([], records[dti]["uncovered_fields"])

    def test_new_pointer_owners_are_explicitly_documented(self):
        documented = audit.parse_doc_table(
            self.root / "doc/chrfields.md",
            "## Unsafe References And Owners",
            "## Generated Range Conflicts",
            "chrfields.md",
        )
        expected = {
            0x140D44E48: (0x68F0, 8),
            0x140D44F60: (0x68F0, 8),
            0x140D44FA0: (0x68F0, 8),
            0x140D45138: (0x68F0, 8),
            0x140D45748: (0x68F0, 8),
        }
        for dti, (offset, size) in expected.items():
            ranges = [item for item in documented if item.dti == dti]
            self.assertTrue(audit.covers(ranges, offset, size), hex(dti))

    def test_cviper_charge_timer_is_verified(self):
        verified = audit.parse_cpp_ranges(
            self.root / "UMVC3Hook/code/concrete_snapshot.cpp",
            "kVerifiedCharacterScalarRanges",
        )
        self.assertTrue(audit.covers(verified, 0x6908, 4))
        self.assertTrue(audit.covers(
            [item for item in verified if item.dti == 0x140D44F60], 0x6908, 4
        ))


if __name__ == "__main__":
    unittest.main()
