#include "../pch.h"
#include "../imgui/imgui.h"
#include "live_benchmark.h"
#include "lag_diagnostics.h"
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
#include <cstdio>
#include <cstring>

extern void ChrSelImGui_HandleExtension(const void* data, size_t size);
extern bool g_multiResimEnabled;

namespace {
constexpr uint32_t Magic = 0x314d424c; // LBM1
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
    uint32_t magic = Magic;
    uint32_t version = 2;
    uint32_t kind = Summary;
    uint32_t generation = 0;
    uint32_t sequence = 0;
    uint32_t flags = 0;
    Costs costs[Metrics] = {};
    uint32_t recentLive = 0, recentRollback = 0;
};
static_assert(sizeof(Packet) == 156 && sizeof(Packet) <= 256);

struct Samples {
    uint64_t ticks[Window] = {};
    size_t count = 0, next = 0;
    mutable Costs summary{};
    mutable bool summaryDirty = true;
    void Add(uint64_t value) {
        ticks[next] = value;
        next = (next + 1) % Window;
        count = std::min(count + 1, Window);
        summaryDirty = true;
    }
};

uint64_t Counter()
{
    LagDiagnostics_Event(LagEvent::BenchmarkClock);
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
    if (!samples.summaryDirty) return samples.summary;
    const size_t count = std::min(samples.count, Window);
    if (!count) return {};
    uint64_t sorted[Window];
    std::copy_n(samples.ticks, count, sorted);
    std::sort(sorted, sorted + count);
    auto us = [](uint64_t ticks) {
        return (uint32_t)std::min(4294967295.0,
            std::max(1.0, (double)ticks * 1000000.0 / (double)Frequency()));
    };
    samples.summary = {(uint32_t)count, us(sorted[0]),
        us(sorted[(count - 1) / 2]),
        us(sorted[(count * 95 + 99) / 100 - 1]),
        us(sorted[count - 1])};
    samples.summaryDirty = false;
    return samples.summary;
}

uint32_t RecentCost(const Samples& samples)
{
    if (!samples.count) return 0;
    uint64_t maximum = 0;
    for (size_t i = 0; i < std::min(samples.count, size_t{2}); ++i)
        maximum = std::max(maximum, samples.ticks[(samples.next + Window - 1 - i) % Window]);
    return (uint32_t)std::min(4294967295.0,
        std::max(1.0, (double)maximum * 1000000.0 / (double)Frequency()));
}

Samples g_samples[Metrics]; // Owned solely by the game thread.
Samples g_pending[(size_t)LiveBenchmarkMetric::Live];
bool g_active = false, g_corrected = false;
bool g_resimSampling = false;
uint64_t g_nextResimSample = 0;
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
std::atomic<bool> g_menuOpen{false};
uint64_t g_pendingShowUntil = 0;
uint64_t g_summaryRequestedUntil = 0;
uint64_t g_nextRequestedSend = 0;

bool SamplingRequested(uint64_t now)
{
    return now < g_showUntil.load(std::memory_order_relaxed) ||
        (Netplay_IsActive() && g_menuOpen.load(std::memory_order_relaxed));
}

bool HasSamplingRequest()
{
    // No clock or game-state queries on the usual disabled path.
    return g_showUntil.load(std::memory_order_relaxed) != 0 ||
        (g_menuOpen.load(std::memory_order_relaxed) && Netplay_IsActive());
}

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
    LagDiagnostics_Event(LagEvent::BenchmarkReset);
    for (auto& samples : g_samples) samples = {};
    g_active = false;
    g_resimSampling = false;
    g_nextResimSample = 0;
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
    if (packet.type != PACKET_BENCHMARK || packet.magic != Magic ||
        packet.version != 2 || packet.generation != g_generation ||
        (packet.kind != Summary && packet.kind != Show) ||
        (packet.flags & ~0x1f07u) || (packet.flags >> 8) > 16)
        return false;
    for (const auto& c : packet.costs) {
        if (c.count > Window) return false;
        if (!c.count) {
            if (c.minimum || c.median || c.p95 || c.maximum) return false;
        } else if (!c.minimum || c.minimum > c.median || c.median > c.p95 ||
                   c.p95 > c.maximum) return false;
    }
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

uint64_t RollbackCost(const Packet& packet, uint32_t Costs::*field)
{
    const uint32_t depth = packet.flags >> 8;
    const auto& restore = Cost(packet, LiveBenchmarkMetric::Restore);
    const auto& tick = Cost(packet, LiveBenchmarkMetric::Headless);
    const auto& save = Cost(packet, LiveBenchmarkMetric::Save);
    const bool correctedSaves = (packet.flags & 1) != 0;
    if (!depth || !restore.count || !tick.count || (correctedSaves && !save.count))
        return 0;
    const uint64_t perTick = (uint64_t)(tick.*field) + (correctedSaves ? save.*field : 0);
    return (uint64_t)(restore.*field) + depth * perTick;
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

void DrawCapacityValues(uint64_t cost, uint64_t detail, uint64_t minimum, uint64_t maximum,
                        uint32_t depth, bool resim, bool fresh, bool expanded, size_t side)
{
    // Render-thread cache: numerical text changes with samples, not display frames.
    struct Display {
        uint64_t cost = 0, detail = 0, minimum = 0, maximum = 0;
        uint32_t depth = 0;
        double percent = 0;
        char headline[64]{}, timing[64]{}, range[64]{};
    };
    static Display cached[2][2];
    auto& display = cached[resim][side];
    if (display.cost != cost || display.detail != detail || display.minimum != minimum ||
        display.maximum != maximum || display.depth != depth) {
        display.cost = cost;
        display.detail = detail;
        display.minimum = minimum;
        display.maximum = maximum;
        display.depth = depth;
        display.percent = resim ? 1666700.0 / cost : (1000000.0 / cost) / 60.0 * 100.0;
        const double fps = resim ? display.percent * 60.0 / 100.0 : 1000000.0 / cost;
        std::snprintf(display.headline, sizeof(display.headline), "%.0f%% (%.0f FPS)",
                      display.percent, fps);
        if (resim) {
            std::snprintf(display.timing, sizeof(display.timing), "%uf: %.1f ms est.",
                          depth, detail / 1000.0);
            std::snprintf(display.range, sizeof(display.range), "Low:%.0f%%  High:%.0f%%",
                          1666700.0 / maximum, 1666700.0 / minimum);
        } else {
            std::snprintf(display.timing, sizeof(display.timing), "Slow: %.1f ms", detail / 1000.0);
            std::snprintf(display.range, sizeof(display.range), "Low:%.0f  High:%.0f",
                          1000000.0 / maximum, 1000000.0 / minimum);
        }
    }
    const ImVec4 color = CapacityColor(display.percent, fresh);
    ImGui::TextColored(color, "%s", display.headline);
    DrawCapacityBar(display.percent, color);
    if (!expanded) return;
    ImGui::TextDisabled("%s", display.timing);
    ImGui::TextDisabled("%s", display.range);
}

void DrawResim(const Packet& packet, bool available, bool fresh, bool expanded = true,
               size_t side = 0)
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
    const uint64_t cost = packet.recentRollback ? packet.recentRollback :
        RollbackCost(packet, &Costs::p95);
    if (!cost) {
        ImGui::TextDisabled("Waiting resim");
        return;
    }
    DrawCapacityValues(cost, cost, RollbackCost(packet, &Costs::minimum),
        RollbackCost(packet, &Costs::maximum), depth, true, fresh, expanded, side);
}

void DrawRate(const Costs& cost, bool available, bool fresh, uint32_t recent = 0,
               bool expanded = true, size_t side = 0)
{
    if (!available || !cost.count) {
        ImGui::TextDisabled("N/A");
        return;
    }
    DrawCapacityValues(recent ? recent : cost.p95, cost.p95, cost.minimum, cost.maximum,
                       0, false, fresh, expanded, side);
}

void Assessment(const Packet& packet, bool fresh)
{
    const auto& live = Cost(packet, LiveBenchmarkMetric::Live);
    const FrameHealth health = Health(live, fresh);
    static const char* labels[] = {"NO RECENT DATA", "CHECKING", "Good", "Stutter", "SLOW"};
    ImGui::TextColored(HealthColor(health), "%s", labels[(size_t)health]);
    if (health != FrameHealth::Smooth) return;
    const auto& clean = Cost(packet, LiveBenchmarkMetric::CleanLive);
    const uint64_t cost = RollbackCost(packet, &Costs::p95);
    if (!(packet.flags >> 8)) {
        ImGui::TextDisabled("Rollback: off");
    } else if (clean.count < 2 || !cost) {
        ImGui::TextDisabled("Rollback: untested");
    } else {
        const uint64_t estimate = (uint64_t)clean.p95 + cost;
        ImGui::TextColored(estimate <= 16667 ? ImVec4(0.57f, 0.81f, 0.65f, 1.0f)
                                           : ImVec4(0.88f, 0.76f, 0.49f, 1.0f),
            "%s", estimate <= 16667 ? "Rollback: good" : "Rollback: may lag");
    }
}
} // namespace

uint64_t LiveBenchmark_BeginFrame(int32_t frame)
{
    LagDiagnosticScope diagnostic(LagMetric::BenchmarkBegin);
    g_active = false;
    if (!HasSamplingRequest()) return 0;
    if (g_lastFrame >= 0 && frame != g_lastFrame) Clear();
    const uint64_t now = GetTickCount64();
    if (!SamplingRequested(now)) return 0;
    if (now < g_nextSample) return 0;
    if (frame < 0 || FrameStepper_IsMatchRestartPending() ||
        (Netplay_IsActive() && !FrameStepper_IsDesyncCheckReady())) return 0;
    const uint64_t action = ((uint64_t(__fastcall*)())_addr(0x140001A90))();
    if (!action || *(int32_t*)(action + 0xD8) < 3 ||
        *(int32_t*)(action + 0xD8) == 12) return 0;
    g_nextSample = now + SampleIntervalMs;
    g_frame = frame;
    g_corrected = false;
    g_pacing = 0;
    for (auto& samples : g_pending) samples.count = samples.next = 0;
    g_active = true;
    LagDiagnostics_Event(LagEvent::LiveSample);
    return Counter();
}

uint64_t LiveBenchmark_Now()
{
    return g_active || g_resimSampling ? Counter() : 0;
}

uint64_t LiveBenchmark_BeginResim()
{
    g_corrected = true;
    g_resimSampling = false;
    if (!HasSamplingRequest()) return 0;
    const uint64_t now = GetTickCount64();
    g_resimSampling = SamplingRequested(now) && now >= g_nextResimSample;
    if (!g_resimSampling) return 0;
    g_nextResimSample = now + SampleIntervalMs;
    LagDiagnostics_Event(LagEvent::CorrectionSample);
    return Counter();
}

void LiveBenchmark_EndResim(uint64_t start)
{
    LiveBenchmark_Record(LiveBenchmarkMetric::Correction, start);
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
    if (metric == LiveBenchmarkMetric::Correction) g_corrected = true;
}

void LiveBenchmark_EndPacing(uint64_t start)
{
    if (start && g_active) g_pacing += Counter() - start;
}

void LiveBenchmark_EndFrame(uint64_t start, int32_t frame)
{
    LagDiagnosticScope diagnostic(LagMetric::BenchmarkEnd);
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
    if (!g_corrected)
        g_samples[(size_t)LiveBenchmarkMetric::CleanLive].Add(elapsed - g_pacing);
    Packet packet;
    packet.generation = g_generation;
    packet.sequence = ++g_sequence;
    packet.flags = g_flags;
    for (size_t i = 0; i < Metrics; ++i) packet.costs[i] = Summarize(g_samples[i]);
    packet.recentLive = RecentCost(g_samples[(size_t)LiveBenchmarkMetric::Live]);
    Packet recent = packet;
    for (size_t i = 0; i < Metrics; ++i)
        recent.costs[i].p95 = RecentCost(g_samples[i]);
    packet.recentRollback = (uint32_t)std::min<uint64_t>(UINT32_MAX,
        RollbackCost(recent, &Costs::p95));
    g_local = packet;
    g_localTime = GetTickCount64();
    Publish();
}

void LiveBenchmark_Update()
{
    LagDiagnosticScope diagnostic(LagMetric::BenchmarkUpdate);
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
    uint64_t now = HasSamplingRequest() ? GetTickCount64() : 0;
    uint8_t bytes[256];
    size_t size;
    bool peerChanged = false;
    while (Netplay_GetExtensionPacket(bytes, sizeof(bytes), size)) {
        uint32_t magic = 0;
        if (size >= 8) std::memcpy(&magic, bytes + 4, sizeof(magic));
        if (magic != Magic) {
            ChrSelImGui_HandleExtension(bytes, size);
            continue;
        }
        if (size != sizeof(Packet)) continue;
        Packet packet;
        std::memcpy(&packet, bytes, sizeof(packet));
        if (!Valid(packet)) continue;
        if (!now) now = GetTickCount64();
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
        if (!now) now = GetTickCount64();
        ++g_showSequence;
        g_pendingShowUntil = now + 5000;
        g_summaryRequestedUntil = now + 8000;
        g_nextRequestedSend = 0;
    }
    if (!connected || !now || !SamplingRequested(now)) return;
    if (!g_nextSend) g_nextSend = now + NextReportDelay();
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
    LagDiagnosticScope diagnostic(LagMetric::BenchmarkDraw);
    const bool requested = !ImGui::GetIO().WantTextInput && KeyPressed('0');
    const bool menu = EscMenuImGui_IsOpen();
    // Esc-menu state belongs to the render thread; sampling runs on the game thread.
    if (g_menuOpen.load(std::memory_order_relaxed) != menu)
        g_menuOpen.store(menu, std::memory_order_relaxed);
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
            ImGui::TextUnformatted(live ? "Game+Draw" : "Rollback");
            if (live && expanded) ImGui::TextDisabled("16.7 ms limit");
            for (int side = 0; side < 2; ++side) {
                ImGui::TableNextColumn();
                const auto& packet = side ? peer : local;
                const bool available = !side || peerTime != 0;
                const bool fresh = side ? peerFresh : localFresh;
                if (live) DrawRate(Cost(packet, metric), available, fresh, packet.recentLive, expanded, side);
                else DrawResim(packet, available, fresh, expanded, side);
            }
        }
        if (expanded) for (const auto metric : {LiveBenchmarkMetric::Save, LiveBenchmarkMetric::Restore,
                                  LiveBenchmarkMetric::Correction}) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(metric == LiveBenchmarkMetric::Save ? "Save" :
                metric == LiveBenchmarkMetric::Restore ? "Restore" : "Rollback");
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
