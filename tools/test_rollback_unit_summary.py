import tempfile
import unittest
from pathlib import Path

from tools.rollback_unit_summary import summarize


class LifetimeSummaryTests(unittest.TestCase):
    def test_restore_retire_collect_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.log"
            events = [
                ("quarantine_hold", 7, 0, 2),
                ("retiring_state_restored", 7, 1, 13),
                ("quarantine_hold", 7, 0, 2),
                ("quarantine_collect", 7, 0, 0),
                ("game_effect_base_begin", 7, 0, 0),
            ]
            text = "".join(
                f"LIFE seq={i} event={event} id={identity} b={b} c={c} d=2\n"
                for i, (event, identity, b, c) in enumerate(events, 1))
            path.write_text(text + "FrameStepper: training frame limit reached\n")
            result = summarize(path)
            self.assertTrue(result["completed"])
            self.assertTrue(result["complete_trace_prefix"])
            self.assertEqual(result["populated_holds"], 2)
            self.assertEqual(result["restored_from_hold"], 1)
            self.assertEqual(result["active_restores"], 1)
            self.assertEqual(result["active_resurrections"], 1)
            self.assertEqual(result["remaining_held"], [])
            self.assertEqual(result["accounting_errors"], [])
            path.write_text(text + "LIFE seq=7 event=quarantine_collect id=7 b=0 c=0\n")
            result = summarize(path)
            self.assertEqual(result["sequence_gaps"], 1)
            self.assertTrue(result["accounting_errors"])

    def test_shot_and_effect_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.log"
            events = [
                ("quarantine_hold", 1, "shot"),
                ("quarantine_hold", 2, "effect"),
                ("quarantine_collect", 1, "shot"),
                ("shot_base_begin", 1, "shot"),
                ("quarantine_collect", 2, "effect"),
                ("game_effect_base_begin", 2, "effect"),
                ("quarantine_hold", 3, "unit"),
                ("quarantine_collect", 3, "unit"),
                ("unit_dispose_begin", 3, "unit"),
            ]
            text = "".join(
                f"LIFE seq={i} event={event} id={identity} kind={kind} b=0 c=0\n"
                for i, (event, identity, kind) in enumerate(events, 1))
            path.write_text(text)
            result = summarize(path)
            self.assertEqual(result["held_by_kind"], {"shot": 1, "effect": 1, "unit": 1})
            self.assertEqual(result["collected_by_kind"], result["held_by_kind"])
            self.assertEqual(result["remaining_held"], [])
            self.assertEqual(result["accounting_errors"], [])
            path.write_text(text.replace("event=shot_base_begin", "event=missing_teardown"))
            self.assertEqual(summarize(path)["accounting_errors"],
                             ["collected identity 1 has 0 base teardowns"])
