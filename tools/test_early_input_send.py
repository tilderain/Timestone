"""Exercise production pre-render input scheduling and outbound mailbox publication."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.test_netplay_stats import block

ROOT = Path(__file__).resolve().parents[1]


class EarlyInputSendTests(unittest.TestCase):
    def run_cpp(self, code):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(code)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_schedule_and_checksum_boundaries(self):
        source = (ROOT / "UMVC3Hook/code/controller.cpp").read_text()
        functions = block(source, "static void PublishFrameInput(bool frameComplete)")
        functions += block(source, 'extern "C" void Controller_PublishInputBeforeRender()')
        packet = block((ROOT / "UMVC3Hook/code/netplay.h").read_text(), "struct InputPacket") + ";"
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
constexpr size_t NET_HISTORY_SIZE=16;
constexpr int MAX_HISTORY=16, RING_SIZE=256;
constexpr int DESYNC_SKIP_FRAMES=10;
constexpr bool INPUT_PROVENANCE_TRACE=false;
''' + packet + r'''
bool active=true, synced=true, restart=false, checksums=true, g_isResimulating=false;
bool g_preRenderInputReady=false;
int32_t g_netFrameCount=100, g_delayFrames=3, g_rollbackFrames=7, g_localFrameAdvantage=-2;
uint32_t generation=1, g_localPending=0x1234;
uint32_t g_scheduledLocalInput[RING_SIZE]{}, g_recordedLocalInput[RING_SIZE]{};
uint32_t g_usedRemoteInput[RING_SIZE]{}, g_oldInputs[MAX_HISTORY]{};
int32_t g_scheduledLocalFrame[RING_SIZE], g_localChecksumFrame[RING_SIZE], g_localChecksumAdvertisedFrame[RING_SIZE];
bool g_localChecksumConfirmed[RING_SIZE]{};
uint32_t g_localChecksum[RING_SIZE]{};
int checksumCalls=0, snapshotCalls=0, autotestCalls=0;
std::vector<InputPacket> sent;
bool Netplay_IsActive() { return active; }
bool FrameStepper_IsBattleSynchronized() { return synced; }
bool FrameStepper_IsMatchRestartPending() { return restart; }
bool FrameStepper_IsDesyncCheckReady() { return checksums; }
uint32_t FrameStepper_GetBattleGeneration() { return generation; }
void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t input, uint32_t) {
    assert(frame==g_netFrameCount && input==g_scheduledLocalInput[frame % RING_SIZE]); ++snapshotCalls;
}
void Controller_RecordFrameChecksum(int32_t frame) {
    ++checksumCalls; g_localChecksumFrame[frame % RING_SIZE]=frame;
    g_localChecksumConfirmed[frame % RING_SIZE]=true;
    g_localChecksum[frame % RING_SIZE]=0xABCD;
}
bool Controller_GetFrameChecksum(int32_t, uint32_t* checksum) { *checksum=0xABCD; return true; }
void AutoTest_OnLiveFrame(int32_t, uint32_t) { ++autotestCalls; }
void UpdateConfirmedChecksums() {}
void HookLog(const char*, ...) {}
void TraceInputPacket(const char*, const char*, int32_t, const InputPacket&, uint16_t, int, int, int) {}
void Netplay_SendPacket(const InputPacket& packet) { sent.push_back(packet); }
''' + functions + r'''
int main() {
    memset(g_scheduledLocalFrame, -1, sizeof(g_scheduledLocalFrame));
    memset(g_localChecksumFrame, -1, sizeof(g_localChecksumFrame));
    memset(g_localChecksumAdvertisedFrame, -1, sizeof(g_localChecksumAdvertisedFrame));
    Controller_PublishInputBeforeRender(); assert(sent.empty());
    g_preRenderInputReady=true;
    g_isResimulating=true; Controller_PublishInputBeforeRender();
    assert(sent.empty() && g_preRenderInputReady);
    g_isResimulating=false; restart=true; Controller_PublishInputBeforeRender(); assert(sent.empty());
    restart=false; synced=false; Controller_PublishInputBeforeRender(); assert(sent.empty());
    synced=true; active=false; Controller_PublishInputBeforeRender(); assert(sent.empty());
    active=true; Controller_PublishInputBeforeRender();
    assert(sent.size()==1 && !g_preRenderInputReady);
    assert(g_scheduledLocalFrame[103]==103 && g_scheduledLocalInput[103]==0x1234);
    assert(sent.back().start_frame==88 && sent.back().inputs[15]==0x1234);
    assert(sent.back().checksum_frame==UINT32_MAX && sent.back().battle_generation==1);
    assert(checksumCalls==0 && snapshotCalls==0 && autotestCalls==0 && g_oldInputs[15]==0);
    Controller_PublishInputBeforeRender(); assert(sent.size()==1);
    PublishFrameInput(true);
    assert(sent.size()==2 && sent.back().inputs[15]==0x1234);
    assert(checksumCalls==1 && autotestCalls==1 && sent.back().checksum_frame==100);
    assert(g_oldInputs[15]==0x1234);
    // Delay decrease: frame 103 has already been advertised, do not replace it.
    g_netFrameCount=101; g_delayFrames=2; g_localPending=0xFFFF;
    g_preRenderInputReady=true; Controller_PublishInputBeforeRender();
    assert(g_scheduledLocalInput[103]==0x1234 && sent.back().inputs[15]==0x1234);
    // Zero delay: schedule early, but snapshot/checksum remain frame-end work.
    g_netFrameCount=110; g_delayFrames=0; g_localPending=0x55;
    g_preRenderInputReady=true; Controller_PublishInputBeforeRender();
    assert(g_scheduledLocalInput[110]==0x55 && snapshotCalls==0 && checksumCalls==1);
    PublishFrameInput(true);
    assert(snapshotCalls==1 && g_recordedLocalInput[110]==0x55 && checksumCalls==2);
    // Frame-end fallback without a render pass, including neutral startup input.
    g_netFrameCount=111; g_localPending=0; PublishFrameInput(true);
    assert(g_scheduledLocalFrame[111]==111 && sent.back().inputs[15]==0);
    const size_t count=sent.size();
    g_isResimulating=true; PublishFrameInput(true); assert(sent.size()==count);
    g_isResimulating=false; g_netFrameCount=-1; PublishFrameInput(false); assert(sent.size()==count);
}
''')

    def test_mailbox_wake_and_pending_checksum(self):
        source = (ROOT / "UMVC3Hook/code/netplay.cpp").read_text()
        function = block(source, "void Netplay_SendPacket(const InputPacket& pkt)")
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
struct InputPacket { uint32_t battle_generation, checksum_frame, checksum, input; };
InputPacket g_outboundPacket{};
bool g_outboundNew=false, g_outboundValid=false;
bool locked=false;
int g_outboundCS=0, wakes=0;
void* g_outboundWake=(void*)1;
struct ScopedCS { ScopedCS(int&) { assert(!locked); locked=true; } ~ScopedCS() { locked=false; } };
bool SetEvent(void* event) { assert(event==g_outboundWake && !locked && g_outboundNew); ++wakes; return true; }
''' + function + r'''
int main() {
    Netplay_SendPacket({1, 50, 0xCAFE, 1});
    Netplay_SendPacket({1, UINT32_MAX, 0, 2});
    assert(wakes==2 && g_outboundPacket.input==2 && g_outboundPacket.checksum_frame==50);
    assert(g_outboundPacket.checksum==0xCAFE);
    g_outboundNew=false; // Service consumed the previous packet.
    Netplay_SendPacket({1, UINT32_MAX, 0, 3});
    assert(g_outboundPacket.checksum_frame==UINT32_MAX);
    Netplay_SendPacket({1, 51, 0xBEEF, 4});
    Netplay_SendPacket({2, UINT32_MAX, 0, 5});
    assert(g_outboundPacket.checksum_frame==UINT32_MAX); // No cross-battle checksum carry.
    g_outboundWake=nullptr;
    Netplay_SendPacket({2, UINT32_MAX, 0, 6}); assert(wakes==5);
}
''')

    def test_ordering_and_service_wait(self):
        controller = (ROOT / "UMVC3Hook/code/controller.cpp").read_text()
        start = block(controller, 'extern "C" void Controller_OnFrameStart()')
        self.assertLess(start.index("g_preRenderInputReady = false"), start.index("WaitForPredictionBudgetFrame"))
        self.assertTrue(start.rstrip().endswith("g_preRenderInputReady = true;\n}"))
        hooks = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        render = block(hooks, "static void __fastcall hkRenderMove(")
        self.assertLess(render.index("Controller_PublishInputBeforeRender"), render.index("oRenderMove(object)"))
        netplay = (ROOT / "UMVC3Hook/code/netplay.cpp").read_text()
        self.assertIn("g_eventDrivenService ? 0 : 1", netplay)
        self.assertIn("WSAEventSelect(g_host->socket, g_socketWake, FD_READ)", netplay)
        self.assertIn("HANDLE events[] = {g_outboundWake, g_socketWake}", netplay)
        self.assertIn("WaitForMultipleObjects(2, events, FALSE, 1)", netplay)


if __name__ == "__main__":
    unittest.main()
