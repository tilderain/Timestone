import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class AutoTestSelectorsTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_native_move_selectors(self):
        source = (Path(__file__).resolve().parents[1] /
                  "UMVC3Hook/code/autotest.cpp").read_text()
        functions = source[source.index("bool FilterContainsNumber("):
                           source.index("bool IsManualCaseSelected(")]
        contains = source[source.index("bool ContainsInsensitive("):
                          source.index("int32_t RequiredQuietFramesForMove(")]
        characters = re.search(
            r"constexpr const char\* SUITE_MANUAL_SCRIPT_CHARACTERS\[\] = \{.*?\};",
            source, re.S).group()
        harness = r'''
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <strings.h>
#define _stricmp strcasecmp
const char* g_autoTestCharacterName = "Chris";
''' + characters + r'''
int SUITE_MANUAL_SCRIPTS[std::size(SUITE_MANUAL_SCRIPT_CHARACTERS)];
''' + contains + functions + r'''
int main() {
    assert(MoveSelectorMatches("8", 8, "Flamethrower 4H"));
    assert(!MoveSelectorMatches("8", 2, "neutral > 2 > 8"));
    assert(!MoveSelectorMatches("8", 3, "(AIR) 8+S"));
    assert(!MoveSelectorMatches("8", 4, "8"));
    assert(!RuntimeCommandSelectorMatches("8", 0, 7, 8, "Air Dash"));
    assert(RuntimeCommandSelectorMatches("0:8", 0, 8, 9, "(AIR) 4"));
    assert(!RuntimeCommandSelectorMatches("0:8", 0, 18, 8, "0:8"));
    assert(!MoveSelectorMatches("0:8", 8, "Flamethrower 4H"));
    assert(RuntimeCommandSelectorMatches(" 8, 0:28 ", 0, 28, 29, "Grenade"));
    assert(!RuntimeCommandSelectorMatches(" 8, 0:28 ", 0, 7, 8, "Air Dash"));
    assert(MoveSelectorMatches("flamethrower", 8, "Flamethrower 4H"));
    assert(MoveSelectorMatches("", 8, "Flamethrower 4H"));
    g_autoTestCharacterName = "Ryu";
    assert(RuntimeCommandSelectorMatches("8", 0, 7, 8, "Air Dash"));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "selectors.cpp").write_text(harness)
            subprocess.run(["g++", "-std=c++20", str(path / "selectors.cpp"),
                            "-o", str(path / "selectors")], check=True)
            subprocess.run([str(path / "selectors")], check=True)


if __name__ == "__main__":
    unittest.main()
