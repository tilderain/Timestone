import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("g++"), "native g++ required")
class NetplayStatsTest(unittest.TestCase):
    def run_cpp(self, source):
        with tempfile.TemporaryDirectory(prefix="netplay-stats-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(source)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_rtt_history_cadence_rollover_and_publication(self):
        source = (ROOT / "UMVC3Hook/code/netplay.cpp").read_text()
        state = source[source.index("static constexpr int AVG_RTT_WINDOW_SIZE"):
                       source.index("// Reset hook defined in controller.cpp")]
        functions = "\n".join(block(source, marker) for marker in (
            "static void Netplay_UpdateRTTHistory(uint32_t now)\n{",
            "uint32_t Netplay_GetRTT()", "uint32_t Netplay_GetAverageRTT()",
            "int32_t Netplay_GetJitter()", "float Netplay_GetPacketLoss()",
            "float Netplay_GetRemotePacketLoss()"))
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
constexpr uint32_t ENET_PEER_PACKET_LOSS_SCALE = 1 << 16;
struct Peer { uint32_t roundTripTime; uint32_t packetLoss; } peer{20, 0};
Peer* g_peer = &peer;
std::atomic<bool> g_running{true};
bool steamActive = false;
bool SteamTransport_IsActive() { return steamActive; }
uint32_t SteamTransport_GetRTT() { return 80; }
int32_t SteamTransport_GetJitter() { return 10; }
float SteamTransport_GetPacketLoss() { return 12.5f; }
float SteamTransport_GetRemotePacketLoss() { return 6.25f; }
''' + state + functions + r'''
int main() {
    assert(Netplay_GetAverageRTT() == 0);
    Netplay_UpdateRTTHistory(0);
    assert(Netplay_GetRTT() == 20 && Netplay_GetAverageRTT() == 20);
    assert(Netplay_GetPacketLoss() == 0.0f);
    assert(Netplay_GetRemotePacketLoss() == -1.0f);
    peer.packetLoss = ENET_PEER_PACKET_LOSS_SCALE / 4;
    peer.roundTripTime = 60;
    for (uint32_t tick = 1; tick < 16; ++tick)
        Netplay_UpdateRTTHistory(tick);
    assert(g_rttHistoryCount == 1 && Netplay_GetAverageRTT() == 20);
    assert(Netplay_GetRTT() == 60);
    assert(Netplay_GetPacketLoss() == 25.0f);
    Netplay_UpdateRTTHistory(16);
    assert(g_rttHistoryCount == 2 && Netplay_GetAverageRTT() == 40);
    for (uint32_t tick = 32; tick <= 640; tick += 16)
        Netplay_UpdateRTTHistory(tick);
    assert(g_rttHistoryCount == 40 && Netplay_GetAverageRTT() == 60);
    peer.roundTripTime = 0;
    Netplay_UpdateRTTHistory(656);
    assert(g_rttHistoryCount == 40 && Netplay_GetAverageRTT() == 60);
    steamActive = true;
    Netplay_UpdateRTTHistory(672);
    assert(Netplay_GetRTT() == 80 && Netplay_GetAverageRTT() == 60);
    assert(Netplay_GetJitter() == 10);
    assert(Netplay_GetPacketLoss() == 12.5f);
    assert(Netplay_GetRemotePacketLoss() == 6.25f);
    g_lastRttSampleTick = UINT32_MAX - 7;
    Netplay_UpdateRTTHistory(8);
    assert(g_lastRttSampleTick == 8);
    g_running = false;
    assert(Netplay_GetRTT() == 0 && Netplay_GetAverageRTT() == 0);
    assert(Netplay_GetJitter() == -1);
    assert(Netplay_GetPacketLoss() == -1.0f);
    assert(Netplay_GetRemotePacketLoss() == -1.0f);
    g_running = true;
    steamActive = false;
    assert(Netplay_GetJitter() == -1);
    g_peer = nullptr;
    Netplay_UpdateRTTHistory(32);
    assert(Netplay_GetPacketLoss() == -1.0f);
}
''')

    def test_steam_status_poll_cadence(self):
        source = (ROOT / "UMVC3Hook/code/steam_transport.cpp").read_text()
        state = source[source.index("static constexpr DWORD STATUS_POLL_INTERVAL_MS"):
                       source.index("static std::atomic<int32_t> g_steamJitter")]
        gate = source[source.index("    const DWORD statusTick ="):
                      source.index("    SteamNetConnectionRealTimeStatus_t status =")]
        self.assertLess(source.index("if (g_bridgeHost) enet_host_flush"),
                        source.index("    const DWORD statusTick ="))
        self.assertIn("g_lastStatusPollTick = g_bridgeCreatedTick - STATUS_POLL_INTERVAL_MS;",
                      source)
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
using DWORD = uint32_t;
DWORD tick;
DWORD GetTickCount() { return tick; }
std::atomic_flag g_bridgePumpLock = ATOMIC_FLAG_INIT;
int polls = 0;
''' + state + '''
bool pump() {
''' + gate + r'''
    ++polls;
    g_bridgePumpLock.clear();
    return true;
}
int main() {
    for (DWORD start : {DWORD{0}, UINT32_MAX - 200}) {
        polls = 0;
        g_lastStatusPollTick = start - STATUS_POLL_INTERVAL_MS;
        for (DWORD elapsed = 0; elapsed <= 1200; ++elapsed) {
            tick = start + elapsed;
            assert(!g_bridgePumpLock.test_and_set());
            assert(pump());
            assert(polls == int(elapsed / 500 + 1));
        }
        assert(!g_bridgePumpLock.test_and_set());
        g_bridgePumpLock.clear();
    }
}
''')

    def test_steam_loss_percentage(self):
        source = (ROOT / "UMVC3Hook/code/steam_transport.cpp").read_text()
        update = source[source.index("        const float quality ="):
                        source.index("        // Steam clears its jitter peak")]
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <limits>
std::atomic<float> g_steamPacketLoss{-1.0f};
std::atomic<float> g_steamRemotePacketLoss{-1.0f};
void update(float deliveryQuality, float remoteDeliveryQuality = 1.0f) {
    struct { float m_flConnectionQualityLocal; float m_flConnectionQualityRemote; }
        status{deliveryQuality, remoteDeliveryQuality};
''' + update + r'''
}
int main() {
    update(1.0f);
    assert(g_steamPacketLoss == 0.0f);
    update(0.875f);
    assert(g_steamPacketLoss == 12.5f);
    update(0.0f);
    assert(g_steamPacketLoss == 100.0f);
    update(-1.0f);
    assert(g_steamPacketLoss == -1.0f);
    update(2.0f);
    assert(g_steamPacketLoss == -1.0f);
    update(std::numeric_limits<float>::quiet_NaN());
    assert(g_steamPacketLoss == -1.0f);
    update(1.0f, 0.875f);
    assert(g_steamPacketLoss == 0.0f && g_steamRemotePacketLoss == 12.5f);
    update(0.875f, 1.0f);
    assert(g_steamPacketLoss == 12.5f && g_steamRemotePacketLoss == 0.0f);
    update(1.0f, 0.0f);
    assert(g_steamRemotePacketLoss == 100.0f);
    for (float invalid : {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()}) {
        update(1.0f, invalid);
        assert(g_steamRemotePacketLoss == -1.0f);
    }
}
''')

    def test_jitter_peak_zero_unavailable_and_tick_wrap(self):
        source = (ROOT / "UMVC3Hook/code/steam_transport.cpp").read_text()
        state = source[source.index("static std::atomic<int32_t> g_steamJitter"):
                       source.index("static DWORD g_bridgeCreatedTick")]
        update = source[source.index("        // Steam clears its jitter peak"):
                        source.index("        if (status.m_eState ==")]
        self.run_cpp(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
using DWORD = uint32_t;
DWORD tick = 0;
DWORD GetTickCount() { return tick; }
''' + state + '''
void update(int32_t jitterUsec, DWORD sampleTick) {
    struct { int32_t m_usecMaxJitter; } status{jitterUsec};
    tick = sampleTick;
''' + update + r'''
}
int main() {
    assert(g_steamJitter == -1);
    update(10000, 1);
    update(-1, 499);
    assert(g_steamJitter == -1);
    update(0, 500);
    assert(g_steamJitter == 10);
    update(0, 1000);
    assert(g_steamJitter == 0);
    update(-1, 1500);
    assert(g_steamJitter == -1);
    update(900, 2000);
    assert(g_steamJitter == 0);
    g_jitterWindowStartTick = UINT32_MAX - 249;
    update(24000, 250);
    assert(g_steamJitter == 24);
}
''')

    def test_hud_keeps_packet_jitter_units(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        self.assertIn('"Jitter: %dms", jitterMs', source)
        self.assertIn('"Jitter: N/A"', source)
        self.assertNotIn("Netplay_GetJitter() / 2", source)
        self.assertIn('"Ping: %ums", Netplay_GetRTT() / 2', source)
        self.assertIn('"Loss: %s", lossText', source)
        self.assertIn('"%s in / %s out packet loss",', source)

    def test_loss_pair_format_and_warning(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        formatting = source[source.index("    const bool lossWarning ="):
                            source.index("    ImVec2 warningPos")]
        self.run_cpp(r'''
#include <cassert>
#include <cstdio>
#include <cstring>
void check(float lossPercent, float remoteLossPercent, const char* expected, bool warn) {
    constexpr float lossWarningPercent = 2.5f;
''' + formatting + r'''
    assert(std::strcmp(lossText, expected) == 0);
    assert(lossWarning == warn);
}
int main() {
    check(0.0f, 0.0f, "0.0% / 0.0%", false);
    check(0.0f, 6.0f, "0.0% / 6.0%", true);
    check(2.5f, 0.0f, "2.5% / 0.0%", true);
    check(-1.0f, 3.0f, "N/A / 3.0%", true);
    check(0.0f, -1.0f, "0.0% / N/A", false);
    check(-1.0f, -1.0f, "N/A / N/A", false);
}
''')

    def test_corner_panel_requires_active_netplay(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        self.assertIn(
            "if (Netplay_IsActive() && Controller_GetNetFrameCount() >= 0 &&\n"
            "        statusHudMode != 0)", source)
        self.assertNotIn("!Netplay_IsActive() || g_statusHudMode != 0", source)

    def test_escape_forces_expanded_hud_without_changing_preference(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        draw = block(source, "void FrameStepper_Draw()")
        mode = next(line for line in draw.splitlines()
                    if "const int statusHudMode =" in line)
        self.run_cpp(r'''
#include <cassert>
int g_statusHudMode = 0;
bool menu = false;
bool EscMenuImGui_IsOpen() { return menu; }
int mode() {
''' + mode + r'''
    return statusHudMode;
}
int main() {
    for (int saved = 0; saved <= 2; ++saved) {
        g_statusHudMode = saved;
        menu = false; assert(mode() == saved);
        menu = true; assert(mode() == 2);
        assert(g_statusHudMode == saved);
        menu = false; assert(mode() == saved);
    }
}
''')
        self.assertNotIn("g_statusHudMode =", draw)
        self.assertIn("if (statusHudMode == 1)", draw)
        self.assertIn("if (statusHudMode == 2 &&", draw)

    def test_escape_settings_follow_current_hud_content(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        draw = block(source, "void FrameStepper_Draw()")
        start = draw.index("    g_statusHudBottom = std::max(")
        measure = draw[start:draw.index(";", start) + 1]
        getter = block(source, "float FrameStepper_GetStatusHudBottom()")
        escape = (ROOT / "UMVC3Hook/code/escmenu_imgui.cpp").read_text()
        start = escape.index("    const float statusHudBottom =")
        position = escape[start:escape.index("    ImGui::PushStyleColor", start)]
        self.run_cpp(r'''
#include <algorithm>
#include <cassert>
struct ImVec2 {
    float x, y;
    ImVec2(float x, float y) : x(x), y(y) {}
};
constexpr int ImGuiCond_Always = 1;
float g_statusHudBottom = 0;
namespace ImGui {
ImVec2 windowPos{12, 12}, windowSize{200, 60}, cursor{12, 240}, settings{0, 0};
struct Style { ImVec2 WindowPadding{8, 8}; } style;
ImVec2 GetWindowPos() { return windowPos; }
ImVec2 GetWindowSize() { return windowSize; }
ImVec2 GetCursorScreenPos() { return cursor; }
Style& GetStyle() { return style; }
void SetNextWindowPos(ImVec2 value, int) { settings = value; }
}
''' + getter + "\nvoid measure() {\n" + measure + "\n}\nvoid position() {\n" + position + r'''
}
int main() {
    // First expanded frame still has last frame's compact window size.
    measure(); position();
    assert(g_statusHudBottom == 248 && ImGui::settings.y == 256);
    assert(ImGui::settings.x == ImGui::windowPos.x);
    // Never place settings inside an outer window taller than its content.
    ImGui::windowSize.y = 300;
    ImGui::cursor.y = 100;
    measure(); position();
    assert(g_statusHudBottom == 312 && ImGui::settings.y == 320);
    g_statusHudBottom = 0;
    position(); assert(ImGui::settings.y == 16);
}
''')
        self.assertLess(draw.index("g_statusHudBottom = 0.0f"),
                        draw.index("if (!IsTrainingMode())"))

    def test_escape_hud_draw_order_and_backdrop(self):
        hooks = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        end_scene = block(hooks, "long __stdcall hkEndScene(")
        self.assertEqual(end_scene.count("FrameStepper_Draw();"), 1)
        self.assertLess(end_scene.index("FrameStepper_Draw();"),
                        end_scene.index("EscMenuImGui_Draw();"))
        escape = (ROOT / "UMVC3Hook/code/escmenu_imgui.cpp").read_text()
        self.assertIn("GetBackgroundDrawList()->AddRectFilled", escape)
        self.assertNotIn('"##EscMenuBackdrop"', escape)

    def test_settings_change_lead_is_halved(self):
        source = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
uint32_t rtt = 0;
uint32_t Netplay_GetRTT() { return rtt; }
int32_t Controller_GetNetFrameCount() { return 100; }
''' + block(source, "static int32_t GetNetplaySettingsEffectiveFrame()") + r'''
int main() {
    for (rtt = 0; rtt <= 1000; ++rtt) {
        const int32_t previousLead = static_cast<int32_t>(rtt * 60 / 1000) + 3;
        assert(GetNetplaySettingsEffectiveFrame() == 100 + (previousLead + 1) / 2);
    }
}
''')


if __name__ == "__main__":
    unittest.main()
