"""Compile production deferred-action dispatch and input waits with game stubs."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from test_netplay_direct import ROOT, CONTROLLER, function


class RemoteActionTests(unittest.TestCase):
    def test_actions_interrupt_waits_and_preserve_capture(self):
        stepper = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        state = stepper[stepper.index("static RestartMatchPacket g_remoteRestart"):
                        stepper.index('extern "C" void Controller_OnFrameStart();')]
        apply = function(stepper[stepper.index("static void ApplyRemoteActions()\n{"):],
                         "ApplyRemoteActions")
        waits = function(CONTROLLER, "WaitForConfirmedFrame") + "\n" + function(
            CONTROLLER, "WaitForPredictionBudgetFrame")
        # Compile the actual post-controller guard too: a yielded frame must not
        # enter either the prediction-popup oMove path or normal simulation.
        guard_start = stepper.index("            // A receive inside an input wait")
        guard = stepper[guard_start:stepper.index(
            "            if (Controller_IsPredictionBudgetExhausted())", guard_start)]
        mocks = r'''
#include "netplay.h"
#include <algorithm>
#include <cassert>
int g_netFrameCount = 100;
uint32_t g_battleGeneration = 1;
bool g_remoteReady, g_remoteRelease, g_remoteFighting;
bool g_remoteOpeningReady, g_remoteOpeningRelease, g_roundSeedReady;
uint32_t g_remoteOpeningMember, g_roundSeed;
bool g_desyncDetected, g_desyncSeen;
int g_desyncFrame, g_restartTargetFrame, g_restartButtonMode;
uint32_t g_desyncLocalCS, g_desyncRemoteCS, g_restartRoundSeed, g_restartKind;
bool active = true, confirmed = false, exhausted = false;
bool synchronized = true, restarting = false, autotest = false;
int captures, capturedFrame, closed, presents, moves, pumps;
uint32_t incomingType;
DesyncPacket incomingDesync;
void HookLog(const char*, ...) {}
int Controller_GetNetFrameCount() { return g_netFrameCount; }
void Controller_ScheduleDelayFrames(int, int) {}
void Controller_ScheduleRollbackFrames(int, int) {}
void Controller_SetMultiResimEnabled(bool) {}
bool Netplay_IsHost() { return false; }
bool Netplay_IsActive() { return active; }
void Netplay_SetTimeout(uint32_t) {}
void AutoTest_PrepareForRestart() {}
bool AutoTest_IsActive() { return autotest; }
bool IsPastIntroStep() { return true; }
bool FrameStepper_IsBattleSynchronized() { return synchronized; }
bool FrameStepper_IsMatchRestartPending() { return restarting; }
void Rollback_ClearPendingReSimulation() {}
void Rollback_NotifyDesyncTriggered(int frame) { ++captures; capturedFrame = frame; }
void UnpauseFrameStepper() {}
void CloseGame() { ++closed; }
bool IsFrameConfirmed(int) { return confirmed; }
void Rollback_FreezeWatchdogHeartbeat(const char*, int) {}
void SetPredictionBudgetExhausted() { exhausted = true; }
void ClearPredictionBudgetExhausted() { exhausted = false; }
bool Controller_ShouldShowPredictionBudgetExhausted() { return false; }
void SwitchToThread() {}
void PresentFrozenFrame() { ++presents; }
void Netplay_Pump() {
    assert(++pumps == 1); // A deferred action must release the wait immediately.
    if (incomingType == PACKET_SNAPDUMP) {
        SnapDumpPacket dump = {};
        FrameStepper_ReceiveNetplayControl(incomingType, &dump);
    } else {
        FrameStepper_ReceiveNetplayControl(incomingType, &incomingDesync);
    }
}
'''
        cases = r'''
int main() {
    for (bool prediction : {false, true}) {
        for (uint32_t type : {PACKET_DESYNC, PACKET_SNAPDUMP, PACKET_AUTOTEST_COMPLETE}) {
            pumps = 0;
            incomingType = type;
            incomingDesync = {};
            incomingDesync.battle_generation = g_battleGeneration;
            incomingDesync.action = DESYNC_TRIGGER;
            incomingDesync.frame = 80;
            assert(!(prediction ? WaitForPredictionBudgetFrame(100) :
                                  WaitForConfirmedFrame(100, "test")));
            assert(FrameStepper_HasPendingRemoteAction());
            int before = presents;
            FinishFrame();
            assert(presents == before + 1 && moves == 0);
            ApplyRemoteActions();
            assert(!FrameStepper_HasPendingRemoteAction());
            assert(!exhausted);
        }
    }
    assert(closed == 2);
    DesyncPacket trigger = {};
    trigger.battle_generation = g_battleGeneration;
    trigger.action = DESYNC_TRIGGER;
    trigger.frame = 42;
    DesyncPacket unpause = trigger;
    unpause.action = DESYNC_UNPAUSE;
    captures = 0;
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &trigger);
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &unpause);
    assert(captures == 0); // No diagnostic mutation inside a wait callback.
    ApplyRemoteActions();
    assert(captures == 1 && capturedFrame == 42);
    assert(!g_desyncDetected && g_desyncSeen);
    ApplyRemoteActions();
    assert(captures == 1);
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &unpause);
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &trigger);
    ApplyRemoteActions();
    assert(captures == 2 && g_desyncDetected && !g_desyncSeen);
    // A stale generation must not overwrite a current trigger's final state.
    --unpause.battle_generation;
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &trigger);
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &unpause);
    ApplyRemoteActions();
    assert(captures == 3 && g_desyncDetected);
    synchronized = false;
    FrameStepper_ReceiveNetplayControl(PACKET_DESYNC, &trigger);
    ApplyRemoteActions();
    assert(captures == 3 && !FrameStepper_HasPendingRemoteAction());
}
'''
        source = mocks + state + apply + waits + "\nvoid FinishFrame() {\n" + guard + "\n++moves;\n}\n" + cases
        with tempfile.TemporaryDirectory(prefix="netplay-actions-", dir="/tmp/opencode") as tmp:
            tmp = Path(tmp)
            (tmp / "test.cpp").write_text(source)
            subprocess.run(["g++", "-std=c++20", "-O2",
                            *os.environ.get("NETPLAY_TEST_FLAGS", "").split(),
                            "-I" + str(ROOT / "UMVC3Hook/code"),
                            "-I" + str(ROOT / "enet-1.3.18/include"),
                            str(tmp / "test.cpp"), "-o", str(tmp / "test")], check=True)
            subprocess.run([str(tmp / "test")], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
