#!/usr/bin/env python3

import importlib.util
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("rollback_profile_ai.py")
SPEC = importlib.util.spec_from_file_location("rollback_profile_ai", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def rollback_entry(frame_count, total_ms, max_frame_ms=1.0):
    return {
        "type": "rollback",
        "frame_count": frame_count,
        "total_ms": total_ms,
        "avg_frame_ms": total_ms / frame_count,
        "max_frame_ms": max_frame_ms,
        "multi_resim_enabled": True,
        "phases_ms": {"native_tick": total_ms / 2, "native_tick_max": max_frame_ms},
        "headless_tick": {"total_ms": total_ms / 3, "max_ms": max_frame_ms},
        "snapshot": {
            "count": frame_count,
            "total_ms": frame_count * 0.25,
            "avg_ms": 0.25,
            "avg_kib": 100 + frame_count,
            "avg_regions": 50,
        },
        "restore": {"total_ms": 0.5},
        "shot_restore": {"current_shots": 2},
        "unit_categories": [],
        "unit_objects": [],
        "hooks": [],
    }


class RollbackProfileAiTest(unittest.TestCase):
    def test_summary_weights_cost_by_replayed_frames(self):
        summary = runner.summarize_profile(
            [rollback_entry(2, 10.0, 6.0), rollback_entry(8, 16.0, 3.0)],
            "host",
        )
        self.assertEqual(2, summary["rollback_windows"])
        self.assertEqual(10, summary["replayed_frames"])
        self.assertAlmostEqual(2.6, summary["ms_per_replayed_frame"])
        self.assertAlmostEqual(5.0, summary["average_depth"])
        self.assertEqual(6.0, summary["headless_tick"]["max_ms"])
        self.assertAlmostEqual(0.25, summary["snapshot"]["avg_ms"])
        self.assertAlmostEqual(106.8, summary["snapshot"]["avg_kib"])

    def test_recent_profiles_collects_all_fresh_generations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before = runner.artifact_snapshot(root)
            started = time.time() - 1
            host = root / "nplive_20260822_120000_host" / "rollback_profile.json"
            host_r1 = root / "nplive_20260822_120100_host_r1" / "rollback_profile.json"
            client = root / "nplive_20260822_120000_client" / "rollback_profile.json"
            for path in (host, host_r1, client):
                path.parent.mkdir()
                path.write_text("[]\n", encoding="utf-8")
            profiles = runner.recent_profiles(root, started, before)
            self.assertEqual({host, host_r1}, set(profiles["host"]))
            self.assertEqual([client], profiles["client"])

    def test_recent_desyncs_rejects_stale_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stale = root / "desync_old_host_f1.usdp"
            stale.touch()
            before = runner.artifact_snapshot(root)
            fresh = root / "desync_new_host_f2.usdp"
            fresh.touch()
            self.assertEqual(
                [str(fresh)], runner.recent_desyncs(root, time.time() - 1, before))

    def test_build_plan_uses_cpu_and_frame_limit_options(self):
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(
                game_dir=Path(directory),
                game=Path("umvc3.exe"),
                wine="wine",
                frames=2000,
                output_root=Path(directory) / "runs",
                label="candidate one",
                wine_prefix=None,
            )
            plan = runner.build_run_plan(args)
            self.assertIn("-training-cpus", plan["argv"])
            self.assertIn("-training-frame-limit=2000", plan["argv"])
            self.assertTrue(plan["run_dir"].endswith("_candidate_one"))

    def test_compare_reports_lower_cost_as_improvement(self):
        def run(label, cost):
            side = runner.summarize_profile(
                [rollback_entry(10, cost) for _ in range(30)], "host")
            client = dict(side)
            client["side"] = "client"
            sides = {"host": side, "client": client}
            return {
                "run_id": label,
                "label": label,
                "run_dir": label,
                "frames": 2000,
                "sides": sides,
                "combined": runner.combined_summary(sides),
            }

        comparison = runner.compare_runs(run("baseline", 20.0),
                                         run("candidate", 15.0))
        self.assertTrue(comparison["comparable"])
        metric = comparison["combined_ms_per_replayed_frame"]
        self.assertAlmostEqual(25.0, metric["improvement_percent"])
        headless = comparison["sides"]["host"]["headless_tick"]
        self.assertAlmostEqual(
            25.0, headless["total_ms_per_replayed_frame"]["improvement_percent"])


if __name__ == "__main__":
    unittest.main()
