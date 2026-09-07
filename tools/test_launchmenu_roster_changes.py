"""Check local roster save/send/readiness ordering without a game connection."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


MOCKS = r'''
#include <cassert>
#include <string>
#include <initializer_list>
#include <cstdint>
using HWND = void*;
using WPARAM = uint32_t;
constexpr int CBN_SELCHANGE = 1, CBN_EDITCHANGE = 5;
int LOWORD(WPARAM value) { return value & 0xffff; }
int HIWORD(WPARAM value) { return value >> 16; }
bool g_isHost, g_localReady;
struct { void* enetPeer; } g_dialogConfig;
std::string events;
void SaveLocalConnectSelection(HWND) { events += 'S'; }
void UpdateReadyButton(HWND) { assert(!g_localReady); events += 'R'; }
void SendP1Selection(HWND) { assert(g_isHost); events += '1'; }
void SendP2Selection(HWND) { assert(!g_isHost && !g_localReady); events += '2'; }
void UpdateSlotOptionsFromCombos(HWND) { events += 'U'; }
'''

CASES = r'''
int main() {
    int peer;
    for (bool host : {false, true}) for (bool connected : {false, true})
    for (bool ready : {false, true}) for (int team : {0, 1}) {
        g_isHost = host;
        g_dialogConfig.enetPeer = connected ? &peer : nullptr;
        const bool local = team == (host ? 0 : 1);
        const std::string send = local && connected ? (host ? "1" : "R2") : "";
        const bool expectedReady = ready && !(local && connected && !host);

        events.clear(); g_localReady = ready;
        OnLocalRosterChanged(nullptr, team);
        assert(events == (local ? "S" : "") + send);
        assert(g_localReady == expectedReady);

        events.clear(); g_localReady = ready;
        SendLocalRosterChange(nullptr, team);
        assert(events == send); // Sending must not itself save a selection.
        assert(g_localReady == expectedReady);

        for (int slot = 0; slot < 3; ++slot) {
            for (int notification : {CBN_SELCHANGE, CBN_EDITCHANGE, 99}) {
                for (bool edited : {false, true}) {
                    events.clear(); g_localReady = ready;
                    CharacterEffects(nullptr,
                        (notification << 16) | (ID_P1_SLOT1 + team * 3 + slot), edited);
                    const bool sync = notification == CBN_SELCHANGE;
                    assert(events == (local && edited ? "S" : "") + (sync ? send : ""));
                    assert(g_localReady == (sync ? expectedReady : ready));
                }
                for (int first : {ID_P1_COLOR1, ID_P1_ASSIST1}) {
                    events.clear(); g_localReady = ready;
                    OptionEffects(nullptr, (notification << 16) | (first + team * 3 + slot));
                    assert(events == (notification == CBN_SELCHANGE
                        ? std::string("U") + (local ? "S" : "") + send : ""));
                    assert(g_localReady == (notification == CBN_SELCHANGE ? expectedReady : ready));
                }
            }
        }
    }
}
'''


def function(source, name):
    match = re.search(r"static void " + name + r"\([^;]*?\)\n\{", source)
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


class RosterChangeTest(unittest.TestCase):
    def test_local_ownership_and_notification_effects(self):
        root = Path(__file__).resolve().parents[1] / "UMVC3Hook/code"
        source = (root / "LaunchMenu.cpp").read_text()
        ids = "\n".join(f"constexpr int {name} = {value};" for name, value in
                        re.findall(r"\b(ID_\w+)\s*=\s*(\d+)\s*,", source))
        program = MOCKS + ids + "\n"
        for name in ("SendLocalRosterChange", "OnLocalRosterChanged"):
            program += function(source, name)
        # Test the actual command-side notification gates and team-ID mapping.
        chars = source.split("case ID_P1_SLOT1: case ID_P1_SLOT2:", 1)[1]
        effects = chars[chars.index("const int team ="):].split("return 0;", 1)[0]
        program += "void CharacterEffects(HWND hDlg, WPARAM wParam, bool rosterEdited) {\n" + effects + "}\n"
        options = source.split("case ID_P1_COLOR1: case ID_P1_COLOR2:", 1)[1]
        effects = options[options.index("if (HIWORD(wParam)"):].split("return 0;", 1)[0]
        program += "void OptionEffects(HWND hDlg, WPARAM wParam) {\n" + effects + "}\n"
        # Received selections must never use the local edit helpers.
        received = source.split("static bool ApplyP1Selection(HWND hDlg, const LaunchP1SelectionPacket& packet)\n", 1)[1]
        received = received.split("// Public API", 1)[0]
        self.assertNotIn("OnLocalRosterChanged(", received)
        self.assertNotIn("SendLocalRosterChange(", received)
        self.assertNotIn("SaveLocalConnectSelection(", received)
        with tempfile.TemporaryDirectory(prefix="roster-changes-", dir="/tmp/opencode") as temp:
            executable = str(Path(temp) / "test_roster_changes")
            subprocess.run(["g++", "-std=c++20", "-O3", "-flto=auto", "-x", "c++", "-", "-o", executable],
                           input=program + CASES, text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
