import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "UMVC3Hook/code"


@unittest.skipUnless(shutil.which("g++"), "native g++ required")
class LiveBenchmarkTest(unittest.TestCase):
    def test_extension_empty_poll(self):
        source = (CODE / "netplay.cpp").read_text()
        dequeue = source.split("bool Netplay_GetExtensionPacket(", 1)[1].split(
            "\nvoid Netplay_ClearInputQueues", 1)[0]
        enqueue = source.split("case PACKET_BENCHMARK:", 1)[1].split("break;", 1)[0]
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
constexpr size_t MAX_EXTENSION_PACKETS = 64, MAX_EXTENSION_PACKET_SIZE = 256;
struct ExtensionPacket { size_t size; uint8_t data[MAX_EXTENSION_PACKET_SIZE]; };
ExtensionPacket g_inboundExtension[MAX_EXTENSION_PACKETS];
size_t g_inboundExtensionHead = 0, g_inboundExtensionCount = 0;
bool g_active = true, g_csInitialized = true;
std::atomic<bool> g_inboundExtensionPending{false};
std::mutex g_extensionCS;
std::atomic<unsigned> locks{0};
struct ScopedCS {
    std::unique_lock<std::mutex> lock;
    explicit ScopedCS(std::mutex& cs) : lock(cs) { ++locks; }
};
struct Packet { size_t dataLength; const uint8_t* data; };
struct Event { Packet* packet; };
void Enqueue(uint32_t value) {
    Packet packet{sizeof(value), reinterpret_cast<const uint8_t*>(&value)};
    Event event{&packet};
''' + enqueue + '\n}\nbool Netplay_GetExtensionPacket(' + dequeue + r'''
int main() {
    uint32_t value = 0;
    size_t size = 99;
    for (int i = 0; i < 10000; ++i)
        assert(!Netplay_GetExtensionPacket(&value, sizeof(value), size));
    assert(locks == 0 && size == 0);
    Enqueue(1); Enqueue(2);
    assert(!Netplay_GetExtensionPacket(&value, 1, size));
    assert(g_inboundExtensionPending && g_inboundExtensionCount == 2);
    assert(Netplay_GetExtensionPacket(&value, sizeof(value), size) && value == 1);
    assert(g_inboundExtensionPending);
    assert(Netplay_GetExtensionPacket(&value, sizeof(value), size) && value == 2);
    assert(!g_inboundExtensionPending);
    for (uint32_t i = 0; i < MAX_EXTENSION_PACKETS + 3; ++i) Enqueue(i);
    for (uint32_t i = 3; i < MAX_EXTENSION_PACKETS + 3; ++i)
        assert(Netplay_GetExtensionPacket(&value, sizeof(value), size) && value == i);
    assert(!g_inboundExtensionPending);
    // Exercise empty-to-nonempty publication across threads without overflow.
    std::thread producer([] {
        for (uint32_t i = 0; i < 10000; ++i) {
            while (g_inboundExtensionPending.load()) std::this_thread::yield();
            Enqueue(i);
        }
    });
    for (uint32_t i = 0; i < 10000; ++i) {
        while (!Netplay_GetExtensionPacket(&value, sizeof(value), size))
            std::this_thread::yield();
        assert(value == i && size == sizeof(value));
    }
    producer.join();
    assert(!g_inboundExtensionPending);
}
'''
        with tempfile.TemporaryDirectory(prefix="benchmark-queue-", dir="/tmp/opencode") as temporary:
            cpp = Path(temporary) / "test.cpp"
            binary = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_collector_protocol_and_display(self):
        source = (CODE / "live_benchmark.cpp").read_text()
        source = "\n".join(line for line in source.splitlines()
                           if not line.startswith("#include"))
        source = source.replace("std::snprintf", "CountedSnprintf")
        harness = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include "live_benchmark.h"
#define __fastcall
int numericFormats = 0;
int CountedSnprintf(char* output, size_t size, const char* format, ...) {
    ++numericFormats;
    va_list args; va_start(args, format);
    const int result = vsnprintf(output, size, format, args);
    va_end(args); return result;
}
struct LARGE_INTEGER { long long QuadPart; };
uint64_t qpc = 1000, clockMs = 1000;
int reads = 0, clockReads = 0;
void QueryPerformanceCounter(LARGE_INTEGER* v) { ++reads; v->QuadPart = qpc; }
void QueryPerformanceFrequency(LARGE_INTEGER* v) { v->QuadPart = 1000000; }
uint64_t GetTickCount64() { ++clockReads; return clockMs; }
uint32_t GetCurrentProcessId() { return 1234; }
constexpr uint32_t PACKET_EXTENSION = 16, PACKET_BENCHMARK = 19;
bool connected = true, ready = true, restarting = false, renderThread = false;
bool singleThread = true, menu = false, key = false, sendOK = true;
bool steam = false;
const char* localUsername = "", *peerUsername = "";
const char* SteamLobby_GetLocalUsername() { return localUsername; }
const char* SteamLobby_GetPeerUsername() { return peerUsername; }
bool g_multiResimEnabled = true;
uint32_t generation = 7;
int32_t rollbackFrames = 7;
extern "C" int32_t Controller_GetRollbackFrames() { return rollbackFrames; }
int32_t action[64] = {};
uint64_t GetAction() { return (uint64_t)action; }
uintptr_t _addr(uint64_t) { return (uintptr_t)&GetAction; }
bool Netplay_IsActive() { return connected; }
bool FrameStepper_IsMatchRestartPending() { return restarting; }
bool FrameStepper_IsDesyncCheckReady() { return ready; }
uint32_t FrameStepper_GetBattleGeneration() { return generation; }
bool D3D9Reset_IsRenderingThreadEnabled() { return renderThread; }
bool IsGameSingleThreaded() { return singleThread; }
bool EscMenuImGui_IsOpen() { return menu; }
bool SteamTransport_IsActive() { return steam; }
int connectionQueries = 0;
const char* SteamLobby_GetConnectionType() { ++connectionQueries; return "Direct"; }
bool KeyPressed(int vk) { assert(vk == '0'); const bool pressed = key; key = false; return pressed; }
std::deque<std::vector<uint8_t>> inbound;
std::vector<std::vector<uint8_t>> outbound;
bool Netplay_GetExtensionPacket(void* data, size_t capacity, size_t& size) {
    if (inbound.empty()) return false;
    size = inbound.front().size();
    assert(size <= capacity);
    std::memcpy(data, inbound.front().data(), size);
    inbound.pop_front();
    return true;
}
bool lastSendReliable = false;
bool Netplay_SendExtensionPacket(const void* data, size_t size, bool reliable = true) {
    if (!sendOK) return false;
    lastSendReliable = reliable;
    const auto* bytes = (const uint8_t*)data;
    outbound.emplace_back(bytes, bytes + size);
    return true;
}
int rosterPackets = 0;
void ChrSelImGui_HandleExtension(const void*, size_t) { ++rosterPackets; }
struct ImVec2 { float x, y; ImVec2(float x = 0, float y = 0) : x(x), y(y) {} };
struct ImVec4 {
    float x, y, z, w;
    ImVec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};
constexpr int ImGuiCond_Always = 1;
constexpr int ImGuiWindowFlags_NoDecoration = 1, ImGuiWindowFlags_NoMove = 2;
constexpr int ImGuiWindowFlags_NoSavedSettings = 4, ImGuiWindowFlags_AlwaysAutoResize = 8;
constexpr int ImGuiWindowFlags_NoInputs = 16, ImGuiTableFlags_BordersInnerV = 1;
constexpr int ImGuiTableColumnFlags_WidthFixed = 1;
constexpr int ImGuiCol_WindowBg = 0, ImGuiCol_Border = 1, ImGuiCol_Text = 2;
constexpr int ImGuiCol_TextDisabled = 3, ImGuiCol_FrameBg = 4, ImGuiCol_PlotHistogram = 5;
constexpr int ImGuiStyleVar_WindowPadding = 0, ImGuiStyleVar_ItemSpacing = 1;
constexpr int ImGuiStyleVar_WindowRounding = 2, ImGuiStyleVar_WindowBorderSize = 3;
namespace ImGui {
struct IO { bool WantTextInput = false; ImVec2 DisplaySize{1280, 720}; } io;
std::vector<std::string> text;
int windows = 0;
bool clickExpand = false;
int colors = 0, styles = 0;
std::vector<float> bars;
std::vector<ImVec4> barColors;
ImVec4 barColor{0, 0, 0, 0};
ImVec4 textColor{0, 0, 0, 0};
struct DrawList {
    int markers = 0;
    void AddLine(ImVec2 begin, ImVec2 end, unsigned int) {
        assert(begin.x == 50 && end.x == 50); ++markers;
    }
} drawList;
DrawList* GetWindowDrawList() { return &drawList; }
ImVec2 GetItemRectMin() { return ImVec2(0, 0); }
ImVec2 GetItemRectMax() { return ImVec2(100, 5); }
unsigned int GetColorU32(int) { return 0; }
IO& GetIO() { return io; }
void Text(const char* fmt, ...) {
    char buffer[512]; va_list args; va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args); text.emplace_back(buffer);
}
void TextDisabled(const char* fmt, ...) {
    char buffer[512]; va_list args; va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args); text.emplace_back(buffer);
}
void TextColored(ImVec4 color, const char* fmt, ...) {
    textColor = color;
    char buffer[512]; va_list args; va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args); text.emplace_back(buffer);
}
void TextUnformatted(const char* value) { text.emplace_back(value); }
void SetNextWindowPos(ImVec2, int, ImVec2) {}
void SetNextWindowBgAlpha(float) {}
void PushStyleColor(int index, ImVec4 color) {
    ++colors; if (index == ImGuiCol_PlotHistogram) barColor = color;
}
void PopStyleColor(int count = 1) { colors -= count; assert(colors >= 0); }
void PushStyleVar(int, ImVec2) { ++styles; }
void PushStyleVar(int, float) { ++styles; }
void PopStyleVar(int count = 1) { styles -= count; assert(styles >= 0); }
void ProgressBar(float fraction, ImVec2, const char*) {
    assert(fraction >= 0 && fraction <= 1);
    bars.push_back(fraction); barColors.push_back(barColor);
}
void SameLine(float = 0, float = -1) {}
bool SmallButton(const char* label) {
    text.emplace_back(label);
    const bool clicked = clickExpand; clickExpand = false; return clicked;
}
void Separator() {}
void Begin(const char*, void*, int) { ++windows; }
void End() {}
void PushTextWrapPos(float) {}
void PopTextWrapPos() {}
float GetCursorPosX() { return 0; }
bool BeginTable(const char*, int, int, ImVec2) { return true; }
void TableSetupColumn(const char* label, int = 0, float = 0) { text.emplace_back(label); }
void TableHeadersRow() {}
void TableNextRow() {}
void TableNextColumn() {}
void EndTable() {}
}
''' + source + r'''
void Receive(Packet packet) {
    const auto* bytes = (const uint8_t*)&packet;
    inbound.emplace_back(bytes, bytes + sizeof(packet));
}
bool HasText(const char* needle) {
    return std::any_of(ImGui::text.begin(), ImGui::text.end(),
        [=](const auto& text) { return text.find(needle) != std::string::npos; });
}
void Frame(int frame) {
    const auto start = LiveBenchmark_BeginFrame(frame);
    assert(start);
    qpc += 900;
    const auto pacing = LiveBenchmark_Now();
    qpc += 10000;
    LiveBenchmark_EndPacing(pacing);
    qpc += 1100;
    LiveBenchmark_EndFrame(start, frame + 1);
    clockMs += 1000;
}
int main() {
    action[0xD8 / 4] = 3;
    LiveBenchmark_Update();
    assert(g_generation == 7);
    assert((g_flags >> 8) == 7);
    assert(g_nextSend == 0); // No report scheduling while disabled.
    assert(outbound.empty());
    bool varied = false;
    const auto firstDelay = NextReportDelay();
    for (int i = 0; i < 100; ++i) {
        const auto delay = NextReportDelay();
        assert(delay >= 5000 && delay <= 10000);
        varied |= delay != firstDelay;
    }
    assert(varied);
    // Neither live frames nor independent corrections collect without a request.
    for (bool online : {false, true}) {
        connected = online;
        LiveBenchmark_Update();
        for (bool open : {false, true}) {
            menu = open;
            LiveBenchmark_Draw();
            if (online && open) continue;
            const int idleReads = reads;
            assert(LiveBenchmark_BeginFrame(0) == 0);
            assert(LiveBenchmark_BeginResim() == 0);
            assert(LiveBenchmark_Now() == 0);
            LiveBenchmark_EndResim(0);
            assert(reads == idleReads);
        }
    }
    // Offline 0 enables both samplers, then expires even with Esc open.
    connected = false;
    LiveBenchmark_Update();
    key = true;
    LiveBenchmark_Draw();
    auto requestedFrame = LiveBenchmark_BeginFrame(0);
    assert(requestedFrame);
    LiveBenchmark_EndFrame(requestedFrame, 1);
    auto requestedResim = LiveBenchmark_BeginResim();
    assert(requestedResim);
    LiveBenchmark_EndResim(requestedResim);
    clockMs += 8000;
    const int expiredReads = reads;
    assert(LiveBenchmark_BeginFrame(1) == 0);
    assert(LiveBenchmark_BeginResim() == 0);
    assert(reads == expiredReads);
    LiveBenchmark_Draw();
    g_showRequested.store(false);
    connected = true;
    LiveBenchmark_Update();
    // Netplay Esc enables collection without pressing 0.
    menu = true;
    LiveBenchmark_Draw();
    Samples samples;
    assert(Summarize(samples).count == 0);
    for (uint64_t i = 1; i <= 150; ++i) samples.Add(i);
    const auto summary = Summarize(samples);
    assert(summary.count == 120 && summary.minimum == 31 && summary.median == 90);
    assert(summary.p95 == 144 && summary.maximum == 150);
    assert(RecentCost(samples) == 150);
    samples.Add(1); samples.Add(2);
    assert(RecentCost(samples) == 2); // Old spikes no longer hold the bar down.
    Samples recentEdges;
    assert(RecentCost(recentEdges) == 0);
    recentEdges.Add(0); assert(RecentCost(recentEdges) == 1);
    recentEdges.Add(UINT64_MAX); assert(RecentCost(recentEdges) == UINT32_MAX);
    recentEdges.Add(4); recentEdges.Add(3); assert(RecentCost(recentEdges) == 4);
    const Costs slowHistory{120, 2000, 20000, 30000, 40000};
    DrawRate(slowHistory, true, true, 16667);
    assert(ImGui::bars.back() > 0.499f && ImGui::bars.back() < 0.501f);
    const int before = reads;
    assert(LiveBenchmark_Now() == 0);
    LiveBenchmark_Record(LiveBenchmarkMetric::Save, 0);
    assert(reads == before);
    ready = false;
    assert(LiveBenchmark_BeginFrame(0) == 0);
    ready = true;
    for (int frame = 0; frame < 120; ++frame) Frame(frame);
    Frame(120);
    auto live = Cost(g_local, LiveBenchmarkMetric::Live);
    assert(live.count == 120 && live.minimum == 2000 && live.maximum == 2000);
    assert(Cost(g_local, LiveBenchmarkMetric::CleanLive).count == 120);
    assert(Cost(g_local, LiveBenchmarkMetric::Restore).count == 0);

    const auto start = LiveBenchmark_BeginFrame(121);
    const auto correction = LiveBenchmark_Now();
    auto phase = LiveBenchmark_Now(); qpc += 200;
    LiveBenchmark_Record(LiveBenchmarkMetric::Save, phase);
    phase = LiveBenchmark_Now(); qpc += 300;
    LiveBenchmark_Record(LiveBenchmarkMetric::Restore, phase);
    phase = LiveBenchmark_Now(); qpc += 100;
    LiveBenchmark_Record(LiveBenchmarkMetric::Headless, phase);
    LiveBenchmark_Record(LiveBenchmarkMetric::Correction, correction);
    phase = LiveBenchmark_Now(); qpc += 10000;
    LiveBenchmark_EndPacing(phase);
    qpc += 500;
    LiveBenchmark_EndFrame(start, 122);
    assert(g_samples[(size_t)LiveBenchmarkMetric::Live].ticks[1] == 1100);
    assert(g_samples[(size_t)LiveBenchmarkMetric::CleanLive].next == 1);
    assert(g_samples[(size_t)LiveBenchmarkMetric::Correction].ticks[0] == 600);
    assert(g_samples[(size_t)LiveBenchmarkMetric::Restore].ticks[0] == 300);
    clockMs += 1000;
    const auto invalid = LiveBenchmark_BeginFrame(122);
    phase = LiveBenchmark_Now(); qpc += 500;
    LiveBenchmark_Record(LiveBenchmarkMetric::Restore, phase);
    LiveBenchmark_EndFrame(invalid, 122); // Frozen frame, not gameplay.
    assert(g_samples[(size_t)LiveBenchmarkMetric::Live].count == 120);
    assert(g_samples[(size_t)LiveBenchmarkMetric::Restore].count == 1);

    Packet peer;
    peer.generation = generation;
    peer.sequence = 1;
    peer.flags = 1u | (7u << 8);
    peer.costs[(size_t)LiveBenchmarkMetric::Live] = {120, 1000, 2000, 3000, 4000};
    assert(Valid(peer));
    Packet malformed = peer;
    malformed.costs[0].minimum = 1;
    assert(!Valid(malformed)); // Empty metrics cannot carry costs.
    malformed = peer; malformed.version = 1; assert(!Valid(malformed));
    malformed = peer; malformed.flags = 8; assert(!Valid(malformed));
    malformed = peer; malformed.flags = 17u << 8; assert(!Valid(malformed));
    malformed = peer; ++malformed.generation; assert(!Valid(malformed));
    malformed = peer; malformed.costs[4].median = 999; assert(!Valid(malformed));
    malformed = peer; malformed.costs[4].count = 121; assert(!Valid(malformed));
    Receive(peer);
    LiveBenchmark_Update();
    assert(g_peerTime == clockMs && g_peerSequence == 1);
    const auto receivedAt = g_peerTime;
    clockMs += 100;
    Receive(peer); LiveBenchmark_Update();
    assert(g_peerTime == receivedAt); // Duplicate summary does not refresh freshness.
    inbound.push_back({16, 0, 0, 0, 0x43, 0x4c, 0x53, 0x31});
    LiveBenchmark_Update(); assert(rosterPackets == 1);

    menu = false;
    LiveBenchmark_Draw();
    ImGui::windows = 0;
    const int beforeHiddenDraw = clockReads;
    LiveBenchmark_Draw(); assert(ImGui::windows == 0);
    assert(clockReads == beforeHiddenDraw);
    key = true; ImGui::io.WantTextInput = true;
    LiveBenchmark_Draw(); assert(ImGui::windows == 0);
    ImGui::io.WantTextInput = false;
    LiveBenchmark_Draw(); assert(ImGui::windows == 1);
    assert(g_showUntil.load() == clockMs + 8000);
    outbound.clear();
    LiveBenchmark_Update();
    bool sentShow = false, sentSummary = false;
    for (const auto& bytes : outbound) {
        Packet packet; std::memcpy(&packet, bytes.data(), sizeof(packet));
        sentShow |= packet.kind == Show;
        sentSummary |= packet.kind == Summary;
    }
    assert(sentShow && sentSummary && g_pendingShowUntil == 0);
    assert(lastSendReliable);
    assert(g_summaryRequestedUntil == clockMs + 8000);
    Packet show; show.kind = Show; show.generation = generation; show.sequence = 1;
    clockMs += 100;
    outbound.clear();
    Receive(show); LiveBenchmark_Update();
    const auto expiry = g_showUntil.load();
    assert(expiry == clockMs + 8000);
    assert(outbound.size() == 1);
    Packet reply; std::memcpy(&reply, outbound.front().data(), sizeof(reply));
    assert(reply.kind == Summary); // Reply immediately, without echoing Show.
    clockMs += 100;
    Receive(show); LiveBenchmark_Update();
    assert(g_showUntil.load() == expiry);
    for (int second = 1; second < 8; ++second) {
        clockMs = expiry - 8000 + second * 1000 - 1;
        g_localTime = clockMs;
        outbound.clear(); LiveBenchmark_Update();
        assert(outbound.empty());
        ++clockMs;
        LiveBenchmark_Update();
        assert(outbound.size() == 1);
        Packet update; std::memcpy(&update, outbound.front().data(), sizeof(update));
        assert(update.kind == Summary);
        assert(lastSendReliable);
        outbound.clear(); LiveBenchmark_Update();
        assert(outbound.empty());
    }
    clockMs = expiry;
    ImGui::windows = 0; LiveBenchmark_Draw(); assert(ImGui::windows == 0);
    menu = true; ImGui::text.clear(); LiveBenchmark_Draw(); assert(ImGui::windows == 1);
    assert(HasText("Expand") && !HasText("Slow:") && !HasText("Low:"));
    assert(!HasText("Samples") && !HasText("16.7 ms limit") && !HasText("ms est."));
    ImGui::clickExpand = true;
    LiveBenchmark_Draw();
    assert(HasText("Slow:") && HasText("Samples") && HasText("16.7 ms limit"));
    assert(HasText("333 FPS") && HasText("Low:250  High:1000"));
    assert(ImGui::colors == 0 && ImGui::styles == 0);
    assert(HasText("You") && HasText("Opp."));
    steam = true;
    localUsername = "Local Steam Name"; peerUsername = "Opponent Steam Name";
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText(localUsername) && HasText(peerUsername));
    steam = false;
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText(localUsername) && HasText("Opp.") && !HasText(peerUsername));
    steam = true;
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText("Steam connection: Direct"));
    const int previousQueries = connectionQueries;
    for (int i = 0; i < 60; ++i) LiveBenchmark_Draw();
    assert(connectionQueries == previousQueries);
    clockMs += 1000;
    LiveBenchmark_Draw();
    assert(connectionQueries == previousQueries + 1);
    steam = false;
    ImGui::bars.clear();
    DrawRate({120, 5000, 7000, 10000, 12000}, true, true);
    assert(ImGui::bars.size() == 1 && ImGui::bars[0] > 0.83f && ImGui::bars[0] < 0.84f);
    // Headline and bar use the same cost; historical health remains separate.
    const Costs spikes{62, 6500, 8403, 20000, 32258};
    assert(Health(spikes, true) == FrameHealth::Spikes);
    DrawRate(spikes, true, true);
    assert(HasText("83% (50 FPS)") && HasText("Slow: 20.0 ms"));
    assert(ImGui::bars.back() > 0.41f && ImGui::bars.back() < 0.42f);
    const auto warning = CapacityColor(83.0, true);
    assert(ImGui::barColors.back().x == warning.x && ImGui::barColors.back().y == warning.y);
    assert(ImGui::textColor.y == ImGui::barColors.back().y);
    Packet resim;
    resim.flags = 7u << 8;
    for (auto& cost : resim.costs) cost = {2, 1000, 1000, 1000, 1000};
    resim.costs[(size_t)LiveBenchmarkMetric::Restore] = {2, 2000, 2000, 2000, 2000};
    assert(RollbackCost(resim, &Costs::p95) == 9000);
    const int markersBefore = ImGui::drawList.markers;
    DrawResim(resim, true, true);
    assert(HasText("185% (111 FPS)") && HasText("7f: 9.0 ms est."));
    assert(ImGui::bars.back() > 0.92f && ImGui::bars.back() < 0.93f);
    assert(ImGui::drawList.markers == markersBefore + 1);
    resim.recentRollback = 27779; // Just below 60%.
    DrawResim(resim, true, true);
    assert(ImGui::barColors.back().y == HealthColor(FrameHealth::Slow).y);
    resim.recentRollback = 20000; // Between 60% and 100%.
    DrawResim(resim, true, true);
    assert(ImGui::barColors.back().y == 0.65f);
    assert(ImGui::textColor.y == ImGui::barColors.back().y);
    assert(HasText("83% (50 FPS)"));
    resim.recentRollback = 16667; // Exactly 100%.
    DrawResim(resim, true, true);
    assert(ImGui::barColors.back().y == HealthColor(FrameHealth::Smooth).y);
    resim.recentRollback = 0;
    resim.flags |= 1;
    assert(RollbackCost(resim, &Costs::p95) == 16000); // Seven corrected saves too.
    resim.flags = 5u << 8;
    assert(RollbackCost(resim, &Costs::p95) == 7000); // Use the reporting peer's depth.
    resim.flags = 7u << 8;
    resim.costs[(size_t)LiveBenchmarkMetric::Headless] = {2, 9000, 9000, 9000, 9000};
    DrawResim(resim, true, true);
    assert(HasText("26% (") && HasText("FPS)"));
    resim.flags = 0;
    ImGui::text.clear(); DrawResim(resim, true, true);
    assert(HasText("Off"));

    g_local = peer; g_localTime = clockMs;
    Publish();
    menu = false; key = true; sendOK = false;
    LiveBenchmark_Draw(); LiveBenchmark_Update();
    assert(g_pendingShowUntil == clockMs + 5000);
    sendOK = true; LiveBenchmark_Update(); assert(g_pendingShowUntil == 0);
    key = true; sendOK = false;
    LiveBenchmark_Draw(); LiveBenchmark_Update();
    clockMs += 5001; sendOK = true; outbound.clear(); LiveBenchmark_Update();
    for (const auto& bytes : outbound) {
        Packet packet; std::memcpy(&packet, bytes.data(), sizeof(packet));
        assert(packet.kind != Show); // Expired request never reappears later.
    }
    Packet departing = peer;
    departing.sequence = 2;
    for (auto& cost : departing.costs) cost = {2, 1000, 1000, 1000, 1000};
    departing.costs[(size_t)LiveBenchmarkMetric::Live] = {120, 3000, 4000, 5000, 6000};
    Receive(departing); LiveBenchmark_Update();
    const uint64_t lastPeerTime = g_peerTime, lastLocalTime = g_localTime;
    assert(lastPeerTime != 0 && lastLocalTime != 0);
    connected = false; LiveBenchmark_Update();
    assert(g_peerTime == lastPeerTime && g_localTime == lastLocalTime);
    assert(g_peer.sequence == departing.sequence);
    menu = true; ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText("Opp. (left)") && HasText("Disconnected"));
    assert(HasText("200 FPS") && HasText("1.00 ms"));
    assert(!ReadPublished().connected);
    // Offline resets/settings changes must not erase the departed player's report.
    clockMs += 20000;
    ++generation;
    g_multiResimEnabled = !g_multiResimEnabled;
    LiveBenchmark_Update();
    assert(g_peerTime == lastPeerTime && g_peer.sequence == departing.sequence);
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText("Disconnected") && HasText("200 FPS"));
    g_multiResimEnabled = !g_multiResimEnabled;
    connected = true; LiveBenchmark_Update();
    assert(g_peerTime == 0 && g_localTime == 0);
    assert(ReadPublished().peerTime == 0 && ReadPublished().connected);
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(!HasText("Disconnected") && !HasText("200 FPS"));
    menu = false;
    Receive(peer); LiveBenchmark_Update(); assert(g_peerTime == 0);
    connected = false; LiveBenchmark_Update();
    assert(g_peerTime == 0);

    Packet assessment;
    assessment.flags = 1u | (7u << 8);
    for (auto& cost : assessment.costs) cost = {120, 100, 200, 300, 400};
    for (auto& cost : assessment.costs) cost.count = 2;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("Good") && HasText("Rollback: good"));
    assessment.costs[(size_t)LiveBenchmarkMetric::Live].count = 1;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("CHECKING"));
    assessment.costs[(size_t)LiveBenchmarkMetric::Live].count = 2;
    assessment.costs[(size_t)LiveBenchmarkMetric::Headless].p95 = 5000;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("Rollback: may lag"));
    assessment.costs[(size_t)LiveBenchmarkMetric::Restore].count = 0;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("Good") && HasText("Rollback: untested"));
    assessment.costs[(size_t)LiveBenchmarkMetric::Live].p95 = 20000;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("Stutter") && !HasText("Good"));
    assessment.costs[(size_t)LiveBenchmarkMetric::Live].median = 20000;
    ImGui::text.clear(); Assessment(assessment, true);
    assert(HasText("SLOW"));
    ImGui::text.clear(); Assessment(assessment, false);
    assert(HasText("NO RECENT DATA") && !HasText("SLOW"));

    // Between sampled frames, even rollback scopes must not read QPC.
    Clear();
    clockMs += 1000;
    key = true;
    LiveBenchmark_Draw();
    const auto sampled = LiveBenchmark_BeginFrame(0);
    assert(sampled);
    qpc += 2000;
    LiveBenchmark_EndFrame(sampled, 1);
    const uint32_t published = g_local.sequence;
    const int sampledReads = reads;
    for (int frame = 1; frame < 60; ++frame) {
        clockMs += 16;
        const auto skipped = LiveBenchmark_BeginFrame(frame);
        assert(skipped == 0);
        auto scope = LiveBenchmark_Now();
        LiveBenchmark_Record(LiveBenchmarkMetric::Headless, scope);
        LiveBenchmark_EndPacing(scope);
        LiveBenchmark_EndFrame(skipped, frame + 1);
    }
    assert(reads == sampledReads && g_local.sequence == published);
    assert(Cost(g_local, LiveBenchmarkMetric::Live).count == 1);
    clockMs += 56;
    Frame(60);
    assert(Cost(g_local, LiveBenchmarkMetric::Live).count == 2);

    // Menu-only reports are jittered, rather than sent each second.
    connected = true;
    g_showUntil.store(0);
    g_showRequested.store(false);
    menu = true;
    LiveBenchmark_Draw();
    LiveBenchmark_Update(); // Connection transition clears the old session.
    clockMs += 1000;
    Frame(0);
    outbound.clear();
    LiveBenchmark_Update();
    assert(outbound.empty());
    const auto deadline = g_nextSend;
    clockMs = deadline - 1;
    g_localTime = clockMs; // Keep the fixture's sample fresh.
    LiveBenchmark_Update(); assert(outbound.empty());
    clockMs = deadline;
    LiveBenchmark_Update(); assert(outbound.size() == 1);
    assert(!lastSendReliable);
    assert(g_nextSend >= clockMs + 5000 && g_nextSend <= clockMs + 10000);
    outbound.clear(); clockMs += 1000;
    LiveBenchmark_Update(); assert(outbound.empty());

    // Closing the menu disables sampling and reports, even when a report is due.
    menu = false;
    LiveBenchmark_Draw();
    clockMs = g_nextSend;
    g_localTime = clockMs;
    LiveBenchmark_Update();
    assert(outbound.empty());
    const int closedReads = reads;
    assert(LiveBenchmark_BeginFrame(1) == 0);
    assert(LiveBenchmark_BeginResim() == 0);
    assert(reads == closedReads);

    peer.generation = generation;
    peer.sequence = 2;
    Receive(peer); LiveBenchmark_Update();
    menu = true;
    clockMs += 10000;
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(HasText("Good"));
    assert(std::count(ImGui::text.begin(), ImGui::text.end(), "NO RECENT DATA") == 1);
    clockMs += 2001;
    ImGui::text.clear(); LiveBenchmark_Draw();
    assert(std::count(ImGui::text.begin(), ImGui::text.end(), "NO RECENT DATA") == 2);

    // An early request waits for the next scheduled sample, not an extra capture.
    Clear(); outbound.clear();
    g_showRequested.store(true);
    const int beforeRequest = reads;
    LiveBenchmark_Update();
    assert(reads == beforeRequest && g_summaryRequestedUntil == clockMs + 8000);
    assert(outbound.size() == 1);
    Frame(0);
    outbound.clear();
    LiveBenchmark_Update();
    assert(g_summaryRequestedUntil > clockMs && outbound.size() == 1);
    Packet freshReply; std::memcpy(&freshReply, outbound.front().data(), sizeof(freshReply));
    assert(freshReply.kind == Summary && freshReply.sequence == g_local.sequence);

    // Corrections between sampled live frames still collect once per second.
    Clear();
    Frame(0);
    clockMs -= 900;
    assert(LiveBenchmark_BeginFrame(1) == 0);
    const auto resimStart = LiveBenchmark_BeginResim();
    assert(resimStart);
    phase = LiveBenchmark_Now(); qpc += 200;
    LiveBenchmark_Record(LiveBenchmarkMetric::Restore, phase);
    phase = LiveBenchmark_Now(); qpc += 100;
    LiveBenchmark_Record(LiveBenchmarkMetric::Headless, phase);
    phase = LiveBenchmark_Now(); qpc += 50;
    LiveBenchmark_Record(LiveBenchmarkMetric::Save, phase);
    LiveBenchmark_EndResim(resimStart);
    assert(LiveBenchmark_Now() == 0);
    LiveBenchmark_EndFrame(0, 2);
    clockMs += 900;
    Frame(2);
    assert(Cost(g_local, LiveBenchmarkMetric::Restore).count == 1);
    assert(Cost(g_local, LiveBenchmarkMetric::Correction).median == 350);
    assert(RollbackCost(g_local, &Costs::p95) == 200 + 7 * 150);
    ImGui::text.clear();
    DrawResim(g_local, true, true);
    assert(!HasText("Awaiting resim") && HasText("FPS)") && !HasText("capacity"));
    // A resim that is not yet due does not collect; the next due one does.
    const auto nextResim = g_nextResimSample;
    clockMs = nextResim - 1;
    assert(LiveBenchmark_BeginResim() == 0);
    assert(LiveBenchmark_Now() == 0);
    LiveBenchmark_EndResim(0);
    clockMs = nextResim;
    const auto secondResim = LiveBenchmark_BeginResim();
    assert(secondResim);
    qpc += 500;
    LiveBenchmark_EndResim(secondResim);
    assert(g_samples[(size_t)LiveBenchmarkMetric::Correction].count == 2);

    // A slow renderer owns its slot; repeated publications cannot overwrite it.
    const auto& held = ReadPublished();
    const auto heldSequence = held.local.sequence;
    for (uint32_t i = 1; i <= 1000; ++i) {
        g_local.sequence = heldSequence + i;
        Publish();
        assert(held.local.sequence == heldSequence);
    }
    assert(ReadPublished().local.sequence == heldSequence + 1000);

    // Exercise production publication code with real concurrent producer/consumer threads.
    g_local = {}; g_peer = {}; g_localTime = g_peerTime = 0; g_connected = true;
    Publish();
    constexpr uint32_t publications = 500000;
    std::atomic<bool> readerReady{false}, finished{false};
    uint32_t observed = 0;
    std::thread writer([&] {
        while (!readerReady.load(std::memory_order_acquire)) std::this_thread::yield();
        for (uint32_t i = 1; i <= publications; ++i) {
            g_local.sequence = i;
            g_local.generation = i ^ 0x76543210u;
            for (auto& cost : g_local.costs) cost = {i, i + 1, i + 2, i + 3, i + 4};
            g_peer = g_local;
            g_localTime = (uint64_t)i * 0x100000001ull;
            g_peerTime = ~g_localTime;
            Publish();
        }
        finished.store(true, std::memory_order_release);
    });
    std::thread reader([&] {
        readerReady.store(true, std::memory_order_release);
        uint32_t previous = 0;
        do {
            const auto& snapshot = ReadPublished();
            const uint32_t sequence = snapshot.local.sequence;
            if (!sequence) continue;
            assert(sequence >= previous);
            previous = sequence;
            assert(snapshot.connected && snapshot.peer.sequence == sequence);
            assert(snapshot.local.generation == (sequence ^ 0x76543210u));
            assert(snapshot.localTime == (uint64_t)sequence * 0x100000001ull);
            assert(snapshot.peerTime == ~snapshot.localTime);
            for (size_t i = 0; i < Metrics; ++i) {
                const auto& a = snapshot.local.costs[i];
                const auto& b = snapshot.peer.costs[i];
                assert(a.count == sequence && a.minimum == sequence + 1 &&
                    a.median == sequence + 2 && a.p95 == sequence + 3 && a.maximum == sequence + 4);
                assert(std::memcmp(&a, &b, sizeof(a)) == 0);
            }
            ++observed;
        } while (!finished.load(std::memory_order_acquire));
        assert(ReadPublished().local.sequence == publications);
    });
    writer.join(); reader.join();
    assert(observed != 0);

    // Isolate collector bookkeeping; clocks/network/ImGui remain stubbed here.
    connected = true; menu = false;
    g_menuOpen.store(false); g_showUntil.store(0); g_showRequested.store(false);
    LiveBenchmark_Update();
    const int idleClocks = clockReads, idleCounters = reads;
    for (int i = 0; i < 10000; ++i) {
        auto start = LiveBenchmark_BeginFrame(i);
        auto correction = LiveBenchmark_BeginResim();
        LiveBenchmark_EndResim(correction);
        LiveBenchmark_EndFrame(start, i + 1);
        LiveBenchmark_Update();
    }
    printf("Idle 10000 frames: %d clock reads, %d QPC reads (stubbed APIs).\n",
        clockReads - idleClocks, reads - idleCounters);
    assert(clockReads == idleClocks && reads == idleCounters);
    Samples recentBench;
    volatile uint32_t recentSink = 0;
    const auto recentStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 1000000; ++i) {
        recentBench.Add(i);
        recentSink = RecentCost(recentBench);
    }
    const auto recentNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - recentStart).count();
    assert(recentSink == 999999);
    printf("Recent-cost bookkeeping: %.1f ns/update (native harness).\n",
        (double)recentNs / 1000000);

    // Cache must preserve exact percentile results through wraparound and reset.
    Samples checked;
    for (uint64_t i = 0; i < 400; ++i) {
        checked.Add((i * 7919) % 10000);
        assert(checked.summaryDirty);
        std::vector<uint64_t> sorted(checked.ticks, checked.ticks + checked.count);
        std::sort(sorted.begin(), sorted.end());
        const auto result = Summarize(checked);
        const auto count = sorted.size();
        auto us = [](uint64_t value) { return (uint32_t)std::max(uint64_t{1}, value); };
        assert(result.count == count && result.minimum == us(sorted.front()) &&
            result.median == us(sorted[(count - 1) / 2]) &&
            result.p95 == us(sorted[(count * 95 + 99) / 100 - 1]) &&
            result.maximum == us(sorted.back()));
        assert(!checked.summaryDirty);
        const auto cached = Summarize(checked);
        assert(std::memcmp(&result, &cached, sizeof(result)) == 0);
    }
    checked = {};
    assert(Summarize(checked).count == 0);

    Samples activeSamples[Metrics];
    for (size_t metric = 0; metric < Metrics; ++metric)
        for (uint64_t i = 0; i < Window; ++i)
            activeSamples[metric].Add(1000 + (i * 7919 + metric * 31) % 10000);
    volatile uint32_t summarySink = 0;
    const auto summaryStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 20000; ++i) {
        // Live/clean frames change every report; rollback history may be unchanged.
        activeSamples[(size_t)LiveBenchmarkMetric::Live].Add(1000 + i % 10000);
        activeSamples[(size_t)LiveBenchmarkMetric::CleanLive].Add(1000 + i % 10000);
        for (const auto& history : activeSamples) summarySink = Summarize(history).p95;
    }
    const auto summaryNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - summaryStart).count();
    assert(summarySink != 0);
    printf("Active six-metric summary (two histories changing): %.1f ns/report.\n",
        (double)summaryNs / 20000);

    Packet displayPackets[2];
    for (size_t side = 0; side < 2; ++side) {
        displayPackets[side].flags = 1u | (7u << 8);
        for (auto& cost : displayPackets[side].costs)
            cost = {120, 1000, 2000, 3000 + (uint32_t)side * 1000, 5000};
        DrawRate(Cost(displayPackets[side], LiveBenchmarkMetric::Live), true, true, 0, true, side);
        DrawResim(displayPackets[side], true, true, true, side);
    }
    numericFormats = 0;
    const auto displayStart = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 20000; ++i) {
        ImGui::text.clear(); ImGui::bars.clear(); ImGui::barColors.clear();
        for (size_t side = 0; side < 2; ++side) {
            const auto& packet = displayPackets[side];
            DrawRate(Cost(packet, LiveBenchmarkMetric::Live), true, true, 0, true, side);
            DrawResim(packet, true, true, true, side);
        }
    }
    const auto displayNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - displayStart).count();
    printf("Four expanded capacity cells: %.1f ns/frame (stubbed ImGui, includes formatting).\n",
        (double)displayNs / 20000);
    assert(numericFormats == 0); // No float formatting for unchanged samples.
    DrawRate(Cost(displayPackets[0], LiveBenchmarkMetric::Live), true, false, 0, false, 0);
    assert(numericFormats == 0 && ImGui::barColors.back().x == HealthColor(FrameHealth::NoData).x);
    displayPackets[0].recentLive = 12345;
    DrawRate(Cost(displayPackets[0], LiveBenchmarkMetric::Live), true, true, 12345, true, 0);
    assert(numericFormats == 3);
    displayPackets[1].flags = 5u << 8;
    DrawResim(displayPackets[1], true, true, true, 1);
    assert(numericFormats == 6 && HasText("5f:"));

    // Native microbenchmark of the handoff only, not Windows/game-frame overhead.
    constexpr uint32_t iterations = 1000000;
    auto beforePublish = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iterations; ++i) Publish();
    const auto publishNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - beforePublish).count();
    ReadPublished();
    volatile uint64_t sink = 0;
    auto beforeRead = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iterations; ++i) sink = ReadPublished().localTime;
    const auto readNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - beforeRead).count();
    assert(sink == g_localTime);
    printf("Snapshot handoff: %.1f ns/publish, %.1f ns/cached read (native harness); "
           "%u coherent reads during %u concurrent publications.\n",
        (double)publishNs / iterations, (double)readNs / iterations, observed, publications);
}
'''
        with tempfile.TemporaryDirectory(prefix="live-benchmark-", dir="/tmp/opencode") as temporary:
            cpp = Path(temporary) / "test.cpp"
            binary = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror",
                            "-I", str(CODE), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_single_dispatcher_and_timing_boundaries(self):
        roster = (CODE / "chrsel_imgui.cpp").read_text()
        self.assertNotIn("Netplay_GetExtensionPacket(", roster)
        self.assertIn("packet.magic != CHRSEL_MAGIC", roster)
        hooks = (CODE / "rollback_hooks.cpp").read_text()
        self.assertIn("g_isResimulating ? 0 :", hooks)
        self.assertLess(hooks.index("oMainMove(main);"), hooks.index("LiveBenchmark_EndPacing"))
        save = (CODE / "rollback_snapshot.cpp").read_text()
        self.assertLess(save.index("SnapDump_CaptureSnapshot(slot)"),
                        save.index("LiveBenchmark_Record(LiveBenchmarkMetric::Save"))
        net = (CODE / "netplay.cpp").read_text()
        self.assertIn("case PACKET_BENCHMARK:", net)
        self.assertIn("ENET_PACKET_FLAG_RELIABLE", net)
        self.assertIn("extension.reliable ? ENET_PACKET_FLAG_RELIABLE : 0", net)
        self.assertIn("g_outboundExtension[tail].reliable = reliable", net)
        escape = (CODE / "escmenu_imgui.cpp").read_text()
        self.assertNotIn("##EscMenuConnection", escape)
        benchmark = (CODE / "live_benchmark.cpp").read_text()
        self.assertIn("Steam connection: %s", benchmark)
        self.assertNotIn("SRWLock", benchmark)
        self.assertNotIn("CRITICAL_SECTION", benchmark)


if __name__ == "__main__":
    unittest.main()
