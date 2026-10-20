#include "../pch.h"
#include "../imgui/imgui.h"
#include "live_benchmark.h"
#include "escmenu_imgui.h"
#include "framestepper.h"
#include "netplay.h"
#include "mk10utils.h"
#include "d3d9reset.h"
#include "steam_transport.h"
#include "steam_lobby.h"
#include "LaunchMenu_settings.h"
#include "controller.h"
#include <algorithm>
#include <atomic>
#include <cstring>

extern void ChrSelImGui_HandleExtension(const void* data, size_t size);
extern bool g_multiResimEnabled;

namespace {
constexpr size_t Window = 120;
constexpr size_t Metrics = (size_t)LiveBenchmarkMetric::Count;
constexpr uint64_t SampleIntervalMs = 1000;
constexpr uint64_t LocalFreshMs = 2500;
constexpr uint64_t PeerFreshMs = 12000;
enum : uint32_t { Summary = 1, Show = 2 };

struct Costs {
    uint32_t count, minimum, median, p95, maximum; // Integer microseconds.
};
struct Packet {
    uint32_t type = PACKET_BENCHMARK;
    uint32_t kind = Summary;
    uint32_t generation = 0;
    uint32_t sequence = 0;
    uint32_t flags = 0;
    Costs costs[Metrics] = {};
    uint32_t recentLive = 0, recentRollback = 0;
    uint32_t corrections = 0, replayedFrames = 0;
    uint32_t rollbackGame = 0;
    Costs paired = {};
};
static_assert(sizeof(Packet) == 160 && sizeof(Packet) <= 256);

struct Samples {
    uint64_t ticks[Window] = {};
    size_t count = 0, next = 0;
    void Add(uint64_t value) {
        ticks[next] = value;
        next = (next + 1) % Window;
        count = std::min(count + 1, Window);
    }
};

uint64_t Counter()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

uint64_t Frequency()
{
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return (uint64_t)value.QuadPart;
    }();
    return frequency;
}

Costs Summarize(const Samples& samples)
{
    const size_t count = std::min(samples.count, Window);
    if (!count) return {};
    uint64_t sorted[Window];
    std::copy_n(samples.ticks, count, sorted);
    std::sort(sorted, sorted + count);
    auto us = [](uint64_t ticks) {
        return (uint32_t)std::min(4294967295.0,
            std::max(1.0, (double)ticks * 1000000.0 / (double)Frequency()));
    };
    return {(uint32_t)count, us(sorted[0]),
        us(sorted[(count - 1) / 2]),
        us(sorted[(count * 95 + 99) / 100 - 1]),
        us(sorted[count - 1])};
}

uint32_t RecentCost(const Samples& samples)
{
    Samples recent;
    for (size_t i = 0; i < std::min(samples.count, size_t{2}); ++i)
        recent.Add(samples.ticks[(samples.next + Window - 1 - i) % Window]);
    return Summarize(recent).maximum;
}

Samples g_samples[Metrics]; // Owned solely by the game thread.
Samples g_pending[(size_t)LiveBenchmarkMetric::Live];
Samples g_pairedSamples;
uint64_t g_pairCorrection = 0;
int32_t g_pairFrame = -1;
bool g_active = false;
bool g_resimSampling = false;
uint64_t g_nextResimSample = 0;
uint32_t g_corrections = 0, g_replayedFrames = 0;
bool g_connected = false;
uint32_t g_generation = 0, g_flags = 0, g_sequence = 0, g_showSequence = 0;
uint32_t g_peerSequence = 0, g_peerShowSequence = 0;
int32_t g_frame = -1, g_lastFrame = -1;
uint64_t g_pacing = 0, g_nextSample = 0, g_nextSend = 0;
Packet g_local, g_peer;
uint64_t g_localTime = 0, g_peerTime = 0;
struct alignas(64) DisplaySnapshot {
    Packet local, peer;
    uint64_t localTime = 0, peerTime = 0;
    bool connected = false;
};
DisplaySnapshot g_display[3];
uint32_t g_writeSlot = 0; // Game thread owns this slot.
uint32_t g_readSlot = 1;  // Serialized EndScene callbacks own this slot.
alignas(64) std::atomic<uint32_t> g_middleSlot{2};
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
              std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free);
std::atomic<uint64_t> g_showUntil{0};
std::atomic<bool> g_showRequested{false};
uint64_t g_pendingShowUntil = 0;
uint64_t g_summaryRequestedUntil = 0;
uint64_t g_nextRequestedSend = 0;

void Publish()
{
    g_display[g_writeSlot] = {g_local, g_peer, g_localTime, g_peerTime, g_connected};
    // Exchange only the spare slot; neither side can overwrite the other's data.
    g_writeSlot = g_middleSlot.exchange(g_writeSlot | 4u, std::memory_order_acq_rel) & 3u;
}

const DisplaySnapshot& ReadPublished()
{
    // EndScene's existing guard supplies a single consumer. Valid until its next read.
    if (g_middleSlot.load(std::memory_order_acquire) & 4u)
        g_readSlot = g_middleSlot.exchange(g_readSlot, std::memory_order_acq_rel) & 3u;
    return g_display[g_readSlot];
}

uint64_t NextReportDelay()
{
    // Private telemetry jitter, never the game's deterministic RNG.
    static uint32_t random = (uint32_t)(GetTickCount64() ^ GetCurrentProcessId()) | 1u;
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return 5000 + random % 5001;
}

void Clear()
{
    for (auto& samples : g_samples) samples = {};
    g_pairedSamples = {};
    g_pairCorrection = 0;
    g_pairFrame = -1;
    g_active = false;
    g_resimSampling = false;
    g_nextResimSample = 0;
    g_corrections = g_replayedFrames = 0;
    g_lastFrame = -1;
    g_nextSample = g_nextSend = 0;
    g_peerSequence = g_peerShowSequence = 0;
    g_pendingShowUntil = 0;
    g_summaryRequestedUntil = 0;
    g_nextRequestedSend = 0;
    // Keep the final reports offline, but never carry them into a new connection.
    if (g_connected) {
        g_local = {};
        g_local.generation = g_generation;
        g_peer = {};
        g_localTime = g_peerTime = 0;
    }
    Publish();
}

bool Valid(const Packet& packet)
{
    if (packet.type != PACKET_BENCHMARK || packet.generation != g_generation ||
        (packet.kind != Summary && packet.kind != Show) ||
        (packet.flags & ~0x1f07u) || (packet.flags >> 8) > 16)
        return false;
    if (packet.corrections > packet.replayedFrames ||
        (!packet.corrections && packet.replayedFrames)) return false;
    for (size_t i = 0; i <= Metrics; ++i) {
        const auto& c = i == Metrics ? packet.paired : packet.costs[i];
        if (c.count > Window) return false;
        if (!c.count) {
            if (c.minimum || c.median || c.p95 || c.maximum) return false;
        } else if (!c.minimum || c.minimum > c.median || c.median > c.p95 ||
                   c.p95 > c.maximum) return false;
    }
    if (packet.paired.count ? (!packet.recentRollback || !packet.rollbackGame) :
        (packet.recentRollback || packet.rollbackGame)) return false;
    return true;
}

const Costs& Cost(const Packet& packet, LiveBenchmarkMetric metric)
{
    return packet.costs[(size_t)metric];
}

enum class FrameHealth { NoData, Checking, Smooth, Spikes, Slow };

FrameHealth Health(const Costs& cost, bool fresh)
{
    if (!fresh || !cost.count) return FrameHealth::NoData;
    if (cost.count < 2) return FrameHealth::Checking;
    if (cost.median > 17167) return FrameHealth::Slow;
    if (cost.p95 > 17167) return FrameHealth::Spikes;
    return FrameHealth::Smooth;
}

ImVec4 HealthColor(FrameHealth health)
{
    if (health == FrameHealth::Slow) return ImVec4(0.95f, 0.50f, 0.44f, 1.0f);
    if (health == FrameHealth::Spikes) return ImVec4(0.95f, 0.76f, 0.40f, 1.0f);
    if (health == FrameHealth::Smooth) return ImVec4(0.57f, 0.85f, 0.65f, 1.0f);
    return ImVec4(0.64f, 0.63f, 0.57f, 1.0f);
}

ImVec4 CapacityColor(double percent, bool fresh)
{
    if (!fresh) return HealthColor(FrameHealth::NoData);
    if (percent <= 60.0) return HealthColor(FrameHealth::Slow);
    if (percent < 100.0) return ImVec4(0.95f, 0.65f, 0.30f, 1.0f);
    return HealthColor(FrameHealth::Smooth);
}

void DrawCapacityBar(double percent, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar((float)std::min(1.0, percent / 200.0), ImVec2(-1, 5), "");
    ImGui::PopStyleColor();
    const ImVec2 begin = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
    const float targetX = (begin.x + end.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(targetX, begin.y - 1),
        ImVec2(targetX, end.y + 1), ImGui::GetColorU32(ImGuiCol_Text));
}

void DrawResim(const Packet& packet, bool available, bool fresh, bool expanded = true)
{
    if (!available || !Cost(packet, LiveBenchmarkMetric::Live).count) {
        ImGui::TextDisabled("N/A");
        return;
    }
    const uint32_t depth = packet.flags >> 8;
    if (!depth) {
        ImGui::TextDisabled("Off");
        return;
    }
    if (!packet.paired.count) {
        ImGui::TextDisabled("Waiting resim");
        return;
    }
    const uint64_t cost = (uint64_t)packet.rollbackGame + packet.recentRollback;
    const double fps = 1000000.0 / cost;
    const double percent = fps / 60.0 * 100.0;
    const ImVec4 color = !fresh ? HealthColor(FrameHealth::NoData) :
        percent < 75.0 ? HealthColor(FrameHealth::Slow) :
        percent < 95.0 ? ImVec4(0.95f, 0.65f, 0.30f, 1.0f) : HealthColor(FrameHealth::Smooth);
    ImGui::TextColored(color, "%.0f%% (%.0f FPS)", percent, fps);
    DrawCapacityBar(percent, color);
    if (!expanded) return;
    ImGui::TextDisabled("Total: %.1f ms", cost / 1000.0);
    ImGui::TextDisabled("Low:%.1f ms  High:%.1f ms",
        packet.paired.minimum / 1000.0, packet.paired.maximum / 1000.0);
}

void DrawRate(const Costs& cost, bool available, bool fresh, uint32_t recent = 0,
              bool expanded = true)
{
    if (!available || !cost.count) {
        ImGui::TextDisabled("N/A");
        return;
    }
    const double fps = 1000000.0 / (recent ? recent : cost.p95);
    const double percent = fps / 60.0 * 100.0;
    const ImVec4 color = CapacityColor(percent, fresh);
    ImGui::TextColored(color, "%.0f%% (%.0f FPS)", percent, fps);
    DrawCapacityBar(percent, color);
    if (!expanded) return;
    ImGui::TextDisabled("p95: %.1f ms", cost.p95 / 1000.0);
    ImGui::TextDisabled("Low:%.1f ms  High:%.1f ms", cost.minimum / 1000.0,
                        cost.maximum / 1000.0);
}

void Assessment(const Packet& packet, bool fresh)
{
    const auto& live = Cost(packet, LiveBenchmarkMetric::Live);
    const FrameHealth health = Health(live, fresh);
    static const char* labels[] = {"NO RECENT DATA", "CHECKING", "Good", "Stutter", "SLOW"};
    ImGui::TextColored(HealthColor(health), "%s", labels[(size_t)health]);
    if (health != FrameHealth::Smooth) return;
    if (!(packet.flags >> 8)) {
        ImGui::TextDisabled("Rollback: off");
    } else if (!packet.paired.count) {
        ImGui::TextDisabled("Rollback: untested");
    } else {
        const uint64_t estimate = (uint64_t)packet.rollbackGame + packet.recentRollback;
        ImGui::TextColored(estimate <= 16667 ? ImVec4(0.57f, 0.81f, 0.65f, 1.0f)
                                           : ImVec4(0.88f, 0.76f, 0.49f, 1.0f),
            "%s", estimate <= 16667 ? "Rollback: good" : "Rollback: may lag");
    }
}
} // namespace

uint64_t LiveBenchmark_BeginFrame(int32_t frame)
{
    g_active = false;
    if (g_lastFrame >= 0 && frame != g_lastFrame) Clear();
    g_frame = frame;
    if (g_pairFrame != frame) g_pairCorrection = 0;
    const uint64_t now = GetTickCount64();
    if (now < g_nextSample && !g_pairCorrection) return 0;
    if (frame < 0 || FrameStepper_IsMatchRestartPending() ||
        (Netplay_IsActive() && !FrameStepper_IsDesyncCheckReady())) {
        g_pairCorrection = 0;
        return 0;
    }
    const uint64_t action = ((uint64_t(__fastcall*)())_addr(0x140001A90))();
    if (!action || *(int32_t*)(action + 0xD8) < 3 ||
        *(int32_t*)(action + 0xD8) == 12) {
        g_pairCorrection = 0;
        return 0;
    }
    g_nextSample = now + SampleIntervalMs;
    g_frame = frame;
    g_pacing = 0;
    for (auto& samples : g_pending) samples.count = samples.next = 0;
    g_active = true;
    return Counter();
}

uint64_t LiveBenchmark_Now()
{
    return g_active || g_resimSampling ? Counter() : 0;
}

uint64_t LiveBenchmark_BeginResim(int32_t frames, int32_t endFrame)
{
    if (frames <= 0) return 0;
    ++g_corrections;
    g_replayedFrames += (uint32_t)frames;
    // Never pair only part of a frame containing multiple corrections.
    g_pairCorrection = 0;
    const uint64_t now = GetTickCount64();
    g_resimSampling = now >= g_nextResimSample;
    if (!g_resimSampling) return 0;
    g_nextResimSample = now + SampleIntervalMs;
    g_pairFrame = endFrame;
    g_pairCorrection = 0;
    return Counter();
}

void LiveBenchmark_EndResim(uint64_t start)
{
    if (start && g_resimSampling) {
        g_pairCorrection = Counter() - start;
        g_samples[(size_t)LiveBenchmarkMetric::Correction].Add(g_pairCorrection);
    }
    g_resimSampling = false;
}

void LiveBenchmark_Record(LiveBenchmarkMetric metric, uint64_t start)
{
    if (!start || (!g_active && !g_resimSampling)) return;
    const size_t index = (size_t)metric;
    if (index >= (size_t)LiveBenchmarkMetric::Live) return;
    // Resim is sampled independently of the once-per-second live frame.
    if (g_resimSampling) g_samples[index].Add(Counter() - start);
    else g_pending[index].Add(Counter() - start);
}

void LiveBenchmark_EndPacing(uint64_t start)
{
    if (start && g_active) g_pacing += Counter() - start;
}

void LiveBenchmark_EndFrame(uint64_t start, int32_t frame)
{
    const uint64_t correction = g_pairCorrection;
    g_pairCorrection = 0;
    if (!start) {
        g_lastFrame = frame;
        return;
    }
    if (!g_active) return;
    const uint64_t end = Counter();
    g_active = false;
    if (frame == g_frame) return;
    if (frame != g_frame + 1 || FrameStepper_IsMatchRestartPending()) {
        Clear();
        return;
    }
    g_lastFrame = frame;
    const uint64_t elapsed = end - start;
    if (elapsed <= g_pacing) return;
    // Commit only advancing frames, leaving the previous results intact on pause.
    for (size_t i = 0; i < (size_t)LiveBenchmarkMetric::Live; ++i) {
        const auto& pending = g_pending[i];
        for (size_t j = 0; j < pending.count; ++j)
            g_samples[i].Add(pending.ticks[(pending.next + Window - pending.count + j) % Window]);
    }
    g_samples[(size_t)LiveBenchmarkMetric::Live].Add(elapsed - g_pacing);
    Packet packet;
    packet.generation = g_generation;
    packet.sequence = ++g_sequence;
    packet.flags = g_flags;
    packet.corrections = g_corrections;
    packet.replayedFrames = g_replayedFrames;
    g_corrections = g_replayedFrames = 0;
    for (size_t i = 0; i < Metrics; ++i) packet.costs[i] = Summarize(g_samples[i]);
    packet.recentLive = RecentCost(g_samples[(size_t)LiveBenchmarkMetric::Live]);
    if (correction) {
        g_pairedSamples.Add(correction + elapsed - g_pacing);
        packet.paired = Summarize(g_pairedSamples);
        packet.recentRollback = (uint32_t)std::clamp(
            (double)correction * 1000000.0 / Frequency(), 1.0, (double)UINT32_MAX);
        packet.rollbackGame = (uint32_t)std::clamp(
            (double)(elapsed - g_pacing) * 1000000.0 / Frequency(), 1.0, (double)UINT32_MAX);
    } else if (g_pairedSamples.count) {
        packet.paired = g_local.paired;
        packet.recentRollback = g_local.recentRollback;
        packet.rollbackGame = g_local.rollbackGame;
    }
    // A quiet interval is not a new resim result. Keep the last completed report.
    if (!packet.corrections && g_samples[(size_t)LiveBenchmarkMetric::Correction].count) {
        packet.corrections = g_local.corrections;
        packet.replayedFrames = g_local.replayedFrames;
        for (size_t i = 0; i < (size_t)LiveBenchmarkMetric::Live; ++i)
            packet.costs[i] = g_local.costs[i];
    }
    g_local = packet;
    g_localTime = GetTickCount64();
    Publish();
}

void LiveBenchmark_Update()
{
    const bool connected = Netplay_IsActive();
    const uint32_t generation = FrameStepper_GetBattleGeneration();
    const uint32_t flags = (g_multiResimEnabled ? 1u : 0u) |
        (D3D9Reset_IsRenderingThreadEnabled() ? 2u : 0u) |
        (IsGameSingleThreaded() ? 4u : 0u) |
        ((uint32_t)std::clamp(Controller_GetRollbackFrames(), 0, 16) << 8);
    if (connected != g_connected || generation != g_generation || flags != g_flags) {
        g_connected = connected;
        g_generation = generation;
        g_flags = flags;
        Clear();
    }
    const uint64_t now = GetTickCount64();
    if (!g_nextSend) g_nextSend = now + NextReportDelay();
    uint8_t bytes[256];
    size_t size;
    bool peerChanged = false;
    while (Netplay_GetExtensionPacket(bytes, sizeof(bytes), size)) {
        uint32_t type = 0;
        if (size < sizeof(type)) continue;
        std::memcpy(&type, bytes, sizeof(type));
        if (type != PACKET_BENCHMARK) {
            ChrSelImGui_HandleExtension(bytes, size);
            continue;
        }
        if (size != sizeof(Packet)) continue;
        Packet packet;
        std::memcpy(&packet, bytes, sizeof(packet));
        if (!Valid(packet)) continue;
        uint32_t& sequence = packet.kind == Show ? g_peerShowSequence : g_peerSequence;
        if ((int32_t)(packet.sequence - sequence) <= 0) continue;
        sequence = packet.sequence;
        if (packet.kind == Show) {
            g_showUntil.store(now + 8000, std::memory_order_relaxed);
            g_summaryRequestedUntil = now + 8000;
            g_nextRequestedSend = 0;
        } else {
            g_peer = packet;
            g_peerTime = now;
            peerChanged = true;
        }
    }
    if (peerChanged) Publish();
    if (g_showRequested.load(std::memory_order_relaxed) &&
        g_showRequested.exchange(false, std::memory_order_relaxed)) {
        ++g_showSequence;
        g_pendingShowUntil = now + 5000;
        g_summaryRequestedUntil = now + 8000;
        g_nextRequestedSend = 0;
    }
    if (!connected) return;
    if (g_pendingShowUntil > now) {
        Packet packet;
        packet.kind = Show;
        packet.generation = generation;
        packet.sequence = g_showSequence;
        if (Netplay_SendExtensionPacket(&packet, sizeof(packet))) g_pendingShowUntil = 0;
    }
    const bool requestedActive = now < g_summaryRequestedUntil;
    const bool requestedDue = requestedActive && now >= g_nextRequestedSend;
    const bool backgroundDue = !requestedActive && now >= g_nextSend;
    if (!backgroundDue && !requestedDue) return;
    if (backgroundDue) g_nextSend = now + NextReportDelay();
    if (g_localTime && now - g_localTime <= LocalFreshMs &&
        Netplay_SendExtensionPacket(&g_local, sizeof(g_local), requestedActive)) {
        if (requestedDue) g_nextRequestedSend = now + SampleIntervalMs;
        if (!backgroundDue) g_nextSend = now + NextReportDelay();
    }
}

void LiveBenchmark_Draw()
{
    const bool requested = !ImGui::GetIO().WantTextInput && KeyPressed('0');
    const bool menu = EscMenuImGui_IsOpen();
    const uint64_t until = g_showUntil.load(std::memory_order_relaxed);
    if (!requested && !menu && !until) return;
    const uint64_t now = GetTickCount64();
    if (requested) {
        g_showUntil.store(now + 8000, std::memory_order_relaxed);
        g_showRequested.store(true, std::memory_order_relaxed);
    } else if (!menu && now >= until) {
        uint64_t expired = until;
        g_showUntil.compare_exchange_strong(expired, 0, std::memory_order_relaxed);
        return;
    }
    const auto& snapshot = ReadPublished();
    const auto& local = snapshot.local;
    const auto& peer = snapshot.peer;
    const uint64_t localTime = snapshot.localTime, peerTime = snapshot.peerTime;
    const bool localFresh = localTime && (now < localTime || now - localTime <= LocalFreshMs);
    const bool peerFresh = snapshot.connected && peerTime &&
        (now < peerTime || now - peerTime <= PeerFreshMs);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 16.0f, 16.0f), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.07f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.38f, 0.35f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.87f, 0.78f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.64f, 0.63f, 0.57f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.18f, 0.15f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::Begin("Performance##LiveBenchmark", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    const float width = std::min(420.0f, std::max(240.0f, display.x - 56.0f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted("PC PERFORMANCE");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextUnformatted(
            "Press 0 to summon the performance chart midmatch."
            "The Game bar shows how long the regular game takes to run. "
            "If that's red, you can't even run Mahvel, What are you doing??\n\n"
            "The Rollback bar is how long the game takes to run, PLUS rollback. "
            "The more red it is, the fewer rollback frames your PC can handle across long distances! "
            "Expect it to turn red during projectile-heavy gameplay, but if it's red and your FPS is low, "
            "crank up the input delay.\n\n"
            "Performance will be improved over time. Please check back later if it's low!!");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    static bool expanded = false;
    ImGui::SameLine(width - 70.0f);
    if (ImGui::SmallButton(expanded ? "Collapse" : "Expand")) expanded = !expanded;
    static const char* connectionType = "Unknown";
    static uint64_t nextConnectionQuery = 0;
    if (SteamTransport_IsActive()) {
        if (now >= nextConnectionQuery) {
            connectionType = SteamLobby_GetConnectionType();
            nextConnectionQuery = now + 1000;
        }
        ImGui::TextDisabled("Steam connection: %s", connectionType);
    } else {
        nextConnectionQuery = 0;
    }
    ImGui::Separator();
    if (ImGui::BeginTable("##Costs", 3, ImGuiTableFlags_BordersInnerV, ImVec2(width, 0))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        const char* localName = SteamLobby_GetLocalUsername();
        const char* peerName = snapshot.connected && SteamTransport_IsActive()
            ? SteamLobby_GetPeerUsername() : nullptr;
        ImGui::TableSetupColumn(localName && localName[0] ? localName : "You");
        ImGui::TableSetupColumn(peerName && peerName[0] ? peerName :
            !snapshot.connected && peerTime ? "Opp. (left)" : "Opp.");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Status");
        ImGui::TableNextColumn();
        Assessment(local, localFresh);
        ImGui::TableNextColumn();
        if (!snapshot.connected && peerTime) ImGui::TextDisabled("Disconnected");
        else Assessment(peer, peerFresh);
        for (const auto metric : {LiveBenchmarkMetric::Live, LiveBenchmarkMetric::Headless}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool live = metric == LiveBenchmarkMetric::Live;
            ImGui::TextUnformatted(live ? "Game" : "Rollback");
            if (live && expanded) ImGui::TextDisabled("16.7 ms");
            if (!live) ImGui::TextDisabled("%u frames", local.corrections ?
                (uint32_t)(((uint64_t)local.replayedFrames + local.corrections - 1) / local.corrections) : 0u);
            for (int side = 0; side < 2; ++side) {
                ImGui::TableNextColumn();
                const auto& packet = side ? peer : local;
                const bool available = !side || peerTime != 0;
                const bool fresh = side ? peerFresh : localFresh;
                if (live) DrawRate(Cost(packet, metric), available, fresh, packet.recentLive, expanded);
                else DrawResim(packet, available, fresh, expanded);
            }
        }
        if (expanded) for (const auto metric : {LiveBenchmarkMetric::Save, LiveBenchmarkMetric::Restore,
                                  LiveBenchmarkMetric::Correction}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(metric == LiveBenchmarkMetric::Save ? "Save" :
                metric == LiveBenchmarkMetric::Restore ? "Restore" : "Resim");
            for (int side = 0; side < 2; ++side) {
                ImGui::TableNextColumn();
                const auto& cost = Cost(side ? peer : local, metric);
                if ((side && !peerTime) || !cost.count)
                    ImGui::TextDisabled("N/A");
                else
                    ImGui::TextDisabled("%.2f ms", cost.median / 1000.0);
            }
        }
        ImGui::EndTable();
    }
    if (expanded) {
        ImGui::Separator();
        ImGui::TextDisabled("Samples %u / %u  |  0: show both for 8s",
            Cost(local, LiveBenchmarkMetric::Live).count, Cost(peer, LiveBenchmarkMetric::Live).count);
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}
