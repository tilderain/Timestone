#pragma once

// Spectating a room match.
//
// A match is watched by simulating it: a spectator boots the same battle and
// is fed the two players' confirmed inputs, never any game state. The feed is
// a log kept per game (launch configuration, round seed, opening characters,
// one input pair per frame, a checksum now and then, and how the game ended).
// The two players write that log from their own netplay session, a spectator
// receives it, plays it and serves it on to other spectators: the players
// each feed one spectator at most, however many are watching.
//
// A spectator runs the ordinary netplay client path with a virtual opponent
// (Netplay_InitSpectator): the start-up barrier, round seed, opening
// characters and rematch restarts all go through the code the players use,
// answered from the log instead of from a peer. It never predicts and never
// rolls back; a frame is simulated only once both inputs for it are known.

#include <cstdint>

struct TrainingBootConfig;
struct CharSelPacket;

// --- Recording (the two players) ----------------------------------------------
// A netplay session in a room match begins / is over.
void Spectate_RecBegin();
void Spectate_RecEnd();
bool Spectate_IsRecording();
// The start-up barrier released: a game begins on frame 0 with this seed.
// The game the players are about to load (its teams and stage are set):
// spectators boot it now and wait at the start for Spectate_RecGameStart,
// instead of beginning to load when the players have already begun to play.
void Spectate_RecGameAnnounce(uint32_t gameId);
void Spectate_RecGameStart(uint32_t gameId, uint32_t roundSeed, uint32_t openingP1,
                           uint32_t openingP2);
// Final inputs of the next frame, in order, starting at 0 for each game.
void Spectate_RecFrame(uint32_t p1, uint32_t p2);
int32_t Spectate_RecFrameCount();
// `parts` are the checksum's four components (controller.cpp), for diagnosis.
void Spectate_RecChecksum(int32_t frame, uint32_t checksum, const uint32_t parts[4],
                          const uint32_t rng[6]);
// The game is being restarted (a rematch) with the roster in TrainingBoot.
void Spectate_RecRestart(uint32_t restartSeed);
// The players left the game for the character select; and, from then until
// the restart that ends it, what a team's side of that screen shows (0 =
// player 1). Each player records both sides: its own and the one it is sent.
void Spectate_RecSelectBegin();
void Spectate_RecSelect(int team, const CharSelPacket& packet);
// The same for the select screen a match is started from, before its first
// game: recording begins here (Spectate_RecBegin at the launch then goes on
// from it), and Spectate_RecEnd with no game played says it was backed out of.
void Spectate_RecPreSelectBegin();

// --- Watching ------------------------------------------------------------------
enum class SpectateState : uint8_t {
    Off,
    Joining,  // looking for someone to be fed by, waiting for the first game
    Ready,    // the first game's configuration arrived: boot the battle
    Watching, // in the battle
};

void Spectate_Start(int slot);
void Spectate_Stop();
SpectateState Spectate_GetState();
int Spectate_Slot();
// Ready -> Watching. Fills the configuration to boot with.
bool Spectate_TakeLaunch(TrainingBootConfig* config, int* cpuMode, bool* fiveBars);
// True while the local game is a spectator's simulation of someone's match.
bool Spectate_IsViewing();

// The game being played back: what the start-up barrier needs. False until
// the feed has delivered it.
bool Spectate_GetGameStart(uint32_t* roundSeed, uint32_t* openingP1, uint32_t* openingP2);
uint32_t Spectate_GetInput(uint32_t team, int32_t frame);
// The players' number for the game being played back (0 between games).
// A spectator waiting at the start of a game the players never began (they
// left while it loaded): there is nothing to wait for.
bool Spectate_GameAbandoned();
uint32_t Spectate_PlayGameId();

enum class SpectateStep : uint8_t {
    Hold,    // nothing to simulate yet
    Play,    // simulate this frame (after `fastForward` frames without drawing;
             // -1: one frame time later than usual, the last picture standing)
    Restart, // the game was restarted: do the same (Spectate_GetRestart)
    Leave,   // the match is over
    Select,  // the game is played out and the players are on the character
             // select: go there too (Spectate_OnSelectApplied)
};
SpectateStep Spectate_Next(int32_t frame, int32_t actionStep, int32_t* fastForward);
bool Spectate_GetRestart(TrainingBootConfig* config, uint32_t* restartSeed);
void Spectate_OnRestartApplied();
// Joining, and the players are on the select screen their match starts from:
// the spectator can go there too (then Spectate_OnPreSelectApplied). The
// screen is left through Spectate_TakeLaunch when the first game is there,
// or Spectate_PreSelectEnded when the players backed out.
bool Spectate_PreSelectOpen();
void Spectate_OnPreSelectApplied();
bool Spectate_PreSelectEnded();
// The local game has left the battle for the select screen. From here:
// a team's side of the screen as the feed last had it (`version` changes
// with it), and how the screen ends.
void Spectate_OnSelectApplied();
bool Spectate_GetSelect(int team, CharSelPacket* packet, uint32_t* version);
enum class SpectateSelectEnd : uint8_t {
    Running, // the players are still on it
    Restart, // they started the next game: boot `config`
    Over,    // the match ended there
};
SpectateSelectEnd Spectate_SelectResult(TrainingBootConfig* config, uint32_t* restartSeed);
// After each simulated frame, for the periodic comparison with the players.
bool Spectate_WantsChecksum(int32_t frame);
void Spectate_OnChecksum(int32_t frame, uint32_t checksum, const uint32_t parts[4],
                         const uint32_t rng[6], bool fastForwarded);

struct SpectateStatus {
    int32_t buffered;   // frames received and not played yet
    bool catchingUp;
    bool waiting;       // holding for data
    bool desynced;      // a checksum did not match the players'
    bool feedLost;      // nobody to be fed by right now
};
SpectateStatus Spectate_GetStatus();

// --- Feed (everyone) -----------------------------------------------------------
// Who is where, as published in the room. Filled by the lobby each frame.
struct SpectateRoomMember {
    uint64_t id;
    int8_t slot;        // seated at, -1 if none
    bool fighting;
    bool selecting;     // on the character select their match starts from
    int8_t watchSlot;   // watching, -1 if none
    uint32_t watchSeq;
    uint64_t upstream;  // who feeds them
};
constexpr int kSpectateRoomMembers = 16; // the most a room holds
struct SpectateRoom {
    uint64_t localId;
    int count;
    SpectateRoomMember members[kSpectateRoomMembers];
};
// Once per frame on the game thread, in the lobby and in a battle. Opens and
// closes the links the room calls for and moves the feed along them. Returns
// who the local spectator wants to be fed by (0 when not watching, or nobody
// is available yet); the lobby publishes that.
uint64_t Spectate_Pump(const SpectateRoom& room);
