import tempfile
import unittest
from pathlib import Path

from tools.lifetime_trace_summary import summarize


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
                f"LIFE seq={i} event={event} id={identity} b={b} c={c}\n"
                for i, (event, identity, b, c) in enumerate(events, 1))
            path.write_text(text + "FrameStepper: training frame limit reached\n")
            result = summarize(path)
            self.assertTrue(result["completed"])
            self.assertTrue(result["complete_trace_prefix"])
            self.assertEqual(result["populated_holds"], 2)
            self.assertEqual(result["restored_from_hold"], 1)
            self.assertEqual(result["remaining_held"], [])
            self.assertEqual(result["accounting_errors"], [])
            path.write_text(text + "LIFE seq=7 event=quarantine_collect id=7 b=0 c=0\n")
            result = summarize(path)
            self.assertEqual(result["sequence_gaps"], 1)
            self.assertTrue(result["accounting_errors"])
