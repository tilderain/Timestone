#!/usr/bin/env python3

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("autotest_ai.py")
SPEC = importlib.util.spec_from_file_location("autotest_ai", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class AutoTestAiTest(unittest.TestCase):
    def test_suite_completed_accepts_completed_host_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "UMVC3Hook_host.log"
            log.write_text("AutoTestSuite: Suite complete: 1 passed, 0 failed\n",
                           encoding="utf-8")
            self.assertTrue(runner.suite_completed(root, log.stat().st_mtime - 1))

    def test_failed_moves_selects_unique_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "results.tsv"
            path.write_text(
                "move\tresult\nHadoken\tSYNC\nShinku Hadoken\tDESYNC\n"
                "Shinku Hadoken\tDESYNC\nTatsu\tERROR\n",
                encoding="utf-8",
            )
            self.assertEqual(["Shinku Hadoken", "Tatsu"], runner.failed_moves(path))

    def test_latest_pair_rejects_stale_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / "desync_20260731_120000_host_f42.usdp"
            client = root / "desync_20260731_120001_client_f42.usdp"
            host.touch()
            client.touch()
            cutoff = max(host.stat().st_mtime, client.stat().st_mtime) + 1
            self.assertIsNone(runner.latest_report_pair(root, "Ryu", cutoff))

    def test_latest_pair_accepts_fresh_matching_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / "desync_20260731_120000_host_f42.usdp"
            client = root / "desync_20260731_120003_client_f42.usdp"
            host.touch()
            client.touch()
            before = min(host.stat().st_mtime, client.stat().st_mtime) - 1
            self.assertEqual((host, client), runner.latest_report_pair(root, "Ryu", before))

    def test_crash_artifact_details_extracts_fault(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "umvc3_crash.log"
            path.write_text(
                "commit=abc (dirty)\n"
                "code=0xC0000005 exception=0x0 thread=7\n"
                "RIP=0x0000000000000000 RSP=0x1 RBP=0x2\n"
                "access=execute address=0x0000000000000000\n",
                encoding="utf-8",
            )
            self.assertEqual(
                {
                    "path": str(path),
                    "commit": "abc (dirty)",
                    "code": "0xC0000005",
                    "rip": "0x0000000000000000",
                    "access": "execute",
                    "access_address": "0x0000000000000000",
                },
                runner.crash_artifact_details(path),
            )

    def test_recent_crash_artifacts_ignores_unchanged_old_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "nplive_old" / "umvc3_crash.log"
            log.parent.mkdir()
            log.write_text("=== UMVC3Hook crash old run ===\n", encoding="utf-8")
            snapshot = runner.crash_artifact_snapshot(root)
            self.assertEqual([], runner.recent_crash_artifacts(
                root, log.stat().st_mtime - 1, snapshot))

    def test_recent_crash_artifacts_accepts_new_crash_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot = runner.crash_artifact_snapshot(root)
            log = root / "nplive_new" / "umvc3_crash.log"
            log.parent.mkdir()
            log.write_text("=== UMVC3Hook crash new run ===\n", encoding="utf-8")
            self.assertEqual([str(log)], runner.recent_crash_artifacts(
                root, log.stat().st_mtime - 1, snapshot))


if __name__ == "__main__":
    unittest.main()
