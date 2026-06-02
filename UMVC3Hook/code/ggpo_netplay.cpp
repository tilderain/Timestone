// ggpo_netplay.cpp
// GGPO session management and callback wiring.
//
// Creates and manages a GGPO session, adding one local and one remote player.
// Bridges GGPO callbacks (save/load game state, advance frame, events) to the
// rollback module and game engine.  The sync input array g_ggpoSyncInputs[]
// is populated by GGPO_SyncInputs() and consumed by controller.cpp's
// hkprocessFrameInput to inject the correct input per player each frame.
#include "ggpo_netplay.h"
#include "rollback.h"
#include <cstdlib>
#include <cstring>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Global GGPO state
// ---------------------------------------------------------------------------
GGPOSession* g_ggpoSession = nullptr;
GGPOPlayerHandle g_ggpoLocalHandle = GGPO_INVALID_HANDLE;
GGPOPlayerHandle g_ggpoRemoteHandle = GGPO_INVALID_HANDLE;
bool g_ggpoActive = false;
bool g_ggpoRunning = false;
bool g_ggpoIsHost = false;

// Synchronised input array — two slots (P1 and P2), populated by
// GGPO_SyncInputs() each frame.  The size is sizeof(uint32_t) per
// player, matching the GGPO payload size set in GGPO_StartSession.
uint32_t g_ggpoSyncInputs[2] = {};

// ---------------------------------------------------------------------------
// GGPO callback declarations
// ---------------------------------------------------------------------------
static bool __cdecl ggpo_begin_game_callback(const char* game);
static bool __cdecl ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame);
static bool __cdecl ggpo_load_game_state_callback(unsigned char* buffer, int len);
static bool __cdecl ggpo_log_game_state_callback(char* filename, unsigned char* buffer, int len);
static void __cdecl ggpo_free_buffer_callback(void* buffer);
static bool __cdecl ggpo_advance_frame_callback(int flags);
static bool __cdecl ggpo_on_event_callback(GGPOEvent* info);

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

// GGPO_StartSession — creates a GGPO session and adds the local + remote
// players.  The session runs asynchronously; call GGPO_WaitForRunning to
// mark it ready, or wait for the GGPO_EVENTCODE_RUNNING event.
bool GGPO_StartSession(bool isHost, const char* remoteIP, unsigned short remotePort, unsigned short localPort)
{
    if (g_ggpoSession) {
        HookLog("GGPO: Session already active, skipping duplicate start\n");
        return false;
    }

    // GGPO SDK internal logging is extremely verbose and spams
    // "prediction ok. proceeding.", "considering incorrect frame -1", etc.
    // _putenv("ggpo.log=true");  // Disabled by default

    GGPOSessionCallbacks cb = {};
    cb.begin_game = ggpo_begin_game_callback;
    cb.save_game_state = ggpo_save_game_state_callback;
    cb.load_game_state = ggpo_load_game_state_callback;
    cb.log_game_state = ggpo_log_game_state_callback;
    cb.free_buffer = ggpo_free_buffer_callback;
    cb.advance_frame = ggpo_advance_frame_callback;
    cb.on_event = ggpo_on_event_callback;

    GGPOErrorCode result = ggpo_start_session(&g_ggpoSession, &cb, "UMVC3", 2, sizeof(uint32_t), localPort);
    if (!GGPO_SUCCEEDED(result))
    {
        HookLog("GGPO: Failed to start session (error %d)\n", result);
        return false;
    }

    g_ggpoIsHost = isHost;

    GGPOPlayer local = {};
    local.size = sizeof(GGPOPlayer);
    local.type = GGPO_PLAYERTYPE_LOCAL;
    local.player_num = isHost ? 1 : 2;

    result = ggpo_add_player(g_ggpoSession, &local, &g_ggpoLocalHandle);
    if (!GGPO_SUCCEEDED(result))
    {
        HookLog("GGPO: Failed to add local player (error %d)\n", result);
        ggpo_close_session(g_ggpoSession);
        g_ggpoSession = nullptr;
        return false;
    }

    GGPOPlayer remote = {};
    remote.size = sizeof(GGPOPlayer);
    remote.type = GGPO_PLAYERTYPE_REMOTE;
    remote.player_num = isHost ? 2 : 1;
    strncpy(remote.u.remote.ip_address, remoteIP, sizeof(remote.u.remote.ip_address) - 1);
    remote.u.remote.ip_address[sizeof(remote.u.remote.ip_address) - 1] = '\0';
    remote.u.remote.port = remotePort;

    result = ggpo_add_player(g_ggpoSession, &remote, &g_ggpoRemoteHandle);
    if (!GGPO_SUCCEEDED(result))
    {
        HookLog("GGPO: Failed to add remote player (error %d)\n", result);
        ggpo_close_session(g_ggpoSession);
        g_ggpoSession = nullptr;
        return false;
    }

    // Set frame delay for local input (GGPO buffers local input by this many
    // frames before delivering it to the game, allowing time for remote input
    // to arrive).
    ggpo_set_frame_delay(g_ggpoSession, g_ggpoLocalHandle, 4);

    // Disconnect timeout: 15 seconds without packets = disconnect.
    ggpo_set_disconnect_timeout(g_ggpoSession, 15000);
    ggpo_set_disconnect_notify_start(g_ggpoSession, 750);

    g_ggpoActive = true;
    HookLog("GGPO: Session started (local port %d, remote %s:%d, isHost=%d)\n",
            localPort, remoteIP, remotePort, isHost ? 1 : 0);

    return true;
}

// GGPO_CloseSession — shuts down the GGPO session and resets all state.
void GGPO_CloseSession()
{
    if (g_ggpoSession)
    {
        Rollback_FreeSavedStates();
        ggpo_close_session(g_ggpoSession);
        g_ggpoSession = nullptr;
    }
    g_ggpoActive = false;
    g_ggpoRunning = false;
    g_ggpoLocalHandle = GGPO_INVALID_HANDLE;
    g_ggpoRemoteHandle = GGPO_INVALID_HANDLE;
    g_ggpoSyncInputs[0] = 0;
    g_ggpoSyncInputs[1] = 0;
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------
bool GGPO_IsActive() { return g_ggpoActive && g_ggpoSession != nullptr; }
bool GGPO_IsHost() { return g_ggpoIsHost; }

// ---------------------------------------------------------------------------
// Input / frame management
// ---------------------------------------------------------------------------

// GGPO_AddLocalInput — submits the local player's physical input to GGPO.
GGPOErrorCode GGPO_AddLocalInput(uint32_t input)
{
    if (!g_ggpoSession) return GGPO_ERRORCODE_INVALID_SESSION;
    return ggpo_add_local_input(g_ggpoSession, g_ggpoLocalHandle, &input, sizeof(uint32_t));
}

// GGPO_SyncInputs — retrieves the synchronised inputs for both players
// from GGPO into g_ggpoSyncInputs[].  Called each frame before game logic.
GGPOErrorCode GGPO_SyncInputs()
{
    if (!g_ggpoSession) return GGPO_ERRORCODE_INVALID_SESSION;
    int disconnectFlags;
    return ggpo_synchronize_input(g_ggpoSession, g_ggpoSyncInputs, sizeof(g_ggpoSyncInputs), &disconnectFlags);
}

// GGPO_AdvanceFrame — notifies GGPO that the current frame has finished
// processing so it can advance its internal state.
GGPOErrorCode GGPO_AdvanceFrame()
{
    if (!g_ggpoSession) return GGPO_ERRORCODE_INVALID_SESSION;
    return ggpo_advance_frame(g_ggpoSession);
}

// GGPO_Idle — pumps the GGPO network layer.  Call periodically (e.g. each
// frame) to process network events and keep the session alive.
GGPOErrorCode GGPO_Idle(int timeout)
{
    if (!g_ggpoSession) return GGPO_ERRORCODE_INVALID_SESSION;
    return ggpo_idle(g_ggpoSession, timeout);
}

// ---------------------------------------------------------------------------
// Network stats
// ---------------------------------------------------------------------------

// GGPO_GetRTT — returns the remote peer's ping in milliseconds.
uint32_t GGPO_GetRTT()
{
    if (!g_ggpoSession) return 0;
    GGPONetworkStats stats;
    if (GGPO_SUCCEEDED(ggpo_get_network_stats(g_ggpoSession, g_ggpoRemoteHandle, &stats)))
        return (uint32_t)stats.network.ping;
    return 0;
}

// GGPO_GetNetworkStats — returns the full network stats structure for the
// remote peer (ping, jitter, packet loss, etc.).
GGPOErrorCode GGPO_GetNetworkStats(GGPONetworkStats* stats)
{
    if (!g_ggpoSession) return GGPO_ERRORCODE_INVALID_SESSION;
    return ggpo_get_network_stats(g_ggpoSession, g_ggpoRemoteHandle, stats);
}

// GGPO_WaitForRunning — marks the session as ready.  After this call the
// game loop begins and GGPO callbacks drive the frame-by-frame sync.
void GGPO_WaitForRunning()
{
    if (!g_ggpoSession || g_ggpoRunning) return;
    HookLog("GGPO: Session started, sync will complete in background\n");
}

// ===========================================================================
// GGPO Callback Implementations
// ===========================================================================

static bool __cdecl ggpo_begin_game_callback(const char* game)
{
    HookLog("GGPO: begin_game callback (%s)\n", game);
    return true;
}

static bool __cdecl ggpo_save_game_state_callback(unsigned char** buffer, int* len, int* checksum, int frame)
{
    return Rollback_SaveGameState(buffer, len, checksum, frame);
}

static bool __cdecl ggpo_load_game_state_callback(unsigned char* buffer, int len)
{
    return Rollback_LoadGameState(buffer, len);
}

static bool __cdecl ggpo_log_game_state_callback(char* filename, unsigned char* buffer, int len)
{
    return true;
}

static void __cdecl ggpo_free_buffer_callback(void* buffer)
{
    free(buffer);
}

static bool __cdecl ggpo_advance_frame_callback(int flags)
{
    Rollback_AdvanceFrame(flags);
    return true;
}

// ggpo_on_event_callback — handles GGPO lifecycle events: connection,
// synchronisation, running state, disconnection, time sync, etc.
static bool __cdecl ggpo_on_event_callback(GGPOEvent* info)
{
    switch (info->code)
    {
        case GGPO_EVENTCODE_CONNECTED_TO_PEER:
            HookLog("GGPO: Connected to peer (player %d)\n", info->u.connected.player);
            break;
        case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
            HookLog("GGPO: Synchronizing with peer %d (%d/%d)\n",
                    info->u.synchronizing.player, info->u.synchronizing.count, info->u.synchronizing.total);
            break;
        case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
            HookLog("GGPO: Synchronized with peer %d\n", info->u.synchronized.player);
            break;
        case GGPO_EVENTCODE_RUNNING:
            g_ggpoRunning = true;
            HookLog("GGPO: Session is RUNNING\n");
            break;
        case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
            HookLog("GGPO: Disconnected from peer %d\n", info->u.disconnected.player);
            g_ggpoActive = false;
            break;
        case GGPO_EVENTCODE_TIMESYNC:
            HookLog("GGPO: Timesync - local is %d frames ahead\n", info->u.timesync.frames_ahead);
            break;
        case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
            HookLog("GGPO: Connection interrupted (player %d, timeout %d)\n",
                    info->u.connection_interrupted.player, info->u.connection_interrupted.disconnect_timeout);
            break;
        case GGPO_EVENTCODE_CONNECTION_RESUMED:
            HookLog("GGPO: Connection resumed (player %d)\n", info->u.connection_resumed.player);
            break;
    }
    return true;
}
