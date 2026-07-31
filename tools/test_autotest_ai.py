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
            log.write_text("AutoTestSuite: command-line suite complete\n", encoding="utf-8")
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
            self.assertIsNone(runner.latest_report_pair(root, cutoff))

    def test_latest_pair_accepts_fresh_matching_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / "desync_20260731_120000_host_f42.usdp"
            client = root / "desync_20260731_120003_client_f42.usdp"
            host.touch()
            client.touch()
            before = min(host.stat().st_mtime, client.stat().st_mtime) - 1
            self.assertEqual((host, client), runner.latest_report_pair(root, before))


if __name__ == "__main__":
    unittest.main()
