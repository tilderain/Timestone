import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from analyze_attachment_outliers import analyze


class AttachmentOutlierAnalysisTest(unittest.TestCase):
    def test_old_and_current_labels_and_enclosing_record(self):
        records = [
            {"start_frame": 2999, "end_frame": 3006, "hooks": [
                {"name": "kirigakure_effect_update", "calls": 79,
                 "total_ms": 0.4899, "max_us": 377.8}]},
            {"start_frame": 3006, "end_frame": 3013, "total_ms": 17.053,
             "headless_tick": {"unit_sync_max_ms": 3.4311},
             "hooks": [{"name": "game_effect_attachment_update", "calls": 65,
                        "total_ms": 3.1469, "max_us": 3037.5}],
             "unit_objects": [{"name": "uGameEffect", "max_us": 3039.9}]},
        ]
        with tempfile.TemporaryDirectory(prefix="attachment-analysis-test-") as temporary:
            path = Path(temporary) / "profile.json"
            path.write_text(json.dumps(records))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                analyze(path, 1)
        text = output.getvalue()
        self.assertIn("windows=2 calls=144 total_ms=3.636800", text)
        self.assertIn('"start_frame": 3006', text)
        self.assertIn('"max_us": 3039.9', text)
        self.assertIn('"unit_sync_max_ms": 3.4311', text)
        self.assertIn("neighbor 2999 3006", text)

    def test_no_attachment_samples(self):
        with tempfile.TemporaryDirectory(prefix="attachment-analysis-test-") as temporary:
            path = Path(temporary) / "profile.json"
            path.write_text("[]")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                analyze(path, 1)
        self.assertIn("No attachment samples", output.getvalue())


if __name__ == "__main__":
    unittest.main()
