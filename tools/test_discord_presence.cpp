// Host test for the Discord presence payloads. Builds and runs on Linux:
//   make DISCORD_PRESENCE_TEST=1 discord-presence-test
#include "discord_presence_format.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* file, int line, const char* what)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("%s:%d: FAILED: %s\n", file, line, what);
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void Copy(char* dst, int size, const char* src)
{
    std::snprintf(dst, static_cast<size_t>(size), "%s", src);
}

PresenceSnapshot Room(bool pub, int members, int limit)
{
    PresenceSnapshot s;
    s.activity = PresenceActivity::InRoom;
    s.lobbyId = 109775244080946091ull;
    Copy(s.roomName, kPresenceNameBytes, "Probe");
    s.memberCount = members;
    s.memberLimit = limit;
    s.roomPublic = pub;
    Copy(s.version, 16, "0.6.5");
    return s;
}

std::string Value(const std::string& json, const char* key)
{
    std::string out;
    Check(DiscordPresence_JsonString(json, key, out), __FILE__, __LINE__, "json key present");
    return out;
}

} // namespace

#define CHECK(cond) Check((cond), __FILE__, __LINE__, #cond)

int main()
{
    // 1. A public room with three of eight seats: joinable, id never published.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(true, 3, 8));
        CHECK(Contains(json, "\"secrets\":{\"join\":\"+connect_lobby 109775244080946091\"}"));
        CHECK(Contains(json, "\"party\":{\"id\":\"timestone-"));
        CHECK(Contains(json, "\"size\":[3,8]"));
        CHECK(Value(json, "details") == "Room: Probe");
        CHECK(Value(json, "state") == "Hanging out");

        const std::size_t idAt = json.find("109775244080946091");
        const std::size_t partyAt = json.find("\"party\":{\"id\":\"timestone-");
        CHECK(idAt != std::string::npos);
        CHECK(partyAt != std::string::npos);
        CHECK(idAt > partyAt); // only inside the join secret
        CHECK(partyAt < idAt);

        const std::string partyId = Value(json, "id");
        CHECK(partyId.size() == 26);
        CHECK(partyId.compare(0, 10, "timestone-") == 0);
    }

    // 2. A private room: party but no join secret, and no privacy field.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(false, 3, 8));
        CHECK(!Contains(json, "\"secrets\""));
        CHECK(!Contains(json, "\"privacy\""));
        CHECK(Contains(json, "\"size\":[3,8]"));
        CHECK(DiscordPresence_IsJoinable(Room(true, 3, 8)));
        CHECK(!DiscordPresence_IsJoinable(Room(false, 3, 8)));
    }

    // 3. A full room is not joinable.
    {
        const std::string json = DiscordPresence_ActivityJson(Room(true, 8, 8));
        CHECK(!Contains(json, "\"secrets\""));
        CHECK(!DiscordPresence_IsJoinable(Room(true, 8, 8)));
    }

    // 4. A match with teams, score and timer.
    {
        PresenceSnapshot s;
        s.activity = PresenceActivity::InMatch;
        s.lobbyId = 109775244080946091ull;
        s.roomPublic = true;
        s.memberCount = 2;
        s.memberLimit = 8;
        Copy(s.roomName, kPresenceNameBytes, "Probe");
        Copy(s.opponent, kPresenceNameBytes, "Bob");
        Copy(s.localTeam[0], kPresenceCharBytes, "Ryu");
        Copy(s.localTeam[1], kPresenceCharBytes, "Dante");
        Copy(s.localTeam[2], kPresenceCharBytes, "Zero");
        Copy(s.opponentTeam[0], kPresenceCharBytes, "Hulk");
        Copy(s.opponentTeam[1], kPresenceCharBytes, "Thor");
        Copy(s.opponentTeam[2], kPresenceCharBytes, "Storm");
        s.firstTo = 2;
        s.winsLocal = 1;
        s.winsOpponent = 0;
        s.startUnix = 1700000000;
        Copy(s.version, 16, "0.6.5");

        const std::string json = DiscordPresence_ActivityJson(s);
        CHECK(Value(json, "details") == "vs Bob - 1-0 (FT2)");
        CHECK(Value(json, "state") == "Ryu/Dante/Zero vs Hulk/Thor/Storm");
        CHECK(Contains(json, "\"timestamps\":{\"start\":1700000000}"));
        CHECK(Contains(json, "\"assets\":{\"large_image\":\"timestone\",\"large_text\":\"Timestone 0.6.5\"}"));
        CHECK(Contains(json, "\"instance\":false"));
    }

    // 5. Escaping of quotes, backslashes and control bytes.
    {
        PresenceSnapshot s = Room(true, 1, 8);
        std::snprintf(s.roomName, kPresenceNameBytes, "a\"b\\c%c", '\x01');
        const std::string json = DiscordPresence_ActivityJson(s);
        CHECK(Contains(json, "\"details\":\"Room: a\\\"b\\\\c\\u0001\""));
    }

    // 6. Multi-byte names survive whole, and text over 128 bytes is cut
    //    without splitting a sequence.
    {
        PresenceSnapshot s = Room(true, 1, 8);
        std::string name;
        for (int i = 0; i < 31; ++i) name += "\xC3\xA9";
        Copy(s.roomName, kPresenceNameBytes, name.c_str());
        const std::string json = DiscordPresence_ActivityJson(s);
        const std::string details = Value(json, "details");
        CHECK(details == "Room: " + name);
        CHECK(details.size() <= 128);
        for (std::size_t i = 0; i < details.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(details[i]);
            if (c == 0xC3) CHECK(i + 1 < details.size() && static_cast<unsigned char>(details[i + 1]) == 0xA9);
        }

        PresenceSnapshot watch = Room(true, 1, 8);
        watch.activity = PresenceActivity::Spectating;
        Copy(watch.watchP1, kPresenceNameBytes, name.c_str()); // 62 bytes
        Copy(watch.watchP2, kPresenceNameBytes, name.c_str());
        const std::string cut = Value(DiscordPresence_ActivityJson(watch), "details");
        CHECK(cut.size() <= 128);
        CHECK(cut.size() > 120);
        CHECK(cut.compare(0, 9, "Watching ") == 0);
        for (std::size_t i = 9; i < cut.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(cut[i]);
            if (c == 0xC3) CHECK(i + 1 < cut.size() && static_cast<unsigned char>(cut[i + 1]) == 0xA9);
            if (c == 0xA9) CHECK(static_cast<unsigned char>(cut[i - 1]) == 0xC3);
        }
    }

    // 7. Off publishes nothing and SET_ACTIVITY without an activity clears it.
    {
        PresenceSnapshot s;
        CHECK(DiscordPresence_ActivityJson(s).empty());
        CHECK(DiscordPresence_SetActivityJson("", 42, 7) ==
              "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42},\"nonce\":\"7\"}");
        CHECK(DiscordPresence_HandshakeJson("1544202390536982610") ==
              "{\"v\":1,\"client_id\":\"1544202390536982610\"}");
        CHECK(DiscordPresence_SubscribeJson("ACTIVITY_JOIN", 3) ==
              "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"3\"}");
        CHECK(DiscordPresence_JoinReplyJson("123", true, 4) ==
              "{\"cmd\":\"SEND_ACTIVITY_JOIN_INVITE\",\"args\":{\"user_id\":\"123\"},\"nonce\":\"4\"}");
        CHECK(DiscordPresence_JoinReplyJson("123", false, 5) ==
              "{\"cmd\":\"CLOSE_ACTIVITY_JOIN_REQUEST\",\"args\":{\"user_id\":\"123\"},\"nonce\":\"5\"}");
    }

    // 8. Reading the client's replies.
    {
        const std::string dispatch =
            "{\"cmd\":\"DISPATCH\",\"data\":{\"secret\":\"+connect_lobby 5\"},"
            "\"evt\":\"ACTIVITY_JOIN\",\"nonce\":null}";
        CHECK(Value(dispatch, "evt") == "ACTIVITY_JOIN");
        CHECK(Value(dispatch, "secret") == "+connect_lobby 5");

        const std::string quoted = "{\"data\":{\"message\":\"a \\\"b\\\" c\"}}";
        CHECK(Value(quoted, "message") == "a \"b\" c");

        const std::string request =
            "{\"cmd\":\"DISPATCH\",\"evt\":\"ACTIVITY_JOIN_REQUEST\","
            "\"data\":{\"user\":{\"avatar\":null,\"id\":\"123\",\"username\":\"x\"}}}";
        const std::size_t userAt = request.find("\"user\"");
        CHECK(userAt != std::string::npos);
        std::string id;
        CHECK(DiscordPresence_JsonString(request, "id", id, userAt));
        CHECK(id == "123");

        long long code = 0;
        CHECK(DiscordPresence_JsonInt("{\"evt\":\"ERROR\",\"data\":{\"code\":4006,\"message\":\"no\"}}",
                                      "code", code));
        CHECK(code == 4006);
        CHECK(!DiscordPresence_JsonInt("{\"code\":\"4006\"}", "code", code));
        CHECK(!DiscordPresence_JsonInt("{\"code\":99999999999999999999999}", "code", code));

        std::string missing;
        CHECK(!DiscordPresence_JsonString("{\"a\":1}", "evt", missing));
    }

    // 9. Framing.
    {
        const std::string frame = DiscordPresence_Frame(1, "ab");
        CHECK(frame.size() == 10);
        CHECK(static_cast<unsigned char>(frame[0]) == 1);
        CHECK(static_cast<unsigned char>(frame[4]) == 2);
        CHECK(frame.substr(8) == "ab");
    }

    // 10. First-run question: due only until answered or turned on by hand.
    CHECK(DiscordPresence_ShouldAsk(0, 0));
    CHECK(DiscordPresence_ShouldAsk(2, 0));
    CHECK(!DiscordPresence_ShouldAsk(1, 0));
    CHECK(!DiscordPresence_ShouldAsk(0, 1));
    CHECK(!DiscordPresence_ShouldAsk(1, 1));

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
