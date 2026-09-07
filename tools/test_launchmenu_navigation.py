"""Exercise the production launch-menu router with scripted window outcomes."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


MOCKS_AND_CASES = r'''
#include <cassert>
#include <initializer_list>
#include <vector>
#include <cstddef>
constexpr int MAX_PATH = 260;
bool disclaimer = true, regular = false;
int fadeCount = 0;
struct Step { MenuAction window; MenuResult result; };
std::vector<Step> steps;
size_t cursor = 0;
void HookLog(const char*, ...) {}
void LaunchMusic_BeginFadeOut() { ++fadeCount; }
bool ConfirmLaunchDisclaimer() { return disclaimer; }
int GetPrivateProfileIntA(const char*, const char*, int, const char*) { return regular; }
namespace LaunchMenuUtils { void GetSettingsPath(char*, size_t) {} }
MenuResult NextWindow(MenuAction window) {
    assert(cursor < steps.size());
    const auto step = steps[cursor++];
    assert(step.window == window);
    return step.result;
}
MenuResult ShowCompactLaunchMenu() { return NextWindow(MenuAction::OpenCompact); }
MenuResult ShowRegularLaunchMenu(bool battle) {
    return NextWindow(battle ? MenuAction::OpenBattleSetup : MenuAction::OpenAdvanced);
}
LaunchOption ShowLaunchMenu();
void Check(bool startRegular, std::initializer_list<Step> script, LaunchOption expected) {
    regular = startRegular;
    steps = script;
    cursor = 0;
    const int previousFade = fadeCount;
    assert(ShowLaunchMenu() == expected);
    assert(cursor == steps.size());
    assert(fadeCount == previousFade + 1);
}
int main() {
    using A = MenuAction;
    for (bool startRegular : {false, true}) {
        const A start = startRegular ? A::OpenAdvanced : A::OpenCompact;
        for (LaunchOption launch : {LAUNCH_NORMAL, LAUNCH_TRAINING, LAUNCH_DUAL_TRAINING})
            Check(startRegular, {{start, {A::Launch, launch}}}, launch);
        // Cancel must not accidentally use a stale launch option.
        Check(startRegular, {{start, {A::Cancel, LAUNCH_NORMAL}}}, LAUNCH_CANCELLED);
    }
    Check(false, {
        {A::OpenCompact, {A::OpenAdvanced, LAUNCH_CANCELLED}},
        {A::OpenAdvanced, {A::OpenCompact, LAUNCH_CANCELLED}},
        {A::OpenCompact, {A::OpenAdvanced, LAUNCH_CANCELLED}},
        {A::OpenAdvanced, {A::Launch, LAUNCH_DUAL_TRAINING}},
    }, LAUNCH_DUAL_TRAINING);
    Check(true, {
        {A::OpenAdvanced, {A::OpenCompact, LAUNCH_CANCELLED}},
        {A::OpenCompact, {A::OpenBattleSetup, LAUNCH_CANCELLED}},
        {A::OpenBattleSetup, {A::OpenCompact, LAUNCH_CANCELLED}},
        {A::OpenCompact, {A::OpenBattleSetup, LAUNCH_CANCELLED}},
        {A::OpenBattleSetup, {A::Launch, LAUNCH_TRAINING}},
    }, LAUNCH_TRAINING);
    Check(false, {
        {A::OpenCompact, {A::OpenBattleSetup, LAUNCH_NORMAL}},
        {A::OpenBattleSetup, {A::OpenCompact, LAUNCH_CANCELLED}},
        {A::OpenCompact, {A::Cancel, LAUNCH_NORMAL}},
    }, LAUNCH_CANCELLED);
    disclaimer = false;
    Check(false, {}, LAUNCH_CANCELLED);
    // Only transitions out of compact preserve the Steam session.
    for (A action : {A::Cancel, A::Launch, A::OpenCompact, A::OpenAdvanced, A::OpenBattleSetup}) {
        assert(!PreserveSteamForMenuTransition(false, action));
        assert(PreserveSteamForMenuTransition(true, action) ==
               (action == A::OpenAdvanced || action == A::OpenBattleSetup));
    }
}
'''


class NavigationTest(unittest.TestCase):
    def test_router_and_transition_cleanup_policy(self):
        root = Path(__file__).resolve().parents[1] / "UMVC3Hook/code"
        public_header = (root / "LaunchMenu.h").read_text()
        launch_option = re.search(r"enum LaunchOption\s*\{.*?\};", public_header, re.S).group()
        navigation = (root / "launchmenu_navigation.h").read_text()
        navigation = "\n".join(line for line in navigation.splitlines()
                               if not line.startswith("#"))
        source = (root / "LaunchMenu_compact.cpp").read_text()
        router = source[source.index("LaunchOption ShowLaunchMenu()\n"):]
        with tempfile.TemporaryDirectory(prefix="menu-navigation-", dir="/tmp/opencode") as temp:
            executable = str(Path(temp) / "test_navigation")
            subprocess.run(
                ["g++", "-std=c++20", "-O3", "-flto=auto", "-x", "c++", "-", "-o", executable],
                input=launch_option + navigation + MOCKS_AND_CASES + router,
                text=True, check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
