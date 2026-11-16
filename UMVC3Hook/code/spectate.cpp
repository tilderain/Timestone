#include "../pch.h"
#include "spectate.h"

#include "steam_lobby.h"
#include "trainingboot.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {

// --- Tuning --------------------------------------------------------------------
// Catch-up runs frames without drawing them (Rollback_FastForward). Off, a
// spectator still works but stays as far behind as its loading took.
constexpr bool kFastForward = true;
constexpr int kResumeBuffer = 30;   // frames wanted before playing on after running dry
constexpr int kFastForwardStart = 90;  // further behind than this: catch up...
constexpr int kFastForwardTarget = 45; // ...to this
constexpr int kFastForwardPerTick = 8;
constexpr int kChecksumInterval = 60;
// How many spectators one member feeds. A fighting player's link competes
// with their match, so they take one; a spectator has nothing to protect.
constexpr int kPlayerChildren = 1;
constexpr int kViewerChildren = 2;
// Bytes sent to one child per frame. Only a late joiner's backlog ever gets
// near it; the live feed is a few hundred bytes a second.
constexpr int kPlayerBudget = 700;
constexpr int kViewerBudget = 12000;
constexpr int kFlushFrames = 12; // live frames batched into one message
constexpr int kMaxLinks = 8;
constexpr ULONGLONG kConnectTimeoutMs = 12000;
constexpr ULONGLONG kAvoidFailedMs = 8000;
constexpr ULONGLONG kLinkGraceMs = 3000;
// Longest game accepted from a feed (the game's own replay buffer holds five
// minutes; a result screen can add to that).
constexpr int32_t kMaxGameFrames = 60 * 60 * 30;

// --- Wire format -----------------------------------------------------------------
constexpr uint8_t kWireVersion = 3;
enum : uint8_t {
    MSG_SUB = 1,    // child: send me this game from this frame on
    MSG_GAME = 2,   // a game and the configuration it is played with
    MSG_START = 3,  // its round seed and opening characters
    MSG_FRAMES = 4, // run-length encoded input pairs
    MSG_CHECK = 5,  // the players' checksum after a frame
    MSG_FINISH = 6, // restarted (with what) or over
};
enum : uint8_t { FINISH_NONE = 0, FINISH_RESTART = 1, FINISH_END = 2 };

#pragma pack(push, 1)
struct WireConfig {
    int32_t chars[2][3];
    int32_t colors[2][3];
    int32_t assists[2][3];
    uint8_t simpleMode[2];
    uint8_t autoJump[2];
    uint8_t voice[2][3];
    int32_t stageId;
    int32_t timer;
    uint8_t cpuMode;
    uint8_t fiveBars;
};
struct MsgSub {
    uint8_t type, version;
    uint32_t game;
    int32_t nextFrame;
};
struct MsgGame {
    uint8_t type;
    uint32_t game;
    WireConfig config;
};
struct MsgStart {
    uint8_t type;
    uint32_t game;
    uint32_t seed;
    uint8_t opening[2];
};
struct MsgFrames {
    uint8_t type;
    uint32_t game;
    int32_t first;
    uint16_t count;
    // then runs
};
struct MsgRun {
    uint8_t run;
    uint32_t p1, p2;
};
struct MsgCheck {
    uint8_t type;
    uint32_t game;
    int32_t frame;
    uint32_t checksum;
    uint32_t parts[4];
    uint32_t rng[6];
};
struct MsgFinish {
    uint8_t type;
    uint32_t game;
    uint8_t kind;
    uint32_t seed;
    WireConfig next;
};
#pragma pack(pop)

// --- The log ---------------------------------------------------------------------
struct Check {
    int32_t frame;
    uint32_t checksum;
    uint32_t parts[4]; // positions, life, actions, RNG
    uint32_t rng[6];   // RNG hash per team member (team * 3 + member)
};
struct Game {
    uint32_t id = 0;
    WireConfig config = {};
    bool started = false;
    uint32_t seed = 0;
    uint8_t opening[2] = {0, 0};
    std::vector<uint32_t> inputs; // two per frame: player 1, player 2
    std::vector<Check> checks;
    uint8_t finish = FINISH_NONE;
    uint32_t finishSeed = 0;
    WireConfig next = {};

    int32_t Frames() const { return static_cast<int32_t>(inputs.size() / 2); }
};
// The game in progress and the one before it (a spectator may still be
// playing that one out).
std::deque<Game> g_games;

Game* FindGame(uint32_t id)
{
    if (!id) return nullptr;
    for (Game& game : g_games)
        if (game.id == id) return &game;
    return nullptr;
}

Game* OldestAfter(uint32_t id)
{
    for (Game& game : g_games)
        if (game.id > id) return &game;
    return nullptr;
}

Game& PushGame(uint32_t id, const WireConfig& config)
{
    while (g_games.size() >= 2) g_games.pop_front();
    g_games.emplace_back();
    Game& game = g_games.back();
    game.id = id;
    game.config = config;
    return game;
}

WireConfig ConfigFromBoot()
{
    const TrainingBootConfig& boot = TrainingBoot_GetConfig();
    WireConfig config = {};
    for (int i = 0; i < 3; ++i) {
        config.chars[0][i] = boot.p1Chars[i];
        config.chars[1][i] = boot.p2Chars[i];
        config.colors[0][i] = boot.p1Colors[i];
        config.colors[1][i] = boot.p2Colors[i];
        config.assists[0][i] = boot.p1Assists[i];
        config.assists[1][i] = boot.p2Assists[i];
        config.voice[0][i] = boot.voiceLanguage[0][i];
        config.voice[1][i] = boot.voiceLanguage[1][i];
    }
    for (int team = 0; team < 2; ++team) {
        config.simpleMode[team] = boot.simpleMode[team] ? 1 : 0;
        config.autoJump[team] = boot.autoJump[team] ? 1 : 0;
    }
    config.stageId = boot.stageId;
    config.timer = boot.timerValue;
    config.cpuMode = static_cast<uint8_t>(TrainingBoot_GetCpuMode());
    config.fiveBars = TrainingBoot_GetFiveBarStart() ? 1 : 0;
    return config;
}

bool ConfigToBoot(const WireConfig& config, TrainingBootConfig& boot)
{
    boot = TrainingBoot_GetDefaultConfig();
    for (int team = 0; team < 2; ++team) {
        for (int i = 0; i < 3; ++i) {
            // Untrusted: the same bounds the launch packet is held to.
            if (config.chars[team][i] < 1 || config.chars[team][i] > 50 ||
                config.colors[team][i] < 0 || config.colors[team][i] > 8 ||
                config.assists[team][i] < 0 || config.assists[team][i] > 2)
                return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        boot.p1Chars[i] = config.chars[0][i];
        boot.p2Chars[i] = config.chars[1][i];
        boot.p1Colors[i] = config.colors[0][i];
        boot.p2Colors[i] = config.colors[1][i];
        boot.p1Assists[i] = config.assists[0][i];
        boot.p2Assists[i] = config.assists[1][i];
        boot.voiceLanguage[0][i] = config.voice[0][i] ? 1 : 0;
        boot.voiceLanguage[1][i] = config.voice[1][i] ? 1 : 0;
    }
    for (int team = 0; team < 2; ++team) {
        boot.simpleMode[team] = config.simpleMode[team] != 0;
        boot.autoJump[team] = config.autoJump[team] != 0;
    }
    boot.stageId = config.stageId;
    boot.timerValue = config.timer > 0 && config.timer <= 99 ? config.timer : 99;
    return true;
}

// --- Recording state ---------------------------------------------------------------
bool g_recording = false;

// --- Viewing state -----------------------------------------------------------------
std::atomic<SpectateState> g_state{SpectateState::Off};
std::atomic<bool> g_viewing{false};
int g_slot = -1;
uint32_t g_playGame = 0; // game being simulated
uint32_t g_lastDone = 0; // newest game finished with or given up on
bool g_buffering = true;
bool g_restartPending = false;
WireConfig g_restartConfig = {};
uint32_t g_restartSeed = 0;
ULONGLONG g_noFeedSince = 0;

struct OwnCheck {
    uint32_t game;
    int32_t frame;
    uint32_t checksum;
    uint32_t parts[4];
    uint32_t rng[6];
    bool fastForwarded;
};
OwnCheck g_ownChecks[32] = {};
uint32_t g_ownCheckCount = 0;

std::atomic<int32_t> g_statBuffered{0};
std::atomic<bool> g_statCatchingUp{false};
std::atomic<bool> g_statWaiting{false};
std::atomic<bool> g_statDesynced{false};
std::atomic<bool> g_statFeedLost{false};

// Every comparison is logged for a while: which part of the state differs,
// and whether the frame was drawn or fast-forwarded, says where to look.
int g_comparisonsLogged = 0;
constexpr int kComparisonLogLimit = 200;

void Compare(const OwnCheck& own, const Check& theirs)
{
    const bool same = own.checksum == theirs.checksum;
    if (!same) g_statDesynced = true;
    if (g_comparisonsLogged >= kComparisonLogLimit) return;
    ++g_comparisonsLogged;
    static const char* const names[4] = {"position", "life", "action", "rng"};
    char detail[160] = "";
    size_t used = 0;
    for (int i = 0; i < 4 && !same; ++i) {
        if (own.parts[i] == theirs.parts[i]) continue;
        const int written = snprintf(detail + used, sizeof(detail) - used, " %s %08X/%08X",
                                     names[i], own.parts[i], theirs.parts[i]);
        if (written > 0) used += static_cast<size_t>(written);
        if (used >= sizeof(detail)) break;
    }
    // Which characters' RNG: P1 point/assists, then P2 ("=" same, "x" differs).
    if (!same && own.parts[3] != theirs.parts[3] && used + 16 < sizeof(detail)) {
        char who[8] = "";
        for (int i = 0; i < 6; ++i) who[i] = own.rng[i] == theirs.rng[i] ? '=' : 'x';
        snprintf(detail + used, sizeof(detail) - used, " who=%.3s/%.3s", who, who + 3);
    }
    HookLog("Spectate: check game %u frame %d %s (%s)%s\n", own.game, own.frame,
            same ? "matches" : "DIFFERS local/players:", own.fastForwarded ? "fast-forwarded" : "drawn",
            detail);
}

void CompareCheck(uint32_t game, const Check& theirs)
{
    for (const OwnCheck& own : g_ownChecks)
        if (own.game == game && own.frame == theirs.frame) Compare(own, theirs);
}

// --- Links -------------------------------------------------------------------------
struct Cursor {
    uint32_t game = 0;
    bool sentGame = false;
    bool sentStart = false;
    int32_t nextFrame = 0;
    size_t nextCheck = 0;
    bool sentFinish = false;
};
struct Link {
    uint64_t id = 0;
    bool child = false;      // the room says we feed them
    bool subscribed = false; // they told us where they are
    Cursor cursor;
    int wait = 0;            // frames since the last live batch went out
    ULONGLONG openedAt = 0;
    ULONGLONG lastWanted = 0;
};
Link g_links[kMaxLinks];
uint64_t g_upstream = 0;
bool g_subSent = false;
// A subscription can reach the other end before it has taken the link up, in
// which case it is lost: it is repeated until the feed answers.
ULONGLONG g_subSentTick = 0;
bool g_feedAnswered = false;
constexpr ULONGLONG kSubscriptionRetryMs = 2000;
struct Avoid {
    uint64_t id;
    ULONGLONG until;
};
Avoid g_avoid[kMaxLinks] = {};

Link* FindLink(uint64_t id)
{
    for (Link& link : g_links)
        if (link.id == id) return &link;
    return nullptr;
}

Link* WantLink(uint64_t id, ULONGLONG now)
{
    Link* link = FindLink(id);
    if (!link) {
        for (Link& candidate : g_links) {
            if (candidate.id) continue;
            candidate = Link{};
            candidate.id = id;
            candidate.openedAt = now;
            link = &candidate;
            break;
        }
        if (!link) return nullptr;
        SteamRoom_SpecOpen(id);
        HookLog("Spectate: opening a link to %llu\n", static_cast<unsigned long long>(id));
    }
    link->lastWanted = now;
    return link;
}

void DropLink(Link& link)
{
    HookLog("Spectate: closing the link to %llu\n", static_cast<unsigned long long>(link.id));
    SteamRoom_SpecClose(link.id);
    if (link.id == g_upstream) {
        g_upstream = 0;
        g_subSent = false;
    }
    link = Link{};
}

bool Avoided(uint64_t id, ULONGLONG now)
{
    for (const Avoid& entry : g_avoid)
        if (entry.id == id && now < entry.until) return true;
    return false;
}

void AvoidFor(uint64_t id, ULONGLONG now)
{
    Avoid* slot = &g_avoid[0];
    for (Avoid& entry : g_avoid) {
        if (entry.id == id || now >= entry.until) {
            slot = &entry;
            break;
        }
    }
    slot->id = id;
    slot->until = now + kAvoidFailedMs;
}

template <typename T>
bool Send(uint64_t to, const T& message)
{
    return SteamRoom_SpecSend(to, &message, static_cast<int>(sizeof(message)));
}

// Sends what this child is missing, oldest first, within `budget` bytes.
void PumpChild(Link& link, int budget)
{
    if (!link.subscribed || g_games.empty()) return;
    Cursor& cursor = link.cursor;
    Game* game = FindGame(cursor.game);
    if (!game) {
        // New here, or on a game we no longer keep: the current one, from the top.
        game = &g_games.back();
        cursor = Cursor{};
        cursor.game = game->id;
    }
    ++link.wait;
    for (int guard = 0; guard < 4; ++guard) {
        if (!cursor.sentGame) {
            MsgGame message = {MSG_GAME, game->id, game->config};
            if (!Send(link.id, message)) return;
            cursor.sentGame = true;
            budget -= static_cast<int>(sizeof(message));
        }
        if (game->started && !cursor.sentStart) {
            MsgStart message = {MSG_START, game->id, game->seed, {game->opening[0], game->opening[1]}};
            if (!Send(link.id, message)) return;
            cursor.sentStart = true;
        }
        const int32_t frames = game->Frames();
        int32_t pending = frames - cursor.nextFrame;
        // A live feed goes out in small batches; a backlog as fast as allowed.
        const bool flush = pending >= kFlushFrames || game->finish != FINISH_NONE ||
                           link.wait >= kFlushFrames;
        while (pending > 0 && flush && budget > static_cast<int>(sizeof(MsgFrames) + sizeof(MsgRun))) {
            uint8_t buffer[1400];
            int capacity = budget < static_cast<int>(sizeof(buffer)) ? budget : static_cast<int>(sizeof(buffer));
            MsgFrames head = {MSG_FRAMES, game->id, cursor.nextFrame, 0};
            int used = static_cast<int>(sizeof(head));
            int32_t frame = cursor.nextFrame;
            while (frame < frames && used + static_cast<int>(sizeof(MsgRun)) <= capacity) {
                MsgRun run = {0, game->inputs[frame * 2], game->inputs[frame * 2 + 1]};
                while (frame < frames && run.run < 255 && game->inputs[frame * 2] == run.p1 &&
                       game->inputs[frame * 2 + 1] == run.p2) {
                    ++run.run;
                    ++frame;
                }
                memcpy(buffer + used, &run, sizeof(run));
                used += static_cast<int>(sizeof(run));
            }
            head.count = static_cast<uint16_t>(frame - cursor.nextFrame);
            memcpy(buffer, &head, sizeof(head));
            if (!SteamRoom_SpecSend(link.id, buffer, used)) return;
            cursor.nextFrame = frame;
            pending = frames - frame;
            budget -= used;
            link.wait = 0;
        }
        while (cursor.nextCheck < game->checks.size() &&
               game->checks[cursor.nextCheck].frame < cursor.nextFrame) {
            const Check& check = game->checks[cursor.nextCheck];
            MsgCheck message = {MSG_CHECK, game->id, check.frame, check.checksum, {}, {}};
            memcpy(message.parts, check.parts, sizeof(message.parts));
            memcpy(message.rng, check.rng, sizeof(message.rng));
            if (!Send(link.id, message)) return;
            ++cursor.nextCheck;
        }
        if (game->finish == FINISH_NONE || cursor.nextFrame < frames) return;
        if (!cursor.sentFinish) {
            MsgFinish message = {MSG_FINISH, game->id, game->finish, game->finishSeed, game->next};
            if (!Send(link.id, message)) return;
            cursor.sentFinish = true;
        }
        Game* following = OldestAfter(game->id);
        if (!following) return;
        game = following;
        cursor = Cursor{};
        cursor.game = game->id;
    }
}

void SendSubscription()
{
    MsgSub message = {MSG_SUB, kWireVersion, 0, 0};
    if (!g_games.empty()) {
        message.game = g_games.back().id;
        message.nextFrame = g_games.back().Frames();
    }
    if (Send(g_upstream, message)) {
        g_subSent = true;
        g_subSentTick = GetTickCount64();
        g_feedAnswered = false;
        HookLog("Spectate: asked %llu for the feed (have game %u, %d frames)\n",
                static_cast<unsigned long long>(g_upstream), message.game, message.nextFrame);
    }
}

void OnSubscription(uint64_t from, const MsgSub& message)
{
    if (message.version != kWireVersion) return;
    Link* link = FindLink(from);
    if (!link) return;
    link->subscribed = true;
    link->wait = kFlushFrames;
    link->cursor = Cursor{};
    if (const Game* game = FindGame(message.game)) {
        // They keep what they have of this game; everything else is resent.
        link->cursor.game = game->id;
        link->cursor.nextFrame = message.nextFrame < 0 ? 0
            : message.nextFrame > game->Frames() ? game->Frames() : message.nextFrame;
        while (link->cursor.nextCheck < game->checks.size() &&
               game->checks[link->cursor.nextCheck].frame < link->cursor.nextFrame)
            ++link->cursor.nextCheck;
    }
    HookLog("Spectate: %llu subscribed (game %u from frame %d)\n",
            static_cast<unsigned long long>(from), message.game, message.nextFrame);
}

void OnFeed(const uint8_t* data, int size)
{
    const uint8_t type = data[0];
    if (type == MSG_GAME && size == sizeof(MsgGame)) {
        MsgGame message;
        memcpy(&message, data, sizeof(message));
        if (!message.game || FindGame(message.game)) return;
        TrainingBootConfig probe;
        if (!ConfigToBoot(message.config, probe)) {
            HookLog("Spectate: ignored game %u with an invalid configuration\n", message.game);
            return;
        }
        PushGame(message.game, message.config);
        HookLog("Spectate: receiving game %u\n", message.game);
        if (g_state.load() == SpectateState::Joining) g_state.store(SpectateState::Ready);
        return;
    }
    if (type == MSG_START && size == sizeof(MsgStart)) {
        MsgStart message;
        memcpy(&message, data, sizeof(message));
        if (Game* game = FindGame(message.game)) {
            game->started = true;
            game->seed = message.seed;
            game->opening[0] = message.opening[0] <= 2 ? message.opening[0] : 0;
            game->opening[1] = message.opening[1] <= 2 ? message.opening[1] : 0;
        }
        return;
    }
    if (type == MSG_FRAMES && size >= static_cast<int>(sizeof(MsgFrames))) {
        MsgFrames head;
        memcpy(&head, data, sizeof(head));
        Game* game = FindGame(head.game);
        if (!game || game->finish != FINISH_NONE) return;
        const int32_t have = game->Frames();
        if (head.first > have || head.first < 0) {
            // A hole: ask again from where we are.
            g_subSent = false;
            return;
        }
        int32_t frame = head.first;
        for (int offset = sizeof(MsgFrames); offset + static_cast<int>(sizeof(MsgRun)) <= size;
             offset += sizeof(MsgRun)) {
            MsgRun run;
            memcpy(&run, data + offset, sizeof(run));
            for (int i = 0; i < run.run; ++i, ++frame) {
                if (frame < have || frame >= kMaxGameFrames) continue; // had it, or absurd
                game->inputs.push_back(run.p1);
                game->inputs.push_back(run.p2);
            }
        }
        return;
    }
    if (type == MSG_CHECK && size == sizeof(MsgCheck)) {
        MsgCheck message;
        memcpy(&message, data, sizeof(message));
        if (Game* game = FindGame(message.game)) {
            Check check = {message.frame, message.checksum, {}, {}};
            memcpy(check.parts, message.parts, sizeof(check.parts));
            memcpy(check.rng, message.rng, sizeof(check.rng));
            game->checks.push_back(check);
            CompareCheck(message.game, check);
        }
        return;
    }
    if (type == MSG_FINISH && size == sizeof(MsgFinish)) {
        MsgFinish message;
        memcpy(&message, data, sizeof(message));
        Game* game = FindGame(message.game);
        if (!game || game->finish != FINISH_NONE) return;
        if (message.kind != FINISH_RESTART && message.kind != FINISH_END) return;
        TrainingBootConfig probe;
        if (message.kind == FINISH_RESTART && !ConfigToBoot(message.next, probe)) return;
        game->finish = message.kind;
        game->finishSeed = message.seed;
        game->next = message.next;
        HookLog("Spectate: game %u %s after %d frames\n", message.game,
                message.kind == FINISH_END ? "ended the match" : "was restarted", game->Frames());
    }
}

// (seq, id) order of two spectators of the same slot: who started first.
bool Before(const SpectateRoomMember& a, const SpectateRoomMember& b)
{
    return a.watchSeq < b.watchSeq || (a.watchSeq == b.watchSeq && a.id < b.id);
}

int CapacityOf(const SpectateRoomMember& member)
{
    return member.watchSlot >= 0 ? kViewerChildren : kPlayerChildren;
}

// How many spectators ahead of `self` claim `parent`.
int ClaimsAhead(const SpectateRoom& room, uint64_t parent, const SpectateRoomMember& self)
{
    int ahead = 0;
    for (int i = 0; i < room.count; ++i) {
        const SpectateRoomMember& other = room.members[i];
        if (other.id != self.id && other.watchSlot >= 0 && other.upstream == parent &&
            Before(other, self))
            ++ahead;
    }
    return ahead;
}

uint64_t ChooseUpstream(const SpectateRoom& room, const SpectateRoomMember& self, ULONGLONG now)
{
    const auto member = [&](uint64_t id) -> const SpectateRoomMember* {
        for (int i = 0; i < room.count; ++i)
            if (room.members[i].id == id) return &room.members[i];
        return nullptr;
    };
    // A source is one of the two players, or a spectator of the same slot
    // who started before us: that order keeps the links free of loops.
    const auto isSource = [&](const SpectateRoomMember& candidate, bool current) {
        if (candidate.id == self.id) return false;
        if (candidate.watchSlot < 0)
            // A player who has just left the fight still holds the end of the log.
            return candidate.slot == g_slot && (candidate.fighting || current);
        return candidate.watchSlot == g_slot && Before(candidate, self) &&
               (candidate.upstream != 0 || current);
    };
    if (g_upstream) {
        const SpectateRoomMember* current = member(g_upstream);
        if (current && isSource(*current, true) && !Avoided(g_upstream, now) &&
            ClaimsAhead(room, g_upstream, self) < CapacityOf(*current))
            return g_upstream;
    }
    // Players first, then spectators in the order they arrived.
    const SpectateRoomMember* best = nullptr;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        for (int i = 0; i < room.count; ++i) {
            const SpectateRoomMember& candidate = room.members[i];
            if ((candidate.watchSlot >= 0) != (pass == 1)) continue;
            if (!isSource(candidate, false) || Avoided(candidate.id, now)) continue;
            if (ClaimsAhead(room, candidate.id, self) >= CapacityOf(candidate)) continue;
            if (!best || (pass == 1 ? Before(candidate, *best) : candidate.id < best->id))
                best = &candidate;
        }
    }
    return best ? best->id : 0;
}

void ResetViewer()
{
    g_playGame = 0;
    g_lastDone = 0;
    g_buffering = true;
    g_restartPending = false;
    g_noFeedSince = 0;
    g_ownCheckCount = 0;
    g_comparisonsLogged = 0;
    memset(g_ownChecks, 0, sizeof(g_ownChecks));
    g_statBuffered = 0;
    g_statCatchingUp = false;
    g_statWaiting = false;
    g_statDesynced = false;
    g_statFeedLost = false;
}

} // namespace

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------
void Spectate_RecBegin()
{
    if (g_state.load() != SpectateState::Off) Spectate_Stop();
    g_games.clear();
    g_recording = true;
    HookLog("Spectate: recording this match for spectators\n");
}

void Spectate_RecEnd()
{
    if (!g_recording) return;
    g_recording = false;
    if (!g_games.empty() && g_games.back().finish == FINISH_NONE) {
        g_games.back().finish = FINISH_END;
        HookLog("Spectate: match over after game %u (%d frames)\n", g_games.back().id,
                g_games.back().Frames());
    }
}

bool Spectate_IsRecording()
{
    return g_recording;
}

void Spectate_RecGameStart(uint32_t gameId, uint32_t roundSeed, uint32_t openingP1,
                           uint32_t openingP2)
{
    if (!g_recording || !gameId) return;
    const WireConfig config = ConfigFromBoot();
    if (!g_games.empty() && g_games.back().finish == FINISH_NONE) {
        // No restart was seen for the game before this one.
        g_games.back().finish = FINISH_RESTART;
        g_games.back().next = config;
    }
    if (FindGame(gameId)) return;
    Game& game = PushGame(gameId, config);
    game.started = true;
    game.seed = roundSeed;
    game.opening[0] = static_cast<uint8_t>(openingP1 <= 2 ? openingP1 : 0);
    game.opening[1] = static_cast<uint8_t>(openingP2 <= 2 ? openingP2 : 0);
    game.inputs.reserve(2 * 60 * 120);
    HookLog("Spectate: game %u started (seed 0x%08X, opening %u/%u)\n", gameId, roundSeed,
            game.opening[0], game.opening[1]);
}

void Spectate_RecFrame(uint32_t p1, uint32_t p2)
{
    if (!g_recording || g_games.empty() || g_games.back().finish != FINISH_NONE) return;
    g_games.back().inputs.push_back(p1);
    g_games.back().inputs.push_back(p2);
}

int32_t Spectate_RecFrameCount()
{
    return g_recording && !g_games.empty() && g_games.back().finish == FINISH_NONE
        ? g_games.back().Frames() : -1;
}

void Spectate_RecChecksum(int32_t frame, uint32_t checksum, const uint32_t parts[4],
                          const uint32_t rng[6])
{
    if (!g_recording || g_games.empty() || g_games.back().finish != FINISH_NONE) return;
    Check check = {frame, checksum, {}, {}};
    memcpy(check.parts, parts, sizeof(check.parts));
    memcpy(check.rng, rng, sizeof(check.rng));
    g_games.back().checks.push_back(check);
}

void Spectate_RecRestart(uint32_t restartSeed)
{
    if (!g_recording || g_games.empty() || g_games.back().finish != FINISH_NONE) return;
    Game& game = g_games.back();
    game.finish = FINISH_RESTART;
    game.finishSeed = restartSeed;
    game.next = ConfigFromBoot();
    HookLog("Spectate: game %u restarted after %d frames\n", game.id, game.Frames());
}

// ---------------------------------------------------------------------------
// Watching
// ---------------------------------------------------------------------------
void Spectate_Start(int slot)
{
    if (g_recording) return;
    g_games.clear();
    ResetViewer();
    g_slot = slot;
    g_upstream = 0;
    g_subSent = false;
    g_viewing.store(false);
    g_state.store(SpectateState::Joining);
    HookLog("Spectate: watching setup %d\n", slot + 1);
}

void Spectate_Stop()
{
    if (g_state.load() == SpectateState::Off) return;
    g_state.store(SpectateState::Off);
    g_viewing.store(false);
    g_slot = -1;
    g_games.clear();
    ResetViewer();
    HookLog("Spectate: stopped watching\n");
}

SpectateState Spectate_GetState()
{
    return g_state.load();
}

int Spectate_Slot()
{
    return g_state.load() == SpectateState::Off ? -1 : g_slot;
}

bool Spectate_TakeLaunch(TrainingBootConfig* config, int* cpuMode, bool* fiveBars)
{
    if (g_state.load() != SpectateState::Ready || g_games.empty() || !config) return false;
    const Game& game = g_games.back();
    if (game.finish == FINISH_END) return false; // arrived as it ended
    // A game already restarted is not worth replaying: boot what follows it.
    const bool restarted = game.finish == FINISH_RESTART;
    const WireConfig& wire = restarted ? game.next : game.config;
    if (!ConfigToBoot(wire, *config)) return false;
    if (cpuMode) *cpuMode = wire.cpuMode;
    if (fiveBars) *fiveBars = wire.fiveBars != 0;
    g_playGame = restarted ? 0 : game.id;
    g_lastDone = restarted ? game.id : game.id - 1;
    g_buffering = true;
    g_state.store(SpectateState::Watching);
    g_viewing.store(true);
    HookLog("Spectate: booting game %u\n", game.id);
    return true;
}

bool Spectate_IsViewing()
{
    return g_viewing.load(std::memory_order_relaxed);
}

bool Spectate_GetGameStart(uint32_t* roundSeed, uint32_t* openingP1, uint32_t* openingP2)
{
    if (!Spectate_IsViewing() || g_restartPending) return false;
    // Between games the next one is simply the oldest we have not played.
    if (g_playGame && !FindGame(g_playGame)) {
        if (g_playGame > g_lastDone) g_lastDone = g_playGame;
        g_playGame = 0;
    }
    if (!g_playGame) {
        const Game* next = OldestAfter(g_lastDone);
        if (!next) return false;
        g_playGame = next->id;
        g_buffering = true;
        HookLog("Spectate: next up is game %u\n", g_playGame);
    }
    const Game* game = FindGame(g_playGame);
    if (!game || !game->started) return false;
    if (roundSeed) *roundSeed = game->seed;
    if (openingP1) *openingP1 = game->opening[0];
    if (openingP2) *openingP2 = game->opening[1];
    return true;
}

uint32_t Spectate_PlayGameId()
{
    return g_playGame;
}

uint32_t Spectate_GetInput(uint32_t team, int32_t frame)
{
    const Game* game = FindGame(g_playGame);
    if (!game || frame < 0 || frame >= game->Frames()) return 0;
    return game->inputs[static_cast<size_t>(frame) * 2 + (team & 1)];
}

SpectateStep Spectate_Next(int32_t frame, int32_t actionStep, int32_t* fastForward)
{
    if (fastForward) *fastForward = 0;
    g_statCatchingUp = false;
    if (!Spectate_IsViewing()) return SpectateStep::Hold;
    if (g_restartPending) return SpectateStep::Restart;

    const auto restartInto = [](const WireConfig& config, uint32_t seed) {
        g_restartPending = true;
        g_restartConfig = config;
        g_restartSeed = seed;
        return SpectateStep::Restart;
    };
    Game* game = FindGame(g_playGame);
    if (!game) {
        // The feed moved on without the game we were in.
        if (const Game* newer = OldestAfter(g_playGame > g_lastDone ? g_playGame : g_lastDone))
            return restartInto(newer->config, 0);
        g_statWaiting = true;
        return SpectateStep::Hold;
    }
    const int32_t available = game->Frames() - frame;
    g_statBuffered = available > 0 ? available : 0;
    if (available > 0) {
        if (g_buffering && available < kResumeBuffer && game->finish == FINISH_NONE) {
            g_statWaiting = true;
            return SpectateStep::Hold;
        }
        g_buffering = false;
        g_statWaiting = false;
        static bool catching = false;
        const bool wasCatching = catching;
        catching = false;
        if (kFastForward && fastForward && available > kFastForwardStart) {
            int32_t count = available - kFastForwardTarget;
            if (count > kFastForwardPerTick) count = kFastForwardPerTick;
            *fastForward = count;
            g_statCatchingUp = true;
            catching = true;
        }
        if (catching != wasCatching)
            HookLog("Spectate: %s at frame %d (%d frames buffered)\n",
                    catching ? "catching up" : "caught up", frame, available);
        return SpectateStep::Play;
    }
    if (game->finish == FINISH_RESTART) return restartInto(game->next, game->finishSeed);
    if (game->finish == FINISH_END) return SpectateStep::Leave;
    if (const Game* newer = OldestAfter(game->id)) return restartInto(newer->config, 0);
    g_buffering = true;
    g_statWaiting = true;
    return SpectateStep::Hold;
}

bool Spectate_GetRestart(TrainingBootConfig* config, uint32_t* restartSeed)
{
    if (!g_restartPending || !config) return false;
    if (!ConfigToBoot(g_restartConfig, *config)) return false;
    if (restartSeed) *restartSeed = g_restartSeed;
    return true;
}

void Spectate_OnRestartApplied()
{
    if (!g_restartPending) return;
    g_restartPending = false;
    if (g_playGame > g_lastDone) g_lastDone = g_playGame;
    g_playGame = 0;
    g_buffering = true;
    g_statDesynced = false;
    g_statBuffered = 0;
    g_comparisonsLogged = 0;
    HookLog("Spectate: restart applied; waiting for the game after %u\n", g_lastDone);
}

bool Spectate_WantsChecksum(int32_t frame)
{
    return Spectate_IsViewing() && frame > 0 && frame % kChecksumInterval == 0;
}

void Spectate_OnChecksum(int32_t frame, uint32_t checksum, const uint32_t parts[4],
                         const uint32_t rng[6], bool fastForwarded)
{
    if (!g_playGame) return;
    OwnCheck own = {g_playGame, frame, checksum, {}, {}, fastForwarded};
    memcpy(own.parts, parts, sizeof(own.parts));
    memcpy(own.rng, rng, sizeof(own.rng));
    g_ownChecks[g_ownCheckCount++ % 32] = own;
    if (const Game* game = FindGame(g_playGame)) {
        for (const Check& check : game->checks)
            if (check.frame == frame) Compare(own, check);
    }
}

SpectateStatus Spectate_GetStatus()
{
    SpectateStatus status = {};
    status.buffered = g_statBuffered.load();
    status.catchingUp = g_statCatchingUp.load();
    status.waiting = g_statWaiting.load();
    status.desynced = g_statDesynced.load();
    status.feedLost = g_statFeedLost.load();
    return status;
}

// ---------------------------------------------------------------------------
// Feed
// ---------------------------------------------------------------------------
uint64_t Spectate_Pump(const SpectateRoom& room)
{
    const ULONGLONG now = GetTickCount64();
    SteamRoom_SpecPump();

    static uint8_t buffer[4096];
    uint64_t from = 0;
    int size = 0;
    while (SteamRoom_SpecReceive(&from, buffer, sizeof(buffer), &size)) {
        if (size < 1) continue;
        if (buffer[0] == MSG_SUB) {
            if (size == sizeof(MsgSub)) {
                MsgSub message;
                memcpy(&message, buffer, sizeof(message));
                OnSubscription(from, message);
            }
        } else if (from == g_upstream && g_state.load() != SpectateState::Off) {
            g_feedAnswered = true;
            OnFeed(buffer, size);
        }
    }

    const SpectateRoomMember* self = nullptr;
    for (int i = 0; i < room.count; ++i)
        if (room.members[i].id == room.localId) self = &room.members[i];
    const bool watching = g_state.load() != SpectateState::Off;

    // Who we are fed by.
    uint64_t wanted = 0;
    if (watching && self && self->watchSlot == g_slot) {
        wanted = ChooseUpstream(room, *self, now);
        if (wanted != g_upstream) {
            if (Link* old = FindLink(g_upstream)) {
                if (!old->child) DropLink(*old);
            }
            g_upstream = wanted;
            g_subSent = false;
            if (wanted)
                HookLog("Spectate: feed source is now %llu\n", static_cast<unsigned long long>(wanted));
        }
    } else if (g_upstream) {
        if (Link* old = FindLink(g_upstream)) {
            if (!old->child) DropLink(*old);
        }
        g_upstream = 0;
        g_subSent = false;
    }
    if (g_upstream) {
        if (Link* link = WantLink(g_upstream, now)) {
            const int state = SteamRoom_SpecState(g_upstream);
            if (state == 2) {
                if (!g_subSent ||
                    (!g_feedAnswered && now - g_subSentTick > kSubscriptionRetryMs))
                    SendSubscription();
            } else {
                g_subSent = false;
                if (state == 3 || state == 0 || now - link->openedAt > kConnectTimeoutMs) {
                    HookLog("Spectate: no link to %llu (state %d); trying someone else\n",
                            static_cast<unsigned long long>(g_upstream), state);
                    AvoidFor(g_upstream, now);
                    link->child = false;
                    DropLink(*link);
                }
            }
        }
    }
    if (watching) {
        if (g_upstream) g_noFeedSince = 0;
        else if (!g_noFeedSince) g_noFeedSince = now;
        g_statFeedLost = g_noFeedSince && now - g_noFeedSince > 2000;
    }

    // Who we feed: whoever the room says is fed by us, up to our share,
    // earliest first. The rest find someone else by the same rule.
    for (Link& link : g_links) link.child = false;
    if (self) {
        const int capacity = self->watchSlot >= 0 ? kViewerChildren : kPlayerChildren;
        for (int i = 0; i < room.count; ++i) {
            const SpectateRoomMember& other = room.members[i];
            if (other.id == room.localId || other.watchSlot < 0 || other.upstream != room.localId)
                continue;
            if (ClaimsAhead(room, room.localId, other) >= capacity) continue;
            if (Link* link = WantLink(other.id, now)) link->child = true;
        }
    }
    const int budget = watching ? kViewerBudget : kPlayerBudget;
    for (Link& link : g_links) {
        if (!link.id) continue;
        if (link.child) {
            const int state = SteamRoom_SpecState(link.id);
            if (state == 2) PumpChild(link, budget);
            else if (state != 1) DropLink(link); // failed: opened afresh next frame
        } else if (link.id != g_upstream && now - link.lastWanted > kLinkGraceMs) {
            DropLink(link);
        }
    }
    return wanted;
}
