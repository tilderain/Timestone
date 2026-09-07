"""Test production connection-state helpers with mock Win32 controls."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


MOCKS = r'''
#include <cassert>
#include <cstring>
#include <strings.h>
#include <map>
#include <set>
#include <cstddef>
constexpr int TRUE = 1, FALSE = 0, SW_HIDE = 0, SW_SHOW = 5;
constexpr int GW_CHILD = 1, GW_HWNDNEXT = 2;
struct Control { int id; const char* name; bool enabled = true, visible = true; };
using HWND = Control*;
std::map<int, Control> controls;
Control parent = {-1, "parent"};
bool g_isHost = false, g_peerReady = false, g_battleSetupOnly = false;
int musicUpdates = 0, readyUpdates = 0, pingUpdates = 0;
void HookLog(const char*, ...) {}
void UpdateLobbyMusicCheckbox(HWND) { ++musicUpdates; }
void UpdateReadyButton(HWND) { ++readyUpdates; }
void UpdatePingLabel(HWND) { ++pingUpdates; }
HWND GetDlgItem(HWND window, int id) {
    const auto it = controls.find(id);
    return window && it != controls.end() ? &it->second : nullptr;
}
HWND GetWindow(HWND window, int which) {
    if (!window) return nullptr;
    const auto it = which == GW_CHILD ? controls.begin() : controls.upper_bound(window->id);
    return it != controls.end() ? &it->second : nullptr;
}
int GetClassNameA(HWND window, char* name, int) { std::strcpy(name, window->name); return 1; }
int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
void EnableWindow(HWND window, int enabled) { if (window) window->enabled = enabled; }
void ShowWindow(HWND window, int show) { if (window) window->visible = show == SW_SHOW; }
'''

CASES = r'''
void Add(int id, const char* name) { controls.emplace(id, Control{id, name}); }
int main() {
    for (bool host : {false, true}) for (bool ready : {false, true})
    for (bool battle : {false, true}) for (bool presets : {false, true}) {
        controls.clear();
        for (int i = 0; i < 6; ++i) {
            Add(ID_P1_SLOT1 + i, "COMBOBOX");
            Add(ID_P1_COLOR1 + i, "COMBOBOX");
            Add(ID_P1_ASSIST1 + i, "COMBOBOX");
        }
        for (int id : {ID_P1_SIMPLE_MODE, ID_P2_SIMPLE_MODE, ID_PEER_READY,
                       ID_LAUNCH_TRAINING, ID_LAUNCH_NORMAL, ID_RANDOMIZE_CHARS,
                       ID_NET_HOST_BTN, ID_NET_CONNECT_BTN}) Add(id, "BUTTON");
        if (presets) {
            Add(ID_P1_SAVED_ROSTER, "COMBOBOX");
            Add(ID_P2_SAVED_ROSTER, "COMBOBOX");
        }
        if (battle) Add(ID_BATTLE_LEAVE, "BUTTON");
        Add(ID_STAGE, "COMBOBOX");
        Add(ID_NET_IP, "EDIT");
        Add(ID_PING_LABEL, "STATIC");
        Add(ID_STEAM_LIST, "SysListView32");
        Add(9998, "listbox"); // Case-insensitive class matching.
        g_isHost = host; g_peerReady = ready; g_battleSetupOnly = battle;
        musicUpdates = readyUpdates = pingUpdates = 0;
        ConfigureConnectedControls(&parent);
        const int team = host ? 0 : 1;
        std::set<int> enabled = {ID_RANDOMIZE_CHARS, ID_PING_LABEL, ID_STEAM_LIST};
        for (int i = team * 3; i < team * 3 + 3; ++i) {
            enabled.insert(ID_P1_SLOT1 + i);
            enabled.insert(ID_P1_COLOR1 + i);
            enabled.insert(ID_P1_ASSIST1 + i);
        }
        enabled.insert(host ? ID_P1_SIMPLE_MODE : ID_P2_SIMPLE_MODE);
        if (presets) enabled.insert(host ? ID_P1_SAVED_ROSTER : ID_P2_SAVED_ROSTER);
        if (host) {
            enabled.insert(ID_STAGE);
            if (ready) enabled.insert(ID_LAUNCH_TRAINING);
        } else enabled.insert(ID_PEER_READY);
        if (battle) enabled.insert(ID_BATTLE_LEAVE);
        for (const auto& [id, control] : controls)
            assert(control.enabled == enabled.contains(id));
        assert(musicUpdates == 1 && readyUpdates == 1 && pingUpdates == 1);

        SetTeamControlsEnabled(&parent, team, false);
        assert(!controls.at(ID_P1_SLOT1 + team * 3).enabled);
        controls.at(ID_STEAM_LIST).enabled = false;
        controls.at(ID_PING_LABEL).enabled = false;
        ResetLaunchMenuButtonState(&parent);
        for (const auto& [id, control] : controls) {
            assert(control.enabled == (id != ID_STEAM_LIST && id != ID_PING_LABEL));
            assert(control.visible == (id != ID_PEER_READY && id != ID_PING_LABEL));
        }
        LaunchMenuControls::SetEnabled(&parent, {ID_NET_HOST_BTN, ID_NET_CONNECT_BTN}, false);
        assert(!controls.at(ID_NET_HOST_BTN).enabled && !controls.at(ID_NET_CONNECT_BTN).enabled);
        const int ids[] = {ID_NET_IP, ID_NET_HOST_BTN, ID_NET_CONNECT_BTN, -123};
        LaunchMenuControls::SetVisible(&parent, ids, false);
        assert(!controls.at(ID_NET_IP).visible);
        LaunchMenuControls::SetVisible(&parent, ids, true);
        assert(controls.at(ID_NET_IP).visible && !controls.at(ID_NET_HOST_BTN).enabled);
        ResetLaunchMenuButtonState(nullptr);
    }
}
'''


def function(source, name):
    match = re.search(r"static void " + name + r"\([^;]*?\)\n\{", source)
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


class ControlStateTest(unittest.TestCase):
    def test_host_client_ready_and_disconnect_states(self):
        root = Path(__file__).resolve().parents[1] / "UMVC3Hook/code"
        source = (root / "LaunchMenu.cpp").read_text()
        ids = "\n".join(f"constexpr int {name} = {value};" for name, value in
                        re.findall(r"\b(ID_\w+)\s*=\s*(\d+)\s*,", source))
        header = (root / "launchmenu_controls.h").read_text()
        helpers = header.split("namespace LaunchMenuControls {", 1)[1].split("inline HWND Create(", 1)[0]
        program = MOCKS + ids + "\nnamespace LaunchMenuControls {" + helpers + "}\n"
        for name in ("SetTeamControlsEnabled", "ConfigureConnectedControls", "ResetLaunchMenuButtonState"):
            program += function(source, name)
        with tempfile.TemporaryDirectory(prefix="menu-state-", dir="/tmp/opencode") as temp:
            executable = str(Path(temp) / "test_state")
            subprocess.run(["g++", "-std=c++20", "-O3", "-flto=auto", "-x", "c++", "-", "-o", executable],
                           input=program + CASES, text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
