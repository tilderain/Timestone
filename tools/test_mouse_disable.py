"""Verify native mouse neutralization without touching ownership/configuration."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MouseDisableTests(unittest.TestCase):
    def test_neutral_state(self):
        source = (ROOT / "UMVC3Hook/code/mouse_disable.cpp").read_text()
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        unit = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define __fastcall
''' + source + r'''
int main() {
    alignas(8) uint8_t mouse[0xD0];
    memset(mouse, 0xAB, sizeof(mouse));
    MouseDisable_Move((uint64_t)mouse);
    for (unsigned i = 0; i < sizeof(mouse); ++i) {
        uint8_t expected = 0xAB;
        if (i >= 0x38 && i < 0x40) expected = 0xFF;
        if (i >= 0x44 && i < 0xA8) expected = 0;
        assert(mouse[i] == expected);
    }
    uint8_t first[sizeof(mouse)];
    memcpy(first, mouse, sizeof(mouse));
    MouseDisable_Move((uint64_t)mouse);
    assert(!memcmp(first, mouse, sizeof(mouse)));
}
'''
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(unit)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_hook_installed_without_timing(self):
        source = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        main = source[source.index("void Rollback_InitMainHooks("):]
        self.assertEqual(source.count("EnableHook(0x1406aa990"), 1)
        self.assertIn("(void*)&MouseDisable_Move", main)
        self.assertNotIn("LAG_BACKEND_TIMING_ENABLED", source)
        self.assertNotIn("MousePoll_", source)


if __name__ == "__main__":
    unittest.main()
