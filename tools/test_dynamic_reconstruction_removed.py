"""Verify the interim build cannot run the removed reconstruction path."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "UMVC3Hook/code"


class DynamicReconstructionRemovedTest(unittest.TestCase):
    def test_removed_modules_are_not_built_or_included(self):
        modules = ("rollback_shot", "rollback_effect", "rollback_model_policy",
                   "concrete_snapshot")
        build_files = [ROOT / "Makefile", ROOT / "UMVC3Hook/UMVC3Hook.vcxproj",
                       ROOT / "UMVC3Hook/UMVC3Hook.vcxproj.filters"]
        for module in modules:
            for suffix in (".cpp", ".h"):
                self.assertFalse((CODE / (module + suffix)).exists())
            for path in build_files:
                self.assertNotIn(module + ".", path.read_text())
        for path in CODE.glob("*.*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            text = path.read_text()
            for module in modules:
                self.assertNotIn(f'#include "{module}.h"', text, str(path))

    def test_snapshot_has_no_dynamic_recipes_or_manager_rewind(self):
        header = (CODE / "rollback_snapshot.h").read_text()
        capture = (CODE / "rollback_snapshot.cpp").read_text()
        restore = (CODE / "rollback_restore.cpp").read_text()
        for name in ("RollbackShotRecipe", "RollbackEffectRecipe",
                     "shotRecipes", "effectRecipes", "characterShotReferences",
                     "characterEffectReferences", "retainedEffectResources"):
            self.assertNotIn(name, header + capture + restore)
        for name in ("RestoreShotAndEffectRecipes", "RestorePreSpawnRng",
                     "Rollback_ClearGameEffectSlots", "shotSlotFlags", "sShot.idPool"):
            self.assertNotIn(name, capture + restore)

    def test_dump_counts_are_explicitly_empty_and_warning_is_present(self):
        dump = (CODE / "rollback_snapdump.cpp").read_text()
        self.assertIn("destination.shotCount = 0;", dump)
        self.assertIn("destination.effectCount = 0;", dump)
        self.assertNotIn("SerializeShot", dump)
        self.assertIn("shot/effect rewind is unavailable", (CODE / "rollback.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
