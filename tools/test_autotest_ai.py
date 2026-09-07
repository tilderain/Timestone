#!/usr/bin/env python3

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("autotest_ai.py")
SPEC = importlib.util.spec_from_file_location("autotest_ai", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class AutoTestAiTest(unittest.TestCase):
    def test_artifact_character_name_maps_internal_roster_names(self):
        self.assertEqual("CapAmerica", runner.artifact_character_name("Captain America"))
        self.assertEqual("Naruhodo", runner.artifact_character_name("Phoenix Wright"))
        self.assertEqual("Ryu", runner.artifact_character_name("Ryu"))

    def test_build_plan_includes_autotest_port(self):
        args = SimpleNamespace(
            character="Ryu",
            game=Path("umvc3.exe"),
            game_dir=Path("."),
            repeat=1,
            port=7788,
            wine=None,
            wine_prefix=None,
        )
        plan = runner.build_plan(args, ["12"])
        self.assertIn("-autotest-port=7788", plan["argv"])
        self.assertEqual(7788, plan["port"])

    def test_build_plan_all_moves_omits_move_filter(self):
        args = SimpleNamespace(
            character="Felicia",
            game=Path("umvc3.exe"),
            game_dir=Path("."),
            repeat=1,
            port=None,
            wine=None,
            wine_prefix=None,
        )
        plan = runner.build_plan(args, None)
        self.assertEqual("all", plan["moves"])
        self.assertFalse(any(arg.startswith("-autotest-moves=")
                             for arg in plan["argv"]))

    def test_suite_completed_accepts_completed_host_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "Timestone_host.log"
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

    def test_latest_pair_searches_nested_rollback_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host = root / "rollback_logs/session_host/desync_20260906_120000_host_f42.usdp"
            client = root / "rollback_logs/session_client/desync_20260906_120000_client_f42.usdp"
            for path in (host, client):
                path.parent.mkdir(parents=True)
                path.touch()
            before = min(host.stat().st_mtime, client.stat().st_mtime) - 1
            self.assertEqual((host, client), runner.latest_report_pair(root, "Chris", before))
            os.utime(client, (before - 1, before - 1))
            self.assertIsNone(runner.latest_report_pair(root, "Chris", before))

    def test_rollback_logs_completion_and_crash_discovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = root / "rollback_logs/nplive_new_host"
            session.mkdir(parents=True)
            log = session / "Timestone_host.log"
            log.write_text("AutoTestSuite: command-line suite complete\n")
            before = log.stat().st_mtime - 1
            self.assertTrue(runner.suite_completed(root, before))
            snapshot = runner.crash_artifact_snapshot(root)
            crash = session / "umvc3_crash.log"
            crash.write_text("=== UMVC3Hook crash new run ===\n")
            self.assertEqual([str(crash)], runner.recent_crash_artifacts(root, before, snapshot))
            self.assertIn(str(crash), runner.crash_artifact_snapshot(root))

    def test_validate_suite_rows_accepts_manual_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            autotest = root / "autotest"
            autotest.mkdir()
            path = autotest / "results_commands.tsv"
            path.write_text(
                "character\tsuite_type\tmove_id\tmatcher_id\tline_id\tmove\tresult\n"
                "Naruhodo\tcommands\t1\t-1\t-1\tEvidence Discard\tSYNC\n"
                "Chris\tcommands\t2\t-1\t-1\tFlamethrower 4H\tSYNC\n",
                encoding="utf-8",
            )
            self.assertEqual([], runner.validate_suite_rows(
                root, "Naruhodo", path.stat().st_mtime - 1))
            self.assertEqual([], runner.validate_suite_rows(
                root, "Chris", path.stat().st_mtime - 1))

    def test_validate_suite_rows_accepts_pineapple_surprise_manual_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            autotest = root / "autotest"
            autotest.mkdir()
            path = autotest / "results_commands.tsv"
            path.write_text(
                "character\tsuite_type\tmove_id\tmatcher_id\tline_id\tmove\tresult\n"
                "DeadPool\tcommands\t1\t-1\t-1\tPineapple Surprise\tSYNC\n",
                encoding="utf-8",
            )
            self.assertEqual([], runner.validate_suite_rows(
                root, "Deadpool", path.stat().st_mtime - 1))

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
            log = root / "nplive_old_host" / "umvc3_crash.log"
            log.parent.mkdir()
            log.write_text("=== UMVC3Hook crash old run ===\n", encoding="utf-8")
            snapshot = runner.crash_artifact_snapshot(root)
            self.assertEqual([], runner.recent_crash_artifacts(
                root, log.stat().st_mtime - 1, snapshot))

    def test_recent_crash_artifacts_accepts_new_crash_log(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot = runner.crash_artifact_snapshot(root)
            log = root / "nplive_new_client" / "umvc3_crash.log"
            log.parent.mkdir()
            log.write_text("=== UMVC3Hook crash new run ===\n", encoding="utf-8")
            self.assertEqual([str(log)], runner.recent_crash_artifacts(
                root, log.stat().st_mtime - 1, snapshot))


if __name__ == "__main__":
    unittest.main()
