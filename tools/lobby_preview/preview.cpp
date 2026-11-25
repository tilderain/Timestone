// Host-side preview of the native lobby screens.
//
// Builds the real screen code (UMVC3Hook/code/lobby_ui.cpp) against a scripted
// RoomService and a Canvas that records draw commands as JSON.
// tools/umvc3_ui/umvc3_ui.py turns those commands into PNGs using textures
// unpacked from the user's own game archives, so layout can be reviewed
// without launching the game. Run tools/lobby_preview/preview.sh.

#include "lobby_canvas.h"
#include "lobby_model.h"
#include "lobby_ui.h"
#include "assist_names.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lobby;

namespace {

class JsonCanvas final : public Canvas {
public:
    void Sprite(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
                Color color) override
    {
        Emit("sprite", tex, x, y, w, h, uv, anchor, color);
    }

    void SpriteTurned(Tex tex, float x, float y, float w, float h, Uv uv, float radians,
                      Color color) override
    {
        Emit("sprite", tex, x, y, w, h, uv, A_MC, color);
        char turn[48];
        snprintf(turn, sizeof(turn), ",\"turn\":%.5f}", radians);
        commands_.back().replace(commands_.back().size() - 1, 1, turn);
    }

    void Frame(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
               Color color) override
    {
        Emit(GetFrameStyle(tex).tileCenter ? "tileframe" : "frame", tex, x, y, w, h, uv, anchor,
             color);
    }

    void Text(const char* utf8, float x, float y, float scale, Anchor anchor, Color color,
              Font font, float stretch, Blend blend) override
    {
        std::string escaped;
        for (const char* c = utf8; *c; ++c) {
            if (*c == '"' || *c == '\\') escaped += '\\';
            escaped += *c;
        }
        char line[1024];
        snprintf(line, sizeof(line),
                 "{\"type\":\"text\",\"str\":\"%s\",\"pos\":[%.2f,%.2f],\"scale\":[%.3f,%.3f],"
                 "\"anchor\":%d,\"color\":[%.3f,%.3f,%.3f,%.3f],\"font\":%d,\"depth\":%d}",
                 escaped.c_str(), x, y, scale, scale * stretch, anchor,
                 // The offline renderer has no blend states: show ink as black.
                 blend == BLEND_MENU_INK ? 0.0f : color.r, blend == BLEND_MENU_INK ? 0.0f : color.g,
                 blend == BLEND_MENU_INK ? 0.0f : color.b, color.a,
                 static_cast<int>(font), Depth());
        commands_.push_back(line);
    }

    void Line(float x0, float y0, float x1, float y1, float width, Color color) override
    {
        char line[256];
        snprintf(line, sizeof(line),
                 "{\"type\":\"line\",\"from\":[%.2f,%.2f],\"to\":[%.2f,%.2f],\"width\":%.2f,"
                 "\"color\":[%.3f,%.3f,%.3f,%.3f],\"depth\":%d}",
                 x0, y0, x1, y1, width, color.r, color.g, color.b, color.a, Depth());
        commands_.push_back(line);
    }

    bool Save(const char* path) const
    {
        FILE* file = fopen(path, "w");
        if (!file) return false;
        fputs("[\n", file);
        for (size_t i = 0; i < commands_.size(); ++i)
            fprintf(file, "%s%s\n", commands_[i].c_str(), i + 1 < commands_.size() ? "," : "");
        fputs("]\n", file);
        fclose(file);
        return true;
    }

    size_t Count() const { return commands_.size(); }

private:
    // Painter's order: later commands are nearer, as in the native backend.
    int Depth() const { return 100000 - static_cast<int>(commands_.size()); }

    void Emit(const char* type, Tex tex, float x, float y, float w, float h, Uv uv,
              Anchor anchor, Color color)
    {
        const TexInfo& info = GetTexInfo(tex);
        std::string path;
        for (const char* c = info.path; *c; ++c) path += *c == '\\' ? '/' : *c;
        if (info.localized)
            path += info.archive == ARC_STAGE || info.archive == ARC_LOOSE ? "_typeC" : "_typeD";
        char line[1024];
        snprintf(line, sizeof(line),
                 "{\"type\":\"%s\",\"tex\":\"%s\",\"pos\":[%.2f,%.2f],\"size\":[%.2f,%.2f],"
                 "\"uv\":[%.5f,%.5f,%.5f,%.5f],\"anchor\":%d,\"color\":[%.3f,%.3f,%.3f,%.3f],"
                 "\"depth\":%d}",
                 type, path.c_str(), x, y, w, h, uv.u0, uv.v0, uv.u1, uv.v1, anchor, color.r,
                 color.g, color.b, color.a, Depth());
        commands_.push_back(line);
    }

    std::vector<std::string> commands_;
};

// ---------------------------------------------------------------------------
// Scripted roster and room data
// ---------------------------------------------------------------------------
const char* const kNames[] = {
    "", "Ryu", "Chun-Li", "Akuma", "Chris", "Wesker", "Viewtiful Joe", "Dante", "Trish",
    "Frank West", "Spencer", "Arthur", "Amaterasu", "Zero", "Tron", "Morrigan", "Hsien-Ko",
    "Felicia", "C. Viper", "Haggar", "Jill", "Strider", "Vergil", "Phoenix Wright", "Firebrand",
    "Nemesis", "Spider-Man", "Captain America", "Wolverine", "Magneto", "Hulk", "She-Hulk",
    "Taskmaster", "Iron Man", "Thor", "Doctor Doom", "Phoenix", "Shuma-Gorath", "M.O.D.O.K.",
    "Dormammu", "Deadpool", "Storm", "Super-Skrull", "Sentinel", "X-23", "Nova",
    "Rocket Raccoon", "Ghost Rider", "Iron Fist", "Doctor Strange", "Hawkeye",
};
int CharacterCount() { return 50; }
int32_t CharacterId(int index) { return index + 1; }
const char* CharacterName(int32_t id) { return id >= 1 && id <= 50 ? kNames[id] : "?"; }
const char* AssistName(int32_t id, int assist)
{
    return id >= 1 && id <= 50 ? kAssistMoveNames[id].names[assist % 3] : "";
}
int ColorCount(int32_t) { return 8; }
int PresetCount() { return 3; }
// The game's reserve units; the third is left without a usable team.
Team g_reserveUnits[3] = {
    {{28, 35, 3}, {1, 2, 0}, {0, 1, 2}},
    {{13, 7, 21}, {0, 0, 1}, {0, 0, 0}},
    {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
};
bool Preset(int index, Team* team)
{
    if (index < 0 || index >= 3 || g_reserveUnits[index].chars[0] == 0) return false;
    *team = g_reserveUnits[index];
    return true;
}
bool SetPreset(int index, const Team& team)
{
    if (index < 0 || index >= 3) return false;
    g_reserveUnits[index] = team;
    return true;
}
const char* const kStageNames[] = {
    "Training Room", "Daily Bugle", "Metro City", "Demon Village", "Kattlelox Island",
    "Hand Hideout", "S.H.I.E.L.D. Helicarrier", "Asgard", "Tricell Labs", "Fate of the World",
    "Danger Room", "City That Never Sleeps", "Days of Future Past", "Demon Village Redux",
    "Bonne Wonderland", "Shadowland", "S.H.I.E.L.D. Air Show", "Asgard: Sea of Space",
    "Chaos at Tricell",
};
const int32_t kStageIds[] = {0, 2, 3, 4, 5, 6, 7, 9, 10, 11, 100, 102, 103, 104, 105, 106, 107, 109, 110};
int StageCount() { return 19; }
int32_t StageId(int index) { return index >= 0 && index < 19 ? kStageIds[index] : kStageAny; }
const char* StageName(int32_t id)
{
    for (int i = 0; i < 19; ++i)
        if (kStageIds[i] == id) return kStageNames[i];
    return "";
}
const RosterSource kRoster = {CharacterCount, CharacterId, CharacterName, AssistName,
                              ColorCount, PresetCount, Preset, SetPreset,
                              StageCount, StageId, StageName};

class FakeService final : public RoomService {
public:
    bool available = true;
    std::string status = "Steam ready";
    RoomBusy busy = RoomBusy::None;
    std::vector<SearchResult> results;
    RoomState room = {};
    std::vector<ChatLine> chat;
    std::string error;

    bool Available() const override { return available; }
    const char* StatusText() const override { return status.c_str(); }
    const char* LocalName() const override { return "tilderain"; }
    RoomBusy Busy() const override { return busy; }

    void Search() override { status = "Found 5 lobbies"; }
    int ResultCount() const override { return static_cast<int>(results.size()); }
    const SearchResult& Result(int index) const override { return results[index]; }

    void Create(const RoomSettings& settings) override
    {
        room = {};
        room.inRoom = true;
        room.id = 1;
        room.settings = settings;
        room.music = settings.music;
        AddMember(1, "tilderain", true, true);
    }
    void Join(uint64_t) override {}
    void Leave() override { room = {}; }
    const RoomState& Room() const override { return room; }

    void Sit(int slot, int side) override
    {
        Member* local = Local();
        if (!local) return;
        local->slot = static_cast<int8_t>(slot);
        local->side = static_cast<int8_t>(side);
        local->accepted = false;
        local->delayPick = kDelayPickNone;
        local->ready = false;
        local->status = MemberStatus::Seated;
        Recompute();
    }
    void Stand() override
    {
        Member* local = Local();
        if (!local) return;
        local->slot = -1;
        local->accepted = false;
        local->delayPick = kDelayPickNone;
        local->ready = false;
        local->status = MemberStatus::Idle;
        Recompute();
    }
    void SetReady(bool ready) override
    {
        Member* local = Local();
        if (!local) return;
        local->ready = ready;
        local->status = ready ? MemberStatus::Ready : MemberStatus::Seated;
        Recompute();
    }
    void SetTeam(const Team& team) override
    {
        if (Member* local = Local()) local->team = team;
    }
    void SetStage(int stage) override
    {
        if (Member* local = Local()) local->stage = static_cast<int16_t>(stage);
    }
    void SetIcon(int icon) override
    {
        if (Member* local = Local()) local->icon = static_cast<int16_t>(icon);
    }
    // No Steam here: a profile picture shows as the default player icon.
    Tex AvatarTex(uint64_t) const override { return TEX_USER_ICON; }
    void AcceptMatch() override
    {
        if (Member* local = Local()) local->accepted = true;
    }
    int AutoDelay(int pingMs) const override { return pingMs < 0 ? -1 : 1 + pingMs / 40; }
    void HoverDelay(int pick) override
    {
        if (Member* local = Local()) local->delayHover = static_cast<int8_t>(pick);
    }
    void PickDelay(int pick) override
    {
        if (Member* local = Local()) local->delayPick = static_cast<int8_t>(pick);
    }
    void JoinQueue(int slot) override
    {
        if (Member* local = Local()) {
            local->queueSlot = static_cast<int8_t>(slot);
            local->queuePos = 3;
        }
    }
    void LeaveQueue() override
    {
        if (Member* local = Local()) local->queueSlot = -1;
    }
    int watchIntent = -1;
    void SetWatchIntent(int slot) override { watchIntent = slot; }
    int WatchIntent() const override { return watchIntent; }
    void Watch(int slot) override
    {
        if (Member* local = Local()) {
            local->watchSlot = static_cast<int8_t>(slot);
            local->status = MemberStatus::Watching;
        }
    }
    void StopWatching() override
    {
        if (Member* local = Local()) local->watchSlot = -1;
    }
    void InviteFriends() override {}
    void CopyLobbyLink() override {}
    void SetMusic(int32_t trackId) override { room.music = trackId; }
    void ApplySettings(const RoomSettings& settings) override
    {
        snprintf(room.settings.name, sizeof(room.settings.name), "%s", settings.name);
        room.settings.visibility = settings.visibility;
        room.settings.firstTo = settings.firstTo;
        room.settings.rotation = settings.rotation;
        char line[kChatCapacity];
        snprintf(line, sizeof(line), "Lobby settings have been changed: First to %d",
                 settings.firstTo);
        AddChat("", line, true);
        chat.back().settings = true;
    }
    void ViewProfile(uint64_t) override {}
    void Kick(uint64_t) override {}
    void SendChat(const char* utf8) override { AddChat("tilderain", utf8, false); }
    int ChatCount() const override { return static_cast<int>(chat.size()); }
    const ChatLine& Chat(int index) const override { return chat[index]; }
    bool TakeError(char* message, size_t capacity) override
    {
        if (error.empty()) return false;
        snprintf(message, capacity, "%s", error.c_str());
        error.clear();
        return true;
    }

    Member& AddMember(uint64_t id, const char* name, bool host, bool local)
    {
        Member& member = room.members[room.memberCount++];
        member = {};
        member.id = id;
        snprintf(member.name, sizeof(member.name), "%s", name);
        member.isHost = host;
        member.isLocal = local;
        member.slot = -1;
        member.queueSlot = -1;
        member.watchSlot = -1;
        member.delayPick = kDelayPickNone;
        member.delayHover = kDelayPickNone;
        member.health[0] = member.health[1] = member.health[2] = kHealthUnknown;
        member.pingMs = static_cast<int16_t>(20 + id * 23 % 190);
        Preset(static_cast<int>(id % 3), &member.team);
        // Every other member has picked a game icon instead of a Steam picture.
        member.icon = static_cast<int16_t>(id % 2 ? kIconSteam : 40 + id * 7 % 190);
        member.stage = static_cast<int16_t>(id % 2 ? 9 : kStageAny);
        return member;
    }
    void Seat(int memberIndex, int slot, int side, bool ready, MemberStatus status)
    {
        Member& member = room.members[memberIndex];
        member.slot = static_cast<int8_t>(slot);
        member.side = static_cast<int8_t>(side);
        member.ready = ready;
        member.status = status;
        // A seated guest in these scenes has already answered the ready
        // prompt; the scenes move it through the later steps by hand.
        member.accepted = true;
        member.delayPick = kDelayPickNone;
        member.delayHover = 4; // highlighting delay 3
    }
    void AddChat(const char* sender, const char* text, bool system)
    {
        ChatLine line = {};
        snprintf(line.sender, sizeof(line.sender), "%s", sender);
        snprintf(line.text, sizeof(line.text), "%s", text);
        line.system = system;
        chat.push_back(line);
    }
    void Recompute()
    {
        for (int slot = 0; slot < room.settings.slotCount; ++slot) {
            int seated = 0, ready = 0;
            bool fighting = false;
            for (int i = 0; i < room.memberCount; ++i) {
                const Member& member = room.members[i];
                if (member.slot != slot || member.side > 1) continue;
                ++seated;
                ready += member.ready;
                fighting |= member.status == MemberStatus::Fighting;
            }
            room.slots[slot] = fighting ? SlotState::InProgress
                             : seated == 2 && ready == 2 ? SlotState::Starting
                             : seated == 2 ? SlotState::Standby
                             : seated == 1 ? SlotState::Waiting : SlotState::Open;
        }
    }

private:
    Member* Local()
    {
        for (int i = 0; i < room.memberCount; ++i)
            if (room.members[i].isLocal) return &room.members[i];
        return nullptr;
    }
};

SearchResult MakeResult(uint64_t id, const char* name, const char* host, int members,
                        int maxMembers, int ping, bool compatible = true, bool live = false)
{
    SearchResult result = {};
    result.id = id;
    snprintf(result.name, sizeof(result.name), "%s", name);
    snprintf(result.host, sizeof(result.host), "%s", host);
    // Every other host has picked a game icon instead of a Steam picture.
    result.hostId = id;
    result.hostIcon = static_cast<int16_t>(id % 2 ? kIconSteam : 30 + id * 11 % 200);
    result.members = members;
    result.maxMembers = maxMembers;
    result.slotCount = 2;
    result.pingMs = ping;
    result.compatible = compatible;
    result.matchInProgress = live;
    result.firstTo = static_cast<int>(id % 4 == 0 ? 0 : 1 + id % 3);
    return result;
}

// Stand-in for the plugin's settings: twelve modes and switches that hold.
const int kFakeModes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 800},
                             {1360, 768}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                             {2560, 1440}, {3840, 2160}};
int g_fakeMode = 8;
bool g_fakeSwitches[static_cast<int>(Setting::Count)] = {};
int g_fakeLevels[static_cast<int>(Level::Count)] = {1, 2};
const SettingsSource kFakeSettings = {
    [](Setting setting) { return g_fakeSwitches[static_cast<int>(setting)]; },
    [](Setting setting, bool on) { g_fakeSwitches[static_cast<int>(setting)] = on; return true; },
    []() { return 12; },
    [](int index, int* width, int* height) {
        if (index < 0) index = g_fakeMode;
        if (index >= 12) return false;
        *width = kFakeModes[index][0];
        *height = kFakeModes[index][1];
        return true;
    },
    [](int index) { g_fakeMode = index; return true; },
    [](Level setting) { return g_fakeLevels[static_cast<int>(setting)]; },
    [](Level setting, int level) { g_fakeLevels[static_cast<int>(setting)] = level; return true; },
};

UiPrefs DefaultPrefs()
{
    UiPrefs prefs = {};
    // Presets 1, 2, 4 and 7 hold teams; the rest are free.
    const struct { int slot; const char* name; Team team; } presets[] = {
        {0, "PRESET 1", {{28, 35, 3}, {1, 2, 0}, {0, 1, 2}}},
        {1, "ZERO MAY CRY", {{13, 7, 22}, {0, 0, 1}, {1, 0, 3}}},
        {3, "MORRIDOOM", {{15, 35, 22}, {2, 1, 1}, {2, 0, 3}}},
        {6, "a long name to be cut", {{46, 10, 50}, {0, 2, 1}, {0, 0, 1}}},
    };
    for (const auto& preset : presets) {
        prefs.presets[preset.slot].used = true;
        prefs.presets[preset.slot].team = preset.team;
        snprintf(prefs.presets[preset.slot].name, sizeof(prefs.presets[preset.slot].name), "%s",
                 preset.name);
    }
    prefs.slot = 0;
    prefs.stage = 105;
    prefs.team = prefs.presets[0].team;
    snprintf(prefs.create.name, sizeof(prefs.create.name), "tilderain's Lobby");
    prefs.create.maxMembers = 8;
    prefs.create.slotCount = 2;
    prefs.create.firstTo = 3;
    prefs.create.rotation = Rotation::WinnerStays;
    return prefs;
}

void Populate(FakeService& service, int slots)
{
    RoomSettings settings = {};
    snprintf(settings.name, sizeof(settings.name), "Friday Night Marvel");
    settings.maxMembers = 8;
    settings.slotCount = slots;
    // One of each for the title bar's access tag.
    settings.visibility = slots == 1 ? Visibility::Private
                        : slots == 2 ? Visibility::FriendsOnly : Visibility::Public;
    settings.firstTo = 3;
    settings.rotation = Rotation::WinnerStays;
    service.Create(settings);
    service.AddMember(2, "Vevota", false, false);
    service.AddMember(3, "ArcherOfLegend", false, false);
    service.AddMember(4, "Jasongamedev", false, false);
    service.AddMember(5, "a_really_long_steam_persona_name", false, false);
    service.AddMember(6, "John2143", false, false);
    service.Seat(1, 0, 1, false, MemberStatus::Seated);
    service.Seat(2, 1, 0, true, MemberStatus::Fighting);
    service.Seat(3, 1, 1, true, MemberStatus::Fighting);
    if (slots > 2) service.Seat(4, 2, 0, false, MemberStatus::Seated);
    // The match in progress, part-way through a game.
    {
        const uint8_t one[3] = {64, 100, 0}, two[3] = {20, 88, 100};
        memcpy(service.room.members[2].health, one, 3);
        memcpy(service.room.members[3].health, two, 3);
    }
    // Two players waiting for the match in progress (the second slot).
    if (slots > 1) {
        service.room.members[5].queueSlot = 1;
        service.room.members[5].queuePos = 1;
        service.room.members[4].queueSlot = 1;
        service.room.members[4].queuePos = 2;
    }
    // ...and watching it meanwhile.
    if (slots > 1) {
        service.room.members[5].watchSlot = 1;
        service.room.members[4].watchSlot = 1;
        service.room.members[5].status = MemberStatus::Watching;
    }
    service.Recompute();
    service.AddChat("", "Vevota joined the lobby.", true);
    service.AddChat("Vevota", "anyone up for a long set?", false);
    service.AddChat("ArcherOfLegend", "Good fight!", false);
    service.AddChat("Jasongamedev", "One more?", false);
}

Input Press(bool Input::*button)
{
    Input input = {};
    input.*button = true;
    return input;
}

void Settle(LobbyUi& ui, int frames = 14)
{
    const Input idle = {};
    for (int i = 0; i < frames; ++i) ui.Update(idle);
}

bool Capture(LobbyUi& ui, const std::string& directory, const char* name)
{
    JsonCanvas canvas;
    ui.Draw(canvas);
    const std::string path = directory + "/" + name + ".json";
    const bool saved = canvas.Save(path.c_str());
    printf("%-14s %3zu commands%s\n", name, canvas.Count(), saved ? "" : "  (write failed)");
    return saved;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string directory = argc > 1 ? argv[1] : ".";
    bool ok = true;

    {   // Top menu.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ok &= Capture(ui, directory, "top");
        ui.Update(Press(&Input::cancel));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "confirm_exit");
    }
    {   // Lobby browser with results.
        FakeService service;
        service.results = {
            MakeResult(11, "Friday Night Marvel", "Vevota", 6, 8, 34, true, true),
            MakeResult(12, "Beginners welcome", "ArcherOfLegend", 2, 4, 72),
            MakeResult(13, "Room to grow", "Jasongamedev", 5, 16, 118),
            MakeResult(14, "EU evening casuals", "a_really_long_steam_persona_name", 3, 8, 164),
            MakeResult(19, "Sixteen seats", "Brian_NY", 11, 16, 55),
            MakeResult(20, "Full house", "rain", 16, 16, 61),
            MakeResult(15, "old build lobby", "John2143", 2, 8, 40, false),
            MakeResult(16, "West coast", "Sheep", 8, 8, 210),
            MakeResult(17, "Tournament practice", "Ryn", 4, 6, -1),
            MakeResult(18, "overflow row", "anotak", 1, 8, 90),
        };
        // Friends-only lobbies, one under the cursor and one not.
        service.results[1].friendsOnly = true;
        service.results[3].friendsOnly = true;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "browse");
    }
    {   // Empty browser.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.Update(Press(&Input::confirm));
        service.status = "Found 0 lobbies";
        Settle(ui);
        ok &= Capture(ui, directory, "browse_empty");
    }
    {   // Create lobby.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "create");
        ui.Update(Press(&Input::up)); // name row: edit it, caret on and off
        ui.Update(Press(&Input::confirm));
        ok &= Capture(ui, directory, "create_name_caret_a");
        Settle(ui, 20);
        ok &= Capture(ui, directory, "create_name_caret_b");
        ui.Update(Press(&Input::cancel));
        ui.Update(Press(&Input::down));
        for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down)); // LOBBY MUSIC
        ui.Update(Press(&Input::right));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "create_music_row");
        ui.Update(Press(&Input::confirm));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "create_music");
    }
    for (int players : {9, 12, 16}) {   // A sixteen-seat room filling past eight rows.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        Populate(service, 4);
        service.room.settings.maxMembers = 16;
        const char* const extra[] = {"Brian_NY", "rain", "catboy_fucker", "TwiGinTec", "MystiThyghs",
                                     "KeepingItOscar", "Sheep", "Ryn", "anotak", "Moka"};
        for (int i = 0; service.room.memberCount < players; ++i)
            service.AddMember(20 + i, extra[i], false, false);
        service.room.members[7].status = MemberStatus::Ready;
        service.room.members[8].queueSlot = 0;
        service.room.members[8].queuePos = 1;
        service.Recompute();
        Settle(ui, 100);
        ui.Update(Press(&Input::left)); // into the player list
        ui.Update(Press(&Input::down));
        Settle(ui, 2);
        char name[32];
        snprintf(name, sizeof(name), "room_%dplayers", players);
        ok &= Capture(ui, directory, name);
    }
    for (int slots : {1, 2, 4}) {   // Room with 1, 2 and 4 match slots.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        Populate(service, slots);
        Settle(ui, 100); // long enough for the room's backdrop to fade in fully
        char name[32];
        snprintf(name, sizeof(name), "room_%dslot", slots);
        ok &= Capture(ui, directory, name); // cursor on the open seat
        if (slots == 4) {
            // The match in progress (the second slot): line up, watch or both.
            ui.Update(Press(&Input::right));
            ui.Update(Press(&Input::confirm));
            Settle(ui, 3);
            ok &= Capture(ui, directory, "room_slot_menu");
            ui.Update(Press(&Input::confirm));
            Settle(ui, 3);
            ok &= Capture(ui, directory, "room_4slot_queued");
            // Watching only: out of the line, the same setup marked in green.
            service.LeaveQueue();
            service.SetWatchIntent(1);
            Settle(ui, 3);
            ok &= Capture(ui, directory, "room_4slot_watching");
        }
        if (slots == 2) {
            ui.Update(Press(&Input::menu));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_menu");
            ui.Update(Press(&Input::cancel));
            ui.Update(Press(&Input::left)); // into the player list
            ui.Update(Press(&Input::down));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_members");
            ui.Update(Press(&Input::confirm));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_member_menu");
            ui.Update(Press(&Input::down));
            ui.Update(Press(&Input::confirm));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_kick");
            ui.Update(Press(&Input::cancel));
            // Up off the top of the player list lands on the chat panel.
            ui.Update(Press(&Input::up));
            ui.Update(Press(&Input::up));
            Settle(ui, 10); // the shortcut tag has popped up
            ok &= Capture(ui, directory, "room_chat_focus");
            {
                Input keyboard = {};
                keyboard.usingKeyboard = true;
                for (int i = 0; i < 2; ++i) ui.Update(keyboard);
                ok &= Capture(ui, directory, "room_chat_focus_keyboard");
                keyboard.actionY = true; // Y on the keyboard types at once
                ui.Update(keyboard);
                keyboard.actionY = false;
                keyboard.typed = "gg";
                ui.Update(keyboard);
                keyboard.typed = nullptr;
                for (int i = 0; i < 2; ++i) ui.Update(keyboard);
                ok &= Capture(ui, directory, "room_chat_typing");
                ui.Update(Press(&Input::cancel));
            }
            for (int i = 0; i < 30; ++i) {
                char line[128];
                snprintf(line, sizeof(line), i % 4 == 1
                             ? "message number %d, which goes on long enough to need a second and even a third row to say it all"
                             : "message number %d in a long evening", i + 1);
                service.AddChat(i % 3 ? "Vevota" : "ArcherOfLegend", line, false);
            }
            ui.Update(Press(&Input::confirm)); // expand
            ui.Update(Press(&Input::up));
            ui.Update(Press(&Input::up));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_chat_expanded");
            ui.Update(Press(&Input::cancel)); // collapse
            ui.Update(Press(&Input::right)); // back to the seats
            ui.Update(Press(&Input::menu));
            ui.Update(Press(&Input::down)); // LOBBY MUSIC
            ui.Update(Press(&Input::confirm));
            ui.Update(Press(&Input::right)); // jump to the stage group
            for (int i = 0; i < 4; ++i) ui.Update(Press(&Input::down));
            ui.Update(Press(&Input::confirm)); // host picks it
            ui.Update(Press(&Input::down));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_music");
            ui.Update(Press(&Input::cancel));
            ui.Update(Press(&Input::menu));
            ui.Update(Press(&Input::down));
            ui.Update(Press(&Input::down)); // LOBBY SETTINGS (host)
            ui.Update(Press(&Input::confirm));
            Settle(ui);
            ui.Update(Press(&Input::down)); // LOBBY SIZE: as created
            ok &= Capture(ui, directory, "room_settings_fixed");
            for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down)); // MATCH FORMAT
            ui.Update(Press(&Input::right)); // the next set length up
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_settings");
            for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down)); // APPLY CHANGES
            ui.Update(Press(&Input::confirm));
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_settings_applied");
            ui.Update(Press(&Input::quickChat)); // RB / X
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_chat");
            ui.Update(Press(&Input::cancel));

            // Sit opposite Vevota: the pre-match prompts, step by step. The
            // delay pick comes first and is the ready-up.
            ui.Update(Press(&Input::confirm));
            Settle(ui, 90);
            ok &= Capture(ui, directory, "match_delay");
            ui.Update(Press(&Input::cancel)); // backing out is asked first
            Settle(ui, 2);
            ok &= Capture(ui, directory, "match_delay_leave");
            ui.Update(Press(&Input::cancel)); // "No": still seated
            ui.Update(Press(&Input::right));
            ui.Update(Press(&Input::right));
            ui.Update(Press(&Input::right));
            ui.Update(Press(&Input::confirm)); // delay 2
            Settle(ui, 3);
            ok &= Capture(ui, directory, "match_delay_wait");
            service.room.members[1].delayPick = kDelayPickAuto;
            Settle(ui, 3);
            ok &= Capture(ui, directory, "match_entry"); // START with the team in use
            ui.Update(Press(&Input::right));
            ui.Update(Press(&Input::right));             // two teams on
            Settle(ui, 3);
            ok &= Capture(ui, directory, "match_entry_next");
            ui.Update(Press(&Input::down));
            Settle(ui, 3);
            ok &= Capture(ui, directory, "match_entry_select");
            ui.Update(Press(&Input::confirm)); // CHARACTER SELECT
            Settle(ui, 20);
            ok &= Capture(ui, directory, "match_team");
            ui.Update(Press(&Input::up));
            ui.Update(Press(&Input::up)); // past PRESET, wrapping to START
            ui.Update(Press(&Input::confirm));
            Settle(ui, 4);
            ok &= Capture(ui, directory, "match_wait_start");
            ui.SetBusy("Connecting to Vevota...");
            Settle(ui, 2);
            ok &= Capture(ui, directory, "room_busy");
        }
    }
    {   // Settings.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs(), kFakeSettings);
        Settle(ui);
        for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "top_settings");
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        ok &= Capture(ui, directory, "settings_resolution_row");
        ui.Update(Press(&Input::confirm)); // the display's modes
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_resolutions");
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::confirm)); // apply the next one
        for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::right)); // relay only
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings");
        ui.Update(Press(&Input::down)); // NETWORK STATS, with its preview
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_net_stats_compact");
        ui.Update(Press(&Input::left));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_net_stats_full");
        ui.Update(Press(&Input::left));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_net_stats_off");
        ui.Update(Press(&Input::down)); // SPECTATOR OVERLAY, with its preview
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_spectator_full");
        ui.Update(Press(&Input::right));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_spectator_compact");
        ui.Update(Press(&Input::right));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "settings_spectator_none");
    }
    {   // Team setup.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_preset_row");
        ui.Update(Press(&Input::confirm)); // every team slot
        for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_presets");
        ui.Update(Press(&Input::actionX)); // rename
        {
            Input typing = {};
            typing.backspace = true;
            for (int i = 0; i < 4; ++i) ui.Update(typing);
            typing.backspace = false;
            typing.typed = " TEAM";
            ui.Update(typing);
        }
        Settle(ui, 25);
        ok &= Capture(ui, directory, "team_presets_rename");
        ui.Update(Press(&Input::confirm));
        ui.Update(Press(&Input::actionY)); // delete
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_presets_delete");
        ui.Update(Press(&Input::cancel));
        for (int i = 0; i < 2; ++i) ui.Update(Press(&Input::down)); // a free slot
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_presets_free");
        for (int i = 0; i < 5; ++i) ui.Update(Press(&Input::down)); // reserve unit A
        ui.Update(Press(&Input::confirm));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_reserve");
        ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team");
        ui.Update(Press(&Input::confirm)); // POINT character dropdown
        ui.Update(Press(&Input::down));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_characters");
        ui.Update(Press(&Input::cancel));
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::down));    // POINT color
        ui.Update(Press(&Input::confirm)); // every color at once
        ui.Update(Press(&Input::right));
        ui.Update(Press(&Input::right));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_colors");
        ui.Update(Press(&Input::cancel));
        ui.Update(Press(&Input::up));
        ui.Update(Press(&Input::up));      // back on the POINT character
        for (int i = 0; i < 6; ++i) ui.Update(Press(&Input::down)); // ANCHOR character
        ui.Update(Press(&Input::confirm));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_characters_anchor");
        ui.Update(Press(&Input::cancel));
        for (int i = 0; i < 3; ++i) ui.Update(Press(&Input::down)); // ICON row
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_icon");
        ui.Update(Press(&Input::down));    // STAGE row
        ui.Update(Press(&Input::confirm)); // stage grid
        ui.Update(Press(&Input::up));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_stages");
        ui.Update(Press(&Input::cancel));
        ui.Update(Press(&Input::up));
        ui.Update(Press(&Input::confirm)); // icon grid
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::down));
        for (int i = 0; i < 5; ++i) ui.Update(Press(&Input::right));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_icon_list");
        ui.Update(Press(&Input::confirm)); // pick a portrait
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_icon_picked");
    }
    {   // A team member left to chance.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        for (int i = 0; i < 4; ++i) ui.Update(Press(&Input::down)); // SECOND character
        ui.Update(Press(&Input::confirm));
        for (int i = 0; i < 5; ++i) ui.Update(Press(&Input::down)); // under the roster: RANDOM
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_random_pick");
        ui.Update(Press(&Input::confirm));
        ui.Update(Press(&Input::down)); // its assist row
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_random");
        for (int i = 0; i < 5; ++i) ui.Update(Press(&Input::up)); // PRESET row
        ui.Update(Press(&Input::confirm));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_random_presets");
    }
    {   // A member drawn again for every game: RANDOM ALL, beside RANDOM.
        FakeService service;
        UiPrefs prefs = DefaultPrefs();
        prefs.presets[0].team.chars[1] = kCharRandom;
        prefs.presets[0].team.chars[2] = kCharRandomAll;
        prefs.team = prefs.presets[0].team;
        LobbyUi ui(service, kRoster, prefs);
        Settle(ui);
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::down));
        ui.Update(Press(&Input::confirm));
        Settle(ui);
        for (int i = 0; i < 7; ++i) ui.Update(Press(&Input::down)); // ANCHOR character
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_random_all");
        ui.Update(Press(&Input::confirm));
        Settle(ui, 2);
        ok &= Capture(ui, directory, "team_random_all_pick");
    }
    {   // The pre-match prompt with a random member on the team in use.
        FakeService service;
        UiPrefs prefs = DefaultPrefs();
        prefs.presets[0].team.chars[1] = kCharRandom;
        prefs.team = prefs.presets[0].team;
        LobbyUi ui(service, kRoster, prefs);
        Settle(ui);
        Populate(service, 4);
        Settle(ui, 100);
        ui.Update(Press(&Input::confirm)); // sit opposite Vevota
        Settle(ui, 90);
        ui.Update(Press(&Input::confirm)); // delay pick
        Settle(ui, 3);
        service.room.members[1].delayPick = kDelayPickAuto;
        Settle(ui, 3);
        ok &= Capture(ui, directory, "match_entry_random");
    }
    {   // Notice modal.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.ShowNotice("Connection to opponent has been lost.\nBattle will now end.");
        Settle(ui, 2);
        ok &= Capture(ui, directory, "notice");
    }
    {   // A notice too long for one line: wrapped, never cut.
        FakeService service;
        LobbyUi ui(service, kRoster, DefaultPrefs());
        Settle(ui);
        ui.ShowNotice("This lobby is running a newer Timestone version.\n"
                      "Lobby: 0.4.2    Yours: 0.3.21\nUpdate Timestone to join.\n"
                      "A much longer line of text that could not possibly fit across the panel "
                      "is broken at a space and carried on below instead of being cut short.");
        Settle(ui, 2);
        ok &= Capture(ui, directory, "notice_long");
    }
    return ok ? 0 : 1;
}
