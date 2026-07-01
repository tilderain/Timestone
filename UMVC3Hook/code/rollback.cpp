// rollback.cpp
// Frame-indexed snapshot/restore for deterministic rollback netcode.
//
// Architecture:
//   A ring buffer holds up to ROLLBACK_WINDOW frame snapshots.  Each snapshot
//   is a flat memcpy dump of all MUST-snapshot regions followed by per-region
//   metadata (address + size) so that restore uses the EXACT addresses that
//   were saved — avoiding the stale-pointer problem with dynamically allocated
//   objects (projectiles, effects).
//
//   SaveSnapshot() is called at the frame boundary BEFORE the game tick.
//   RestoreSnapshot(targetFrame) rewinds to that frame for re-simulation.
//   ReSimulate(start, end) chains restore + tick + save for each frame.
//
//   Physics state (~11424 bytes) is embedded inside the sMain manager at
//   offset ~0x31A78.  Capturing the full sMain (0x40080 bytes) covers it.
//
//   Dynamic projectile/effect objects are traversed via intrusive linked
//   lists, with DTI-based sizing for each node.
//
// See doc/rollback.md for the full singleton catalog and per-field analysis.
#include "../pch.h"

#include "rollback.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "diag.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstring>
#include <new>
#include <cstdio>
#include <vector>
#include <string>

extern void HookLog(const char* fmt, ...);

// ===========================================================================
// Addresses — absolute VAs, converted at runtime via _addr()
// ===========================================================================
static constexpr uintptr_t PTR_SREPLAY      = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER   = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT        = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER   = 0x140d44060;
static constexpr uintptr_t PTR_ANEXTBATTLE  = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT        = 0x140e17698;
static constexpr uintptr_t PTR_SCAMERA      = 0x140e17930;

// sMvc3Main__move — the full per-frame update pipeline
static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;

// ===========================================================================
// Offsets & Struct Constants
// ===========================================================================
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uintptr_t TEAM0_OFFSET           = 0x40;
static constexpr uintptr_t TEAM1_OFFSET           = 0x300;
static constexpr size_t    TEAM_SIZE              = 0x2C0;

static constexpr uintptr_t NODE_CHR_OFFSET        = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET       = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

// DTI object size is encoded at +0x30: lower 23 bits = (actual_size >> 2).
// Decode: ((*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2).
// Do NOT read +0x34 — that field is the CRC32 class-name hash, not the size.
// IMPORTANT: The DTI pointer is NOT at object+0x08. Per MtObject__isKindOf, it's
// accessed via vtable[4] = *(void**)(*(uint64_t*)object + 0x20).
static uint64_t GetObjectDti(uint64_t objPtr) {
    uint64_t vtable = *(uint64_t*)objPtr;
    if (vtable && vtable >= 0x140000000) {
        auto fn = (uint64_t(__fastcall*)(uint64_t))(*(uint64_t*)(vtable + 0x20));
        if (fn && (uint64_t)fn >= 0x140000000) {
            return fn(objPtr);
        }
    }
    return 0;
}
static uint32_t DecodeDtiSize(uint64_t dtiPtr) {
    if (!dtiPtr || dtiPtr < 0x140000000) return 0;
    return ((*(uint32_t*)(dtiPtr + 0x30) & 0x7FFFFF) << 2);
}

static constexpr size_t MAX_CHAR_SIZE        = 0x40000;
static constexpr int    MAX_REGIONS          = 512;
static constexpr int    DYNAMIC_SAFETY       = 128; // max projectile/effect nodes
static constexpr int    SSHOT_SENTINEL_COUNT = 10;  // 10 sentinel slots starting at 0x140d47f90
static constexpr size_t SSHOT_NODE_OFFSET    = 0x1450; // uShot->cChrNode offset

// ===========================================================================
// Global Re-Simulation Indicators & Hook State
// ===========================================================================
bool    g_isResimulating         = false;
int32_t g_resimulateCurrentFrame = 0;
int32_t g_postResimFrame         = -1;

// Pending re-sim request from FrameStepper_Process (HookUpdate thread).
// hkMvc3Main_move on the main game thread checks/resets these so the
// actual oMvc3Main_move calls run on the correct thread (avoiding D3D
// thread-safety issues in the render path).
int32_t g_resimRequestedStart   = -1;
int32_t g_resimRequestedEnd     = -1;
int32_t g_restoreRequestedFrame = -1;

int     g_f7State        = 0;
int32_t g_f7StartFrame   = 0;
int     g_f7Remaining    = 0;
int     g_f7DiagSession  = -1;

// Rollback request from controller.cpp — late input arrived that differs
// from what was predicted.  Start/end inclusive frame range.
// __ImageBase gives us the DLL's base address (used with GetModuleFileNameA
// to resolve paths relative to the DLL's directory instead of the CWD, which
// games often change after loading plugins).
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

// Resolve a relative path to one rooted at the DLL's own directory.
// Returns the number of chars written (like snprintf), or negative on error.
// |dst| is always null-terminated.
static int ResolveDllPath(char* dst, size_t dstLen, const char* relative) {
    if (!dst || dstLen == 0) return -1;
    char modPath[260];
    DWORD ret = GetModuleFileNameA((HINSTANCE)&__ImageBase, modPath, sizeof(modPath));
    if (ret == 0 || ret >= sizeof(modPath)) { dst[0] = '\0'; return -1; }
    char* sep = strrchr(modPath, '\\');
    if (sep) *(sep + 1) = '\0'; else modPath[0] = '\0';
    return snprintf(dst, dstLen, "%s%s", modPath, relative);
}

static int32_t g_rollbackReqStart = -1;
static int32_t g_rollbackReqEnd   = -1;
static uint64_t g_sMvc3Main      = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;
// Trampoline for the render/present call — NOP'd during re-simulation
static void(__fastcall* oCallSRender)(uint64_t param_1) = nullptr;
// Trampoline for sMain__move (frame-timing / QPC / Sleep / busy-wait)
static void(__fastcall* oMain_move)(uint64_t param_1) = nullptr;
// Trampoline for the collision sound-event iterator (hook crashes during re-sim)
static void(__fastcall* oCollisionSoundUpdate)(uint64_t param_1) = nullptr;

// ===========================================================================
// Resim diagnostic dumper — binary + text state capture
// ===========================================================================
static int    g_diagSessionId  = 0;
static char   g_diagDir[260]   = {};
static int    g_diagWriteCount = 0;
static FILE*  g_diagNdjsonFile    = nullptr;
static DWORD  g_diagNdjsonFlush   = 0;

// Netplay per-frame live NDJSON logging
static bool   g_netplayLiveInit   = false;
static char   g_netplayLivePath[260] = {};
static int    g_netplayLiveReset   = 0;

static std::string g_pendingNPLiveJSON[256];
static int32_t     g_pendingNPLiveFrame[256];

// Ground-truth verification: locally queued re-simulation states, paired with
// remote ground truth data upon receipt of a GroundTruthPacket.
struct PendingResimDump {
    int32_t frame;
    ResimCharState p1Post;
    ResimCharState p2Post;
};
static constexpr int PENDING_RESIM_DUMP_RING = 256;
static PendingResimDump g_pendingResimDumps[PENDING_RESIM_DUMP_RING] = {};
static bool             g_pendingResimDumpValid[PENDING_RESIM_DUMP_RING] = {};

// ===========================================================================
// Diag Snapshot Accessors — the ONLY place that should read g_resimFrameSnap
// / g_liveFrameSnap. Centralizes the frame-stamp check that was previously
// missing at three separate call sites.
// ===========================================================================
static const FrameDiagSnapshot* Rollback_GetResimSnapshot(int32_t frame) {
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG;
    const FrameDiagSnapshot& snap = g_resimFrameSnap[idx];
    if (snap.frame != frame) {
        HookLog("Rollback: stale resim diag slot for frame %d (slot holds frame %d) — refusing read\n",
                frame, snap.frame);
        return nullptr;
    }
    return &snap;
}

static const FrameDiagSnapshot* Rollback_GetLiveSnapshot(int32_t frame) {
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG;
    const FrameDiagSnapshot& snap = g_liveFrameSnap[idx];
    if (snap.frame != frame) {
        HookLog("Rollback: stale live diag slot for frame %d (slot holds frame %d) — refusing read\n",
                frame, snap.frame);
        return nullptr;
    }
    return &snap;
}

// Pulls P1/P2 point-character post-state out of a snapshot. Returns false
// if either character isn't present (so callers don't have to repeat the
// teamIdx/charSlot scan loop).
static bool Rollback_FindP1P2Post(const FrameDiagSnapshot& snap,
                                   const CharDiagSnapshot** p1Post,
                                   const CharDiagSnapshot** p2Post)
{
    *p1Post = nullptr;
    *p2Post = nullptr;
    for (int ci = 0; ci < snap.charCount && ci < 6; ci++) {
        uint32_t teamIdx = snap.chars[ci].post.teamIdx;
        uint32_t slot    = snap.chars[ci].post.charSlot;
        if (teamIdx == 0 && slot == 0) *p1Post = &snap.chars[ci].post;
        if (teamIdx == 1 && slot == 0) *p2Post = &snap.chars[ci].post;
    }
    return *p1Post && *p2Post;
}

static void Rollback_FillResimCharState(ResimCharState& dst, const CharDiagSnapshot& s)
{
    dst.actionId = s.actionId;
    dst.posX     = s.posX;
    dst.posY     = s.posY;
    dst.posZ     = s.posZ;
    dst.flags    = s.flags14F0;
    dst.mHeld    = *(uint32_t*)(s.cchr + 0x14);
    dst.frameCnt = *(uint64_t*)(s.cchr + 0x18);
    for (int ri = 0; ri < 4; ri++)
        dst.rng[ri] = s.xorShift[ri];
}

static void Rollback_NetplayLiveInit() {
    g_netplayLiveInit = false;
    g_netplayLivePath[0] = '\0';

    char dir[260];
    SYSTEMTIME st;
    GetLocalTime(&st);
    char rel[80];
    
    // Append the local netplay role as a suffix to the directory name
    const char* sideSuffix = Netplay_IsActive() ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    snprintf(rel, sizeof(rel), "nplive_%04d%02d%02d_%02d%02d%02d%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, sideSuffix);
             
    ResolveDllPath(dir, sizeof(dir), rel);
    CreateDirectoryA(dir, NULL);

    snprintf(g_netplayLivePath, sizeof(g_netplayLivePath), "%s/frames.ndjson", dir);
    FILE* f = fopen(g_netplayLivePath, "w");
    if (f) fclose(f);

    for (int i = 0; i < 256; i++) {
        g_pendingNPLiveFrame[i] = -1;
        g_pendingNPLiveJSON[i].clear();
    }

    HookLog("NetplayLive: NDJSON log -> %s\n", g_netplayLivePath);
    g_netplayLiveInit = true;
}

std::string Rollback_GenerateNetplayLiveJSON(int32_t frame, bool isResim) {
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string sideStr = Netplay_IsHost() ? "host" : "client";
    if (isResim) {
        sideStr += "_resim";
    }
    const char* side = sideStr.c_str();

    uint32_t recvInput = Controller_GetReceivedRemoteInput(frame);
    uint32_t usedInput = Controller_GetUsedRemoteInput(frame);
    uint32_t localInput = Controller_GetRecordedLocalInput(frame);

    char header[512];
    snprintf(header, sizeof(header),
             "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\","
             "\"inputs\":{\"local\":\"0x%08X\",\"usedRemote\":\"0x%08X\",\"recvRemote\":\"0x%08X\"},"
             "\"chars\":[",
             tsBuf, frame, side, localInput, usedInput, recvInput);

    std::string out = header;

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    bool firstChar = true;
    if (sCharacter) {
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
            int ci = 0;
            while (node) {
                uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (chr) {
                    CharDiagSnapshot snap;
                    CaptureCharSnapshot(chr, ti, ci, snap);

                    if (!firstChar) out += ",";
                    firstChar = false;

                    char charBuf[512];
                    snprintf(charBuf, sizeof(charBuf),
                            "{\"team\":%d,\"slot\":%d,"
                            "\"pos\":[%.4f,%.4f,%.4f],"
                            "\"act\":\"0x%08X\",\"fl\":\"0x%08X\","
                            "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                            "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                            ti, ci,
                            snap.posX, snap.posY, snap.posZ,
                            snap.actionId, snap.flags14F0,
                            *(uint32_t*)(snap.cchr + 0x14),
                            *(uint64_t*)(snap.cchr + 0x18),
                            snap.xorShift[0], snap.xorShift[1],
                            snap.xorShift[2], snap.xorShift[3]);
                    out += charBuf;
                    ci++;
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    out += "]}\n";
    return out;
}

static void Rollback_WriteNetplayLiveJSON(const std::string& json) {
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0]) return;

    FILE* f = fopen(g_netplayLivePath, "a");
    if (!f) return;

    fputs(json.c_str(), f);
    fclose(f);
}

extern "C" void Rollback_CommitPendingNetplayLive(int32_t frame) {
    int idx = (uint32_t)frame % 256;
    if (g_pendingNPLiveFrame[idx] == frame) {
        Rollback_WriteNetplayLiveJSON(g_pendingNPLiveJSON[idx]);
        g_pendingNPLiveFrame[idx] = -1;
    }
}

extern "C" void Rollback_DiscardPendingNetplayLive(int32_t frame) {
    int idx = (uint32_t)frame % 256;
    if (g_pendingNPLiveFrame[idx] == frame) {
        g_pendingNPLiveFrame[idx] = -1;
    }
}

static void Rollback_DumpNetplayLive(int32_t frame) {
    if (!g_netplayLiveInit) Rollback_NetplayLiveInit();
    if (!g_netplayLivePath[0]) return;

    std::string json = Rollback_GenerateNetplayLiveJSON(frame, false);

    int idx = (uint32_t)frame % 256;
    if (Controller_IsFrameConfirmed(frame)) {
        Rollback_WriteNetplayLiveJSON(json);
        g_pendingNPLiveFrame[idx] = -1;
    } else {
        g_pendingNPLiveJSON[idx] = json;
        g_pendingNPLiveFrame[idx] = frame;
    }
}

// Captures character state into the diag ring buffer.
// If postOnly is false (default): captures both pre and post from the current
//   memory state (used for live frames and initial resim pre-capture).
// If postOnly is true: captures only the post-state, leaving pre untouched
//   (used for resim post-capture after the frame tick to preserve the pre-tick
//    state that was captured before oMvc3Main_move).
static void Rollback_CaptureFrameDiagSnapshot(int32_t frame, bool isResim, bool postOnly = false)
{
    FrameDiagSnapshot& dst = isResim ? g_resimFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG]
                                      : g_liveFrameSnap[(uint32_t)(frame >= 0 ? frame : 0) % ROLLBACK_WINDOW_DIAG];
    if (!postOnly) {
        dst.charCount = 0;
        dst.frame = frame;
    }

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    int ci = 0;
    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        int slotInTeam = 0;
        while (node && (postOnly ? (ci < dst.charCount) : (dst.charCount < 6))) {
            uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (chr) {
                CharDiagSnapshot snap;
                CaptureCharSnapshot(chr, ti, slotInTeam, snap);
                if (postOnly) {
                    dst.chars[ci].post = snap;
                } else {
                    dst.chars[dst.charCount].pre  = snap;
                    dst.chars[dst.charCount].post = snap;
                    dst.charCount++;
                }
                ci++;
                slotInTeam++;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
}

static void Rollback_DiagInit();

// ---------------------------------------------------------------------------
// Rollback_LogResimNotifyNDJSON — called on the RECEIVER side when a resim
// notify packet arrives from the remote peer.  Writes the sender's pre-state
// as side="ground_truth" and post-state as side="resim" to both the nplive
// NDJSON and the rdiag.ndjson (when available).
// ---------------------------------------------------------------------------
void Rollback_LogResimNotifyNDJSON(const ResimNotifyPacket& pkt)
{
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const char* senderSide = Netplay_IsHost() ? "host" : "client";

    auto writeEntry = [&](FILE* f, int32_t frame, const char* side,
                          const ResimCharState& p1, const ResimCharState& p2)
    {
        fprintf(f, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,"
                   "\"side\":\"%s\",\"resim_notify\":true,"
                   "\"senderSide\":\"%s\",\"correctedTeam\":%u,"
                   "\"corrected_frames\":[%d,%d],"
                   "\"inputs\":{\"local\":\"—\",\"usedRemote\":\"0x%08X\",\"recvRemote\":\"0x%08X\"},"
                   "\"chars\":[",
                tsBuf, frame, side, senderSide, pkt.correctedTeam,
                pkt.resimStartFrame, pkt.resimEndFrame,
                pkt.frames[0].usedInput, pkt.frames[0].recvInput);
        // P1 point
        fprintf(f, "{\"team\":0,\"slot\":0,"
                   "\"pos\":[%.4f,%.4f,%.4f],"
                   "\"act\":\"0x%08X\",\"fl\":\"0x%08X\","
                   "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                   "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                p1.posX, p1.posY, p1.posZ,
                p1.actionId, p1.flags,
                p1.mHeld, (unsigned long long)p1.frameCnt,
                p1.rng[0], p1.rng[1], p1.rng[2], p1.rng[3]);
        // P2 point
        fprintf(f, ",{\"team\":1,\"slot\":0,"
                   "\"pos\":[%.4f,%.4f,%.4f],"
                   "\"act\":\"0x%08X\",\"fl\":\"0x%08X\","
                   "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                   "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                p2.posX, p2.posY, p2.posZ,
                p2.actionId, p2.flags,
                p2.mHeld, (unsigned long long)p2.frameCnt,
                p2.rng[0], p2.rng[1], p2.rng[2], p2.rng[3]);
        fprintf(f, "]}\n");
    };

    // --- Write to rdiag.ndjson (pre-state as ground_truth, post-state as resim) ---
    // Initialize the diag directory if this side hasn't created one yet
    // (e.g. receiver that never ran Rollback_ReSimulate locally).
    // Remote notify packets are kept out of nplive to ensure nplive only logs local simulation.
    Rollback_DiagInit();
    if (g_diagNdjsonFile) {
        for (uint32_t fi = 0; fi < pkt.frameCount; fi++) {
            const auto& rf = pkt.frames[fi];
            writeEntry(g_diagNdjsonFile, rf.frame, "ground_truth", rf.p1Pre, rf.p2Pre);
            writeEntry(g_diagNdjsonFile, rf.frame, "resim", rf.p1Post, rf.p2Post);
        }
        DWORD now = GetTickCount();
        if (now - g_diagNdjsonFlush >= 1000) {
            fflush(g_diagNdjsonFile);
            g_diagNdjsonFlush = now;
        }
    }
}

// static decltype(&QueryPerformanceCounter) g_real_qpc = nullptr;
// static uint64_t g_virtual_qpc = 0;
// static DWORD g_main_thread_id = 0;
// static uint64_t g_qpc_history[ROLLBACK_WINDOW] = {};
// 
// static BOOL WINAPI hooked_qpc(LARGE_INTEGER* out) {
//     if (g_isResimulating && GetCurrentThreadId() == g_main_thread_id) {
//         if (out) out->QuadPart = (LONGLONG)g_virtual_qpc;
//         return TRUE;
//     }
//     if (!g_real_qpc) return FALSE;
//     return g_real_qpc(out);
// }

// Frame counter / rollback state from controller.cpp
extern "C" int32_t Controller_GetNetFrameCount();
extern "C" bool    Controller_IsRollbackEnabled();
extern "C" uint32_t Controller_GetCurrentFrameInput();
extern "C" void    Controller_InjectLocalInput(int32_t frame, uint32_t input);
extern "C" void    Controller_InjectRemoteInput(int32_t frame, uint32_t input);
extern "C" void    Controller_SilentFrameStart();
extern "C" void    Controller_SilentFrameEnd();
extern "C" void    Controller_SaveInputHistory();
extern "C" void    Controller_RestoreInputHistory();
extern "C" void    Controller_ResolveRemoteInput(int32_t frame);

// ===========================================================================
// Snapshot Region Descriptor & Ring Buffer
// ===========================================================================
struct Region {
    void*    ptr;  // source/destination address in game memory
    size_t   size; // number of bytes
};

struct SnapshotSlot {
    int32_t  frame       = -1;    // game frame this slot holds (-1 = invalid)
    uint8_t* data        = nullptr; // compact linear memory pool
    size_t   size        = 0;       // total bytes of data written
    size_t   capacity    = 0;       // allocated capacity of data[]
    Region   regions[MAX_REGIONS];  // exact addresses captured at save time
    int      regionCount = 0;       // number of populated regions[]
    uint32_t localInput  = 0;      // local player's delayed input for this frame
    uint32_t remoteInput = 0;      // best-known remote input for this frame
};

static SnapshotSlot g_slots[ROLLBACK_WINDOW];

// ===========================================================================
// Safe Dynamic List Traversal
//
// Walks an intrusive linked list starting at |listHead|, reading each node's
// DTI to determine object size.  Bounded by DYNAMIC_SAFETY to prevent
// infinite loops on corrupted lists.
// ===========================================================================
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0) {
        // Reject obviously-bogus pointers
        if (current < 0x10000) break;

        // Read object size from DTI (accessed via vtable[4], not object+0x08)
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE) {
            objSize = 0x400; // conservative fallback for typical nodes
        }

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        ++r;

        current = *(uint64_t*)(current + nextOffset);
    }
}

// ===========================================================================
// Region List Builder — enumerates all snapshot-worthy memory
// ===========================================================================
static int BuildRegions(Region* regions, int maxRegions)
{
    if (!regions || maxRegions < 1) return 0;

    int r = 0;
    auto add = [&](void* ptr, size_t sz) {
        if (r < maxRegions && ptr && sz > 0) {
            regions[r].ptr  = ptr;
            regions[r].size = sz;
            ++r;
        }
    };

    // ---- 1. sReplay: frame counter only (sReplay+0x54). Full replay buffer
    //     has internal pointers that go stale. ---
    uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (sReplay) {
        add((void*)(sReplay + 0x54), 4); // just the frame counter
    }

    // ---- 1a. Global XorShift RNG state (16 bytes, 4 × uint32) ----
    //     Used by 64/73 call sites across gameplay (hit roll, damage variance,
    //     AI decisions, projectile patterns).  Without restoring this, re-simulation
    //     with different inputs consumes RNG differently, causing desyncs in all
    //     RNG-derived behavior between the original and re-simulated timelines.
    //     Address confirmed from LEA RAX,[0x140d765d8] in MtXorShift__getGlobalState.
    add((void*)_addr(0x140d765d8), 16);

    // ---- 2. sShot manager — vtable + shot ID pool + sentinel slot array only ----
    //     Snapshotting only up to +0x1E0 (end of 10 × 0x28 sentinel slots at +0x50)
    //     to avoid corrupting CRITICAL_SECTION objects at +0x1E0 and the
    //     pointer-heavy activity/pending regions at +0x370+ that go stale.
    uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
    if (sShot) {
        add((void*)sShot, 0x1E0);
    }

    // ---- 2a. Slot active flags (NOT in sShot — stored in sGameConfig+0x4A89C) ---
    //     sShot__setSlotActiveFlag writes to uHaggar__getter_49F0(sGameConfig) + 0x48A6c.
    //     uHaggar__getter_49F0 returns sGameConfig + 0x1E30, so the flags live at
    //     sGameConfig + 0x1E30 + 0x48A6c = sGameConfig + 0x4A89C.  10 slots × 1 byte.
    //     Without restoring these the game sees no active projectiles and frees them,
    //     causing stale-address corruption on the next restore.
    {
        uint64_t sGameConfig = *(uint64_t*)_addr(0x140D50F88);
        if (sGameConfig) {
            add((void*)(sGameConfig + 0x4A89C), 10);
        }
    }

    // ---- 2b. uShot projectile objects — walk each sentinel's circular list ---
    //     Each sentinel starting at 0x140d47f90 + slot*0x28 is an embedded cChrNode.
    //     A sentinel's "node reference" is sentinel+0x08. mpNext at sentinel+0x10
    //     stores the first real uShot's nodeRef (= uShot + SSHOT_NODE_OFFSET + 0x08).
    //     We follow nodeRef->mpNext (at *(nodeRef + 0x08)) back to the sentinel.
    int shotCount = 0;
    {
        for (int si = 0; si < SSHOT_SENTINEL_COUNT; ++si) {
            uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + si * 0x28;
            uint64_t sentinelRef = sentinel + 0x08;
            uint64_t curRef = *(uint64_t*)(sentinel + 0x10);
            int safety = DYNAMIC_SAFETY;
            while (curRef != sentinelRef && safety-- > 0) {
                if (curRef < 0x10000) break;
                uint64_t uShot = curRef - 0x08 - SSHOT_NODE_OFFSET;

                // uShot+0x08 holds list pointers, NOT a DTI descriptor.
                // Using a DTI lookup here reads garbage and produces a 1-byte
                // snapshot, truncating the projectile.  Use a flat safe size.
                uint32_t objSize = 0x2200;

                add((void*)uShot, objSize);
                shotCount++;
                curRef = *(uint64_t*)(curRef + 0x08);
            }
        }
    }
    if (shotCount > 0) {
        HookLog("Snapshot Diagnosis: Captured %d active uShot projectiles on heap\n", shotCount);
    }

    // ---- 3. sHitSolver — collision pairs + result buffer (static array) ----
    {
        uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
        add((void*)sHitSolver, 0x34080);
    }

    // ---- 4. aNextBattle — battle state machine (static manager) ----
    {
        uint64_t aNextBattle = *(uint64_t*)_addr(PTR_ANEXTBATTLE);
        add((void*)aNextBattle, 0x20000);
    }

    // ---- 5. sGameUI — SKIPPED: has internal ptrs (ui elements, text buffers) ---
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // ---- 6. sMain — physics, timing, frame pacing (needed for re-sim consistency) ----
    {
        uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
        add((void*)sMain, 0x40080);
    }

    // ---- 7. sCamera — view matrices, 8-slot camera stack, cutin state ----
    //     3456 B total; skip vtable @+0 .. +7 on restore (handled via flat
    //     memcpy — the vtable pointer is the same at save and restore time
    //     for a singleton, so restoring it is safe here).
    {
        uint64_t sCamera = *(uint64_t*)_addr(PTR_SCAMERA);
        if (sCamera) {
            add((void*)sCamera, 0xD80);
        }
    }

    // ---- 8. sGameEffect — SKIPPED: individual effect nodes are captured ---
    //     via the sUnit category traversal (categories 13-16) below.  Flat-
    //     restoring sGameEffect would crash due to stale effect pointers.

    // ---- 10. sAction (fighting phase state machine) ----
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (sMvc3Mgr) {
        uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300);
    }

    // ---- 11-16. Per-character uCharacter + 17-18. cChrTeam ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE);
        add((void*)team1, TEAM_SIZE);

        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

            while (node && r < maxRegions) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character) {
                    uint64_t vtable = *(uint64_t*)character;
                    uint64_t dtiFunc = (vtable >= 0x140000000) ? *(uint64_t*)(vtable + 0x20) : 0;
                    uint64_t dtiPtr = GetObjectDti(character);
                    uint32_t charSize = DecodeDtiSize(dtiPtr);
                    uint32_t finalSize = charSize;
                    if (finalSize == 0 || finalSize > MAX_CHAR_SIZE) {
                        finalSize = 0x20000;
                    }
                    uint32_t* animFields = (uint32_t*)(character + 0x1300);
                   // HookLog("DIAG Character[team %d]: addr=0x%llX vtable=0x%llX dtiFunc=0x%llX dtiPtr=0x%llX rawSize=%u finalSize=%u\n"
                 //           "DIAG   anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                 //           ti, character, vtable, dtiFunc, dtiPtr, (unsigned)charSize, (unsigned)finalSize,
                 //           animFields[0], animFields[1], animFields[2], animFields[3],
                //            animFields[4], animFields[5], animFields[6], animFields[7],
                 //           animFields[8], animFields[9], animFields[10], animFields[11],
                 //           animFields[12], animFields[13], animFields[14], animFields[15]);
                    add((void*)character, finalSize);

                    // cChrInput is embedded inside uCharacter at +0x21C8.
                    // The full cChrInput struct is ~0x5A0 bytes covering all
                    // ring buffers (mHeldRing, mReleasedRing, mPressedRing),
                    // direction history (512B at +0x360), final processed fields,
                    // and the +0x590/+0x594 fields read by getInputDirection.
                    // Snapshot the entire range so direction-history and
                    // ring-buffer-head state survive rollback restores.
                    uint64_t cChrInput = character + 0x21C8;
                    add((void*)cChrInput, 0x5A0);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    // ---- 19-22. sUnit effect categories — SKIPPED (heap corruption) ---
    //     Walking effect categories 13-16 and restoring individual nodes at
    //     their heap addresses corrupts the pool allocator's free-list metadata
    //     when nodes were freed between save and restore (MtHeapAllocator crash).
    //     Instead, we zero the sGameEffect slot tables post-restore to allow
    //     fresh effect allocations without conflicting with old slots.

    return r;
}

// ===========================================================================
// Sleep-Patch & Headless Tick — game-logic-only re-simulation stepping
// ===========================================================================
static constexpr uintptr_t FN_SMAIN_MOVE            = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_MANAGERS        = 0x140258540;
static constexpr uintptr_t FN_PAD_PROCESS            = 0x140289c30;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE    = 0x140001af0;
static constexpr uintptr_t FN_MVC3MGR_GETACTION      = 0x1409629d0;
static constexpr uintptr_t FN_AFTER_GETACTION        = 0x14014f410;
static constexpr uintptr_t FN_CALL_401E0             = 0x1405bb710;  // direct call on sMain+0x401e0
static constexpr uintptr_t FN_CALL_AI                = 0x140647910;  // direct call on sMain+0x40240 (AI)
static constexpr uintptr_t FN_SMAIN_TAIL             = 0x140259ea0;  // tail function with sMain as arg
static constexpr uintptr_t FN_CALL_SRENDER           = 0x14053a250;  // call on sMain+0x40190 (sRender)
static constexpr uintptr_t FN_COLLISION_SOUND_UPDATE = 0x1405b72b0;  // collision sound-event iterator
static constexpr uintptr_t FN_RESET_HITNODES         = 0x140259e90;  // j_sHitSolver__resetActiveNodes
static constexpr uintptr_t FN_PAD_METHOD_3D00        = 0x1402a3d00;  // sPad method
static constexpr uintptr_t FN_PAD_METHOD_3DC0        = 0x1402a3dc0;  // sPad method

// sMvc3Manager subsystem getters (for MgrSubsysV30 helper)
static constexpr uintptr_t FN_GETGAMECONFIG          = 0x1401bcc50;
static constexpr uintptr_t FN_GETGAMEUI              = 0x14097d370;
static constexpr uintptr_t FN_GETSHOT                = 0x140683730;
static constexpr uintptr_t FN_GETGAMEEFFECT          = 0x140015690;
static constexpr uintptr_t FN_GETHITSOLVER           = 0x140056130;
static constexpr uintptr_t FN_GETCHARACTER           = 0x1407bcd80;
static constexpr uintptr_t FN_GETSTAGE               = 0x140259460;
static constexpr uintptr_t FN_GETREPLAY              = 0x14013c430;
static constexpr uintptr_t FN_GETBATTLESET           = 0x1408f1f10;
static constexpr uintptr_t FN_GETMVC3SET             = 0x14004b000;
static constexpr uintptr_t FN_GETNETWORK             = 0x1407cda50;
static constexpr uintptr_t FN_GETLICENSE             = 0x14010a200;
static constexpr uintptr_t FN_GETRANKING             = 0x140703370;
static constexpr uintptr_t FN_GETNETROOM             = 0x14030e6c0;
static constexpr uintptr_t FN_GETEARTHATK            = 0x140327a10;

// Sleep-patch state: the two Sleep() call sites inside sMain__move
// (0x140521df0).  Each is a 6-byte "CALL [RIP+xxx]" through the IAT.
static constexpr uintptr_t SLEEP_CALL_FIXED = 0x1405220b2;  // Sleep(dt-1)
static constexpr uintptr_t SLEEP_CALL_VAR   = 0x140521f93;  // Sleep(1)
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c; // CALL [RAX+0x48] inside FUN_140537080
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4; // vtable+0x50 on sMain+0x40208
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1; // vtable+0x50 on sMain+0x40270
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113; // vtable+0x50 in FUN_140537080 (inside if-block)
static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool    g_sleepPatched = false;
static bool    g_presentPatched = false;
static bool    g_swapchainPatched = false;

// Helper: call a vtable method on an object pointer stored at |offset| in
// sMvc3Main.  vtable_offset is the byte offset into the vtable (e.g. 0x30).
static inline void VTableCall(uint64_t sMain, uint32_t memberOffset, uint32_t vtableOffset)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    uint64_t vtable = *(uint64_t*)obj;
    if (!vtable) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (fn) fn(obj);
}

// Helper: call a non-vtable direct function with an object from sMain member.
static inline void DirectMemberCall(uint64_t sMain, uint32_t memberOffset, uintptr_t funcAddr)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    ((void(__fastcall*)(uint64_t))_addr(funcAddr))(obj);
}

// Helper: sMvc3Manager::getInstance() → getter(instance) → vtable(vfnOffset) on result.
static inline void MgrSubsysCall(uintptr_t getterAddr, uint32_t vfnOffset)
{
    uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    if (!inst) return;
    uint64_t obj = ((uint64_t(__fastcall*)(uint64_t))_addr(getterAddr))(inst);
    if (!obj) return;
    uint64_t vt = *(uint64_t*)obj;
    if (!vt) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vt + vfnOffset);
    if (fn) fn(obj);
}

// Helper: Aligns restored sMain structures with actual performance counters
// to prevent the game loop from computing a negative/excessive frame-time
// delta after re-simulation, which would cause a freeze in the busy-wait
// or in the Sleep-based frame-pacing loop.
static void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Sync high-resolution performance-counter samples with physical counter.
    // sMain+0x40 is the "base QPC" sampled at the PREVIOUS frame boundary;
    // sMain__move computes the frame delta as (current_QPC - base) * 1000 / freq.
    // Without a valid base the first post-re-sim delta will be enormous and
    // the frame-pacer and/or tick-count multiplier will produce wrong results.
    *(uint64_t*)(sMain + 0x40)     = qpc.QuadPart;  // base QPC for delta
    *(uint64_t*)(sMain + 0x40048)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70)     = qpc.QuadPart;

    // Reset timing deltas to 60 FPS nominal values so the next real tick
    // sees a well-formed 16 ms frame delta.
    *(uint32_t*)(sMain + 0x38)     = 16;           // ~16ms frame delta
    *(float*)(sMain + 0x80)        = 0.01666667f;  // 60fps float delta
    *(uint32_t*)(sMain + 0xA8)     = 16;           // current delta
    *(uint32_t*)(sMain + 0xA4)     = 16;           // shifted delta

    // Zero accumulated sleep/deadline calculations so they recompute fresh.
    // These are the "accumulated time" (sMain+0x60) and "deadline" (sMain+0x68)
    // used by the frame-pacing busy-wait logic.  If left stale the game may
    // spin forever waiting for sMain+0x60 < sMain+0x68 to become false.
    *(uint64_t*)(sMain + 0x60)     = 0;
    *(uint64_t*)(sMain + 0x68)     = 0;

    // Set tick count so the next real frame advances by exactly 1 step.
    // (Without this the restored frame count may jump or stall.)
    *(uint32_t*)(sMain + 0x48)     = 1;
}

void Rollback_PatchSleep(bool enable)
{
    if (enable && !g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = true;
    } else if (!enable && g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = false;
    }
}

void Rollback_PatchPresent(bool enable)
{
    // NOP / restore the 3-byte CALL [RAX+0x48] inside FUN_140537080.
    // This Present call blocks for ~16ms per frame under DXVK; NOPping it
    // during re-sim lets the rest of the render-sync function run normally
    // (setting render-done flags, advancing frame counter, etc.) without
    // the blocking Present.  Required because the game crashes if we skip
    // FUN_140537080 entirely (downstream game-logic depends on its state).
    if (enable && !g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = true;
    } else if (!enable && g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = false;
    }
}

void Rollback_PatchSwapchain(bool enable)
{
    // NOP / restore three vtable+0x50 call sites that are part of the
    // swapchain / end-frame path in sMvc3Main__move and FUN_140537080.
    // Each instruction is "CALL [RAX+0x50]" = FF 50 50 (3 bytes).
    //
    // These swapchain operations expect a Present to have been submitted;
    // with the Present NOP'd during re-sim they may do unnecessary work or
    // block.  NOPping them eliminates any remaining D3D9/Vulkan overhead
    // from the re-sim path.
    static const uintptr_t sites[3] = {
        SWAPCHAIN_CALL_A,
        SWAPCHAIN_CALL_B,
        SWAPCHAIN_CALL_C,
    };
    if (enable && !g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = true;
    } else if (!enable && g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = false;
    }
}

// ===========================================================================
// Rollback_PatchDrawCalls — NOP all remaining render/draw call sites
//
// Every render-related CALL inside sMvc3Main__move that is still running
// during re-sim.  Comment out individual entries in the `sites` array
// below to test whether skipping that call site crashes the game.
//
// Sites organised by category:
//   A–G:  vtable+0x48 indirect draw calls (3 bytes each)
//   H–K:  Direct render-function CALLs (5 bytes each)
//   L:    vtable+0x30 render-logic update (3 bytes)
// ===========================================================================
void Rollback_PatchDrawCalls(bool enable)
{
    static struct { uintptr_t addr; int n; const char* label; } const sites[] = {
        // --- vtable+0x48 DRAW calls (FF 50 48 / FF 52 48) ---
        { 0x1402594d9, 3, "draw_401a8"       },
        { 0x1402594e6, 3, "draw_40198"       },
        { 0x1402594f3, 3, "draw_401d8"       },
        { 0x140259515, 3, "draw_gameConfig"  },
        { 0x140259568, 3, "draw_40270"       },
        { 0x140259575, 3, "draw_40208"       },
        { 0x140259896, 3, "draw_401e0"       },
        // --- Direct render-function CALLs (E8 rel32 = 5 bytes) ---
        { 0x140259525, 5, "setRenderTargetAspect" },
        { 0x140259537, 5, "util_C7D0"             },
        { 0x140259548, 5, "setField_40"            },
        { 0x140259559, 5, "getter_7110"            },
        // --- vtable+0x30 render-logic update ---
        { 0x1402598ab, 3, "sRender_vtable+0x30"   },
    };
    static constexpr int N = sizeof(sites) / sizeof(sites[0]);

    // Saved bytes storage — indexed identically to sites[]
    static uint8_t saved[N][6] = {};   // max 6 bytes per site
    static bool    patched = false;

    if (enable && !patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(saved[i], (void*)_addr(sites[i].addr), sites[i].n);
            memset((void*)_addr(sites[i].addr), 0x90, sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
            //HookLog("Rollback: NOP'd draw call %s @ 0x%llX (%d bytes)\n",
            //        sites[i].label, (uint64_t)_addr(sites[i].addr), sites[i].n);
        }
        patched = true;
    } else if (!enable && patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i].addr), saved[i], sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
        }
        patched = false;
    } else if (!enable && !patched) {
        return; // nothing to do
    } else {
        //HookLog("Rollback: draw calls already patched, skipping\n");
    }
}

void Rollback_HeadlessTick(uint64_t sMain)
{
    if (!sMain) 
    {
        HookLog("Rollbackheadlesstick: smain is bad?:? :>\n");
        return;
    }

    HookLog("Rollback_HeadlessTick: begin\n");

    // ---- Disable collision sound-event updates during re-sim ----
    // sCollision__move checks the flag at collisionObj+0x73a8 before
    // calling its sound-event iterator (FUN_1405B72B0).  During re-sim the
    // sound slot-index increments past the per-entry capacity, hitting a
    // null-pointer bug in the iterator.  Sound doesn't matter for headless
    // re-simulation; zeroing the flag safely skips the crashing call.
    //
    // (We also hook FUN_1405B72B0 via MinHook below as a belt-and-suspenders
    //  guard — the flag can be re-enabled on worker threads.)

    // ==================================================================
    // Phase 1 — Frame timing + Manager state + Input (always needed)
    // ==================================================================
    // 1. Frame timing — SKIP sMain__move during re-simulation.
    //    sMain__move (0x140521df0) reads real QPC, computes frame deltas,
    //    runs Sleep()/busy-wait for frame pacing, and updates sMain timing
    //    fields (0x38, 0x48, 0x60, 0x68, 0x80, etc.).  Calling it during
    //    re-sim corrupts those fields (it samples real QPC across the re-sim
    //    window) and the first post-re-sim real frame computes a stale delta
    //    that confuses the frame-pacer — causing a permanent hang.
    //
    //    Instead we just set the tick count (sMain+0x48) to 1 so any game
    //    logic that checks it sees a valid advance.  The real sMain__move
    //    runs on the next natural frame boundary after re-sim ends.
    HookLog("Rollback_HeadlessTick: Phase1 step1 — Frame timing (skipping sMain__move)\n");
    *(uint32_t*)(sMain + 0x48) = 1;
    // 2. Window-focus/manager state
    HookLog("Rollback_HeadlessTick: Phase1 step2 — Window-focus/manager state\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_MANAGERS))(sMain);
    // 3. Input processing (CRITICAL — gathers pad / applies historical inputs)
    HookLog("Rollback_HeadlessTick: Phase1 step3 — Input processing\n");
    uint64_t syncPad = *(uint64_t*)(sMain + 0x401c0);
    if (syncPad)
        ((void(__fastcall*)(uint64_t))_addr(FN_PAD_PROCESS))(syncPad);
    // 4. Async loader update (vtable+0x30)
    HookLog("Rollback_HeadlessTick: Phase1 step4 — Async loader update\n");
    VTableCall(sMain, 0x40260, 0x30);

    // ==================================================================
    // [SKIP] vtable+0x48 draw calls on [0x401a8], [0x40198], [0x401d8]
    // [SKIP] FUN_1405bb710 on [0x401e0] — render debug/profiling call
    // [SKIP] sRender/sPrimitive render-setup block
    // [SKIP] vtable+0x48 on [0x40270], [0x40208]
    // [SKIP] FUN_140537080 (Present!) — would crash through kiero hooks
    // ==================================================================

    // ==================================================================
    // Phase 2 — Post-render game-logic updates (mirrors original order)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase2 begin — Post-render game-logic updates\n");
    // 5. sMvc3Area update (vtable+0x30)
    VTableCall(sMain, 0x40200, 0x30);

    // 6. sMvc3Manager singleton chain (getAction + afterAction)
    HookLog("Rollback_HeadlessTick: Phase2 step6 — sMvc3Manager singleton chain\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t action = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_MVC3MGR_GETACTION))(mgr);
            if (action)
                ((void(__fastcall*)(uint64_t))_addr(FN_AFTER_GETACTION))(action);
        }
    }

    // 7. Post-render vtable+0x30 updates (game-logic state)
    HookLog("Rollback_HeadlessTick: Phase2 step7 — Post-render vtable+0x30 updates\n");
    VTableCall(sMain, 0x40230, 0x30);  // sMvc3Vibration
    VTableCall(sMain, 0x401c0, 0x30);  // sMvc3SyncPad
    VTableCall(sMain, 0x40210, 0x30);  // sMouse
    VTableCall(sMain, 0x40228, 0x30);  // sId
    VTableCall(sMain, 0x40218, 0x30);  // sKeyboard
    VTableCall(sMain, 0x401c0, 0x50);  // sMvc3SyncPad vfn+0x50
    VTableCall(sMain, 0x401e8, 0x30);  // sMvc3Shader2
    VTableCall(sMain, 0x40240, 0x30);  // sAI

    // ---- Manager subsystems (vtable+0x30 updates) ----
    HookLog("Rollback_HeadlessTick: Phase2 — Manager subsystems\n");
    MgrSubsysCall(FN_GETGAMECONFIG, 0x30);   // sGameConfig
    MgrSubsysCall(FN_GETGAMEUI, 0x30);       // sGameUI

    // [SKIP] getShot->vtable+0x48 (draw)
    // [SKIP] getGameEffect->vtable+0x48 (draw)

    VTableCall(sMain, 0x401a0, 0x30);  // collision (sCollision__move)
    VTableCall(sMain, 0x401e0, 0x30);  // mpCollision second call

    MgrSubsysCall(FN_GETHITSOLVER, 0x30);    // sHitSolver

    VTableCall(sMain, 0x401a0, 0x68);  // physics vfn+0x68
    VTableCall(sMain, 0x401a0, 0x58);  // physics vfn+0x58

    // 8. Direct AI call (separate from vtable+0x30 above)
    HookLog("Rollback_HeadlessTick: Phase2 step8 — Direct AI call\n");
    DirectMemberCall(sMain, 0x40240, FN_CALL_AI);

    // 9. Remaining vtable+0x30 member updates
    HookLog("Rollback_HeadlessTick: Phase2 step9 — Remaining vtable+0x30 updates\n");
    VTableCall(sMain, 0x401d8, 0x30);
    VTableCall(sMain, 0x40208, 0x30);
    VTableCall(sMain, 0x401b0, 0x30);
    VTableCall(sMain, 0x401f0, 0x30);
    VTableCall(sMain, 0x40248, 0x30);
    VTableCall(sMain, 0x40258, 0x30);

    // ==================================================================
    // Phase 3 — Manager subsystem update loop (all vtable+0x30)
    //
    // NOTE: The function at 0x140259270 was previously identified as a
    // manager subsystem getter, but Ghidra analysis revealed it is actually
    // Concurrency::details::VirtualProcessor::GetOwningRoot — a ConcRT
    // thread-pool internal. Calling it with vtable+0x30 corrupts the
    // dispatch table across all 16 worker threads, causing crashes at
    // address 0x100000000.  It is intentionally excluded below.
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase3 begin — Manager subsystem update loop\n");
    // getInstance -> vtable+0x30 (manager itself)
    HookLog("Rollback_HeadlessTick: Phase3 — sMvc3Manager vtable+0x30\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t vt = *(uint64_t*)mgr;
            if (vt) {
                auto fn = *(void(__fastcall**)(uint64_t))(vt + 0x30);
                if (fn) fn(mgr);
            }
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Character/Stage/Shot/GameEffect vtable+0x30\n");
    MgrSubsysCall(FN_GETCHARACTER, 0x30);    // sCharacter
    MgrSubsysCall(FN_GETSTAGE, 0x30);        // sStage
    MgrSubsysCall(FN_GETSHOT, 0x30);         // sShot (update, NOT draw)
    MgrSubsysCall(FN_GETGAMEEFFECT, 0x30);   // sGameEffect (update, NOT draw)
    // getHitSolver -> j_sHitSolver__resetActiveNodes (direct, not vtable)
    HookLog("Rollback_HeadlessTick: Phase3 — Hit solver reset\n");
    {
        uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (inst) {
            uint64_t hs = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_GETHITSOLVER))(inst);
            if (hs)
                ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(hs);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Replay/Battle/Network/License vtable+0x30\n");
    MgrSubsysCall(FN_GETREPLAY, 0x30);       // sReplay
    // FN_MGR_GETSOMETHING (0x140259270) intentionally excluded — see note above.
    MgrSubsysCall(FN_GETBATTLESET, 0x30);    // sBattleSetting
    MgrSubsysCall(FN_GETMVC3SET, 0x30);      // sMvc3Setting
    MgrSubsysCall(FN_GETNETWORK, 0x30);      // sNetwork
    MgrSubsysCall(FN_GETLICENSE, 0x30);      // sLicense

    HookLog("Rollback_HeadlessTick: Phase3 — Remaining member vtable+0x30\n");
    VTableCall(sMain, 0x401b8, 0x30);
    VTableCall(sMain, 0x401c8, 0x30);
    VTableCall(sMain, 0x401d0, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 — Ranking/NetRoom/EarthAtk vtable+0x30\n");
    MgrSubsysCall(FN_GETRANKING, 0x30);      // sRanking
    MgrSubsysCall(FN_GETNETROOM, 0x30);      // sNetworkRoom
    MgrSubsysCall(FN_GETEARTHATK, 0x30);     // sEarthAttack

    // ==================================================================
    // Phase 4 — Tail + finalize
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase4 begin — Tail + finalize\n");
    VTableCall(sMain, 0x40198, 0x30);
    VTableCall(sMain, 0x40270, 0x30);

    // [SKIP] vtable+0x48 on [0x401e0] (draw)

    // Tail function
    HookLog("Rollback_HeadlessTick: Phase4 — sMain tail\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_SMAIN_TAIL))(sMain);

    // [SKIP] sRender game-logic state update (vtable+0x30) — handled inside
    //       FN_SMAIN_MOVE, and the internal allocator isn't snapshot-safe.
    // [SKIP] Direct sRender method — rendering is meaningless during headless re-sim.
    //[SKIP] vtable+0x50 on [0x40208] (render)
    //[SKIP] vtable+0x50 on [0x40270] (render)

    HookLog("Rollback_HeadlessTick: Phase4 — Final vtable+0x30\n");
    VTableCall(sMain, 0x401f8, 0x30);

    // ==================================================================
    // Phase 5 — Pad methods + exit check (SKIP exit during re-sim)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase5 begin — Pad methods\n");
    // sPad::method_3D00(0xa4) and sPad::method_3D00(0xa5) — input state flush
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa4, 1);
    }
    HookLog("Rollback_HeadlessTick: Phase5 — Pad method 0xa5\n");
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa5, 1);
    }
    // [SKIP] The ALT+F4 exit check (sMain__exit call) — we NEVER want to
    // exit during re-simulation. The original checks if F4+Enter was pressed
    // and calls sMain__exit. Skip this entirely.

    HookLog("Rollback_HeadlessTick: end\n");
}

// Forward declarations
static void Rollback_ClearStalePointers();
static void Rollback_ClearGameEffectSlots();

// ===========================================================================
// Public API — Save / Restore / ReSimulate
// ===========================================================================

void Rollback_SaveSnapshot(int32_t frame)
{
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += regions[i].size;
    }

    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (slot.capacity < total) {
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[total];
        if (!slot.data) {
            HookLog("Rollback: failed to allocate %zu-byte snapshot for frame %d\n",
                    total, frame);
            slot.capacity    = 0;
            slot.size        = 0;
            slot.regionCount = 0;
            return;
        }
        slot.capacity = total;
    }

    // Copy data AND save per-region metadata for safe restore
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        slot.regions[i].ptr  = regions[i].ptr;
        slot.regions[i].size = regions[i].size;
        std::memcpy(slot.data + offset, regions[i].ptr, regions[i].size);
        offset += regions[i].size;
    }
    slot.size = offset;

    // [EXPERIMENTAL] QPC capture — see mvc3/rollback W3-C2
    // LARGE_INTEGER qpc_cap;
    // if (QueryPerformanceCounter(&qpc_cap)) {
    //     g_qpc_history[slotIdx] = qpc_cap.QuadPart;
    // }
    
    // Save the local player's input for this frame in the snapshot slot.
    // At this point g_oldInputs[MAX_HISTORY - g_delayFrames] holds the
    // delayed input that the imminent game tick will use.
    slot.localInput = Controller_GetCurrentFrameInput();

    // Save the best-known remote input for this frame.
    slot.remoteInput = Controller_GetReceivedRemoteInput(frame);
    
    //HookLog("Rollback: saved frame %d with %d regions, total size %zu bytes\n", frame, count, offset);
}

void Rollback_UpdateRemoteInput(int32_t frame, uint32_t input)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];
    if (slot.data && slot.size > 0 && slot.regionCount > 0 && slot.frame == frame)
    {
        slot.remoteInput = input;
    }
}

extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        slot.localInput = localInput;
        slot.remoteInput = remoteInput;
    }
}

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    const SnapshotSlot& slot = g_slots[slotIdx];
    
    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        if (outLocal) *outLocal = slot.localInput;
        if (outRemote) *outRemote = slot.remoteInput;
        return true;
    }
    return false;
}

void Rollback_RestoreSnapshot(int32_t frame)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Verify the slot contains data for exactly this frame (ring-buffer safety)
    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot available for frame %d (slot has frame %d)\n",
                frame, slot.frame);
        return;
    }

    // Restore using SAVED region metadata (addresses + sizes), not re-resolved.
    // This ensures dynamic objects (projectiles, effects) map to the exact
    // same memory addresses that were captured, even if the game rebuilt its
    // internal lists between save and restore.
    size_t offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        if (copySize > 0) {
            std::memcpy(slot.regions[i].ptr, slot.data + offset, copySize);
        }
        offset += slot.regions[i].size;
    }

    // Log ALL heap regions with their sizes
    offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t raddr = (uint64_t)slot.regions[i].ptr;
        size_t rsize = slot.regions[i].size;
     //   HookLog("DIAG Restore region[%d]: addr=0x%llX size=%zu\n", i, raddr, rsize);
        uint64_t maybeVt = (raddr >= 0x100000 && raddr < 0x140000000) ? *(uint64_t*)raddr : 0;
        if (maybeVt >= 0x140000000 && rsize >= 0x1000) {
            uint32_t* animAfter = (uint32_t*)(raddr + 0x1300);
     //       HookLog("DIAG Post-restore char@0x%llX region[%d]: anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
     //               raddr, i,
      //              animAfter[0], animAfter[1], animAfter[2], animAfter[3],
      //              animAfter[4], animAfter[5], animAfter[6], animAfter[7],
          //          animAfter[8], animAfter[9], animAfter[10], animAfter[11],
       //             animAfter[12], animAfter[13], animAfter[14], animAfter[15]);
        }
        offset += rsize;
    }

    // No sentinel fix needed — uShot projectile objects were saved individually
    // (see BuildRegions section 2b) and restored at their original addresses, so
    // the sentinel→uShot linked lists remain self-consistent after the memcpy.

    // Clear stale character slot pointers so MtObject__replaceSlot_6F30
    // doesn't try to call vtable[0x78] (release) on objects that were
    // freed between save and restore.
    Rollback_ClearStalePointers();

    // Zero sGameEffect slot tables so stale effect object references
    // don't cause crashes or visual corruption when the game tries to
    // update effect animation positions.  Fresh effects will be allocated
    // into clean slots on the next game tick.
    //Rollback_ClearGameEffectSlots();

    HookLog("Rollback: restored frame %d with %d regions, total size %zu bytes\n", frame, slot.regionCount, offset);
}

// ===========================================================================
// Diag dumpers — capture full snapshot state + character text for resim debugging
// ===========================================================================

static uint64_t fnv1a_64(const void* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static void Rollback_DiagInit() {
    if (g_diagDir[0]) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char rel[80];
    
    // Append the local netplay role as a suffix to the directory name
    const char* sideSuffix = Netplay_IsActive() ? (Netplay_IsHost() ? "_host" : "_client") : "_offline";
    snprintf(rel, sizeof(rel), "rdiag_%04d%02d%02d_%02d%02d%02d%s",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, sideSuffix);
             
    ResolveDllPath(g_diagDir, sizeof(g_diagDir), rel);
    CreateDirectoryA(g_diagDir, NULL);
    HookLog("Rollback: diag dir: %s\n", g_diagDir);
    char ndjsonPath[260];
    snprintf(ndjsonPath, sizeof(ndjsonPath), "%s/rdiag.ndjson", g_diagDir);
    g_diagNdjsonFile = fopen(ndjsonPath, "a");
    if (g_diagNdjsonFile)
        g_diagNdjsonFlush = GetTickCount();
}

// Binary: one file per session with [header + N entries].
// Header:  magic(4)="RBSD"  ver(u32)  sessionId(u32)  entryCount(u32)
// Entry:   frame(i32) side(u8) regionCount(u32) dataSize(u32)
//          regions[ regionSize(u32) regionHash(u64) regionData(bytes) ]
// v1 → v2: per-region FNV-1a hash added after size for region-level divergence detection.
static void Rollback_DumpBinary(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.bin", g_diagDir, session);

    bool isNew = false;
    FILE* f = fopen(path, "rb");
    if (!f) {
        f = fopen(path, "wb");
        if (!f) return;
        isNew = true;
        uint32_t magic = 0x44534252;
        uint32_t ver = 2;
        uint32_t sid = (uint32_t)session;
        uint32_t ec = 0;
        fwrite(&magic, 4, 1, f);
        fwrite(&ver, 4, 1, f);
        fwrite(&sid, 4, 1, f);
        fwrite(&ec, 4, 1, f);
    } else {
        fclose(f);
        f = fopen(path, "ab");
        if (!f) return;
    }

    g_diagWriteCount++;

    int32_t binFrame = side == 0 ? frame : frame + 1;
    fwrite(&binFrame, sizeof(binFrame), 1, f);
    fwrite(&side, 1, 1, f);
    uint32_t cc = (uint32_t)count;
    fwrite(&cc, sizeof(cc), 1, f);

    uint32_t totalSize = 0;
    for (int i = 0; i < count; i++)
        totalSize += (uint32_t)regions[i].size;
    fwrite(&totalSize, sizeof(totalSize), 1, f);

    for (int i = 0; i < count; i++) {
        uint32_t sz = (uint32_t)regions[i].size;
        uint64_t hash = fnv1a_64(regions[i].ptr, regions[i].size);
        fwrite(&sz, sizeof(sz), 1, f);
        fwrite(&hash, sizeof(hash), 1, f);
        fwrite(regions[i].ptr, 1, sz, f);
    }

    fclose(f);

    if (isNew) {
        uint32_t ec = (uint32_t)g_diagWriteCount;
        f = fopen(path, "r+b");
        if (f) {
            fseek(f, 12, SEEK_SET);
            fwrite(&ec, 4, 1, f);
            fclose(f);
        }
    }
}

// Text: character state per frame — appended to session text log
// RESIM dump labeling convention:
//   side=0 (LIVE): frame = startFrame of the resim window.  The actual captured
//     state is the current live frame (endFrame), NOT startFrame.  The label uses
//     startFrame because call-site changes broke the resim — the user must
//     mentally map: LIVE frame=N → state at the CURRENT live frame, not frame N.
//   side=1 (RESIM): frame = the frame that was just ticked.  The captured state
//     is at frame+1 (the start of the next frame).  We display frame+1 here so
//     the output frame number matches the actual state.
static void Rollback_DumpCharText(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);

    FILE* f = fopen(path, "a");
    if (!f) return;

    int32_t displayFrame = side == 0 ? frame : frame + 1;
    fprintf(f, "=== %s frame=%d ===\n", side == 0 ? "LIVE" : "RESIM", displayFrame);
    if (side == 0) {
        fprintf(f, "# NOTE: LIVE frame=%d labels the resim window start, actual state is at current live frame\n", frame);
    }

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) { fprintf(f, "  (no sCharacter)\n\n"); fclose(f); return; }

    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        int ci = 0;
        while (node) {
            uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (chr) {
                CharDiagSnapshot snap;
                CaptureCharSnapshot(chr, ti, ci, snap);

                uint64_t fcnt = *(uint64_t*)(snap.cchr + 0x18);
                uint32_t mHeld = *(uint32_t*)(snap.cchr + 0x14);
                uint32_t mRemap = *(uint32_t*)(snap.cchr + 0x560);
                uint32_t mMask  = *(uint32_t*)(snap.cchr + 0x568);
                uint32_t mProc  = *(uint32_t*)(snap.cchr + 0x56C);
                uint32_t mOld   = *(uint32_t*)(snap.cchr + 0x10);
                uint32_t h590   = *(uint32_t*)(snap.cchr + 0x590);
                uint32_t h594   = *(uint32_t*)(snap.cchr + 0x594);
                uint64_t heldR0 = *(uint64_t*)(snap.cchr + 0x28);
                uint32_t relHead= *(uint32_t*)(snap.cchr + 0x0A8);
                uint64_t relR0  = *(uint64_t*)(snap.cchr + 0x0AC);
                uint64_t prsR0  = *(uint64_t*)(snap.cchr + 0x130);
                uint32_t dir0   = *(uint32_t*)(snap.cchr + 0x1B0);
                uint32_t dir4   = *(uint32_t*)(snap.cchr + 0x1B4);
                uint32_t dirSlot= (uint32_t)(fcnt & 0x3F);
                uint64_t dirRing0  = *(uint64_t*)(snap.cchr + 0x360);
                uint64_t dirRingSlot=*(uint64_t*)(snap.cchr + 0x360 + dirSlot * 8);

                fprintf(f, "  Team%d Char%d @ 0x%llX\n", ti, ci, (uint64_t)chr);
                fprintf(f, "    pos=(%+.4f,%+.4f,%+.4f) act=0x%08X fl=0x%08X\n",
                        snap.posX, snap.posY, snap.posZ, snap.actionId, snap.flags14F0);
                fprintf(f, "    cci: mOld=0x%08X mHeld=0x%08X remap=0x%08X mask=0x%08X proc=0x%08X\n",
                        mOld, mHeld, mRemap, mMask, mProc);
                fprintf(f, "    cci: cntr=%llu h590=0x%08X h594=0x%08X\n", fcnt, h590, h594);
                fprintf(f, "    cci: heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX\n",
                        heldR0, relHead, relR0, prsR0);
                fprintf(f, "    cci: dir0=0x%08X dir4=0x%08X dirRing[0]=0x%016llX [%u]=0x%016llX\n",
                        dir0, dir4, dirRing0, dirSlot, dirRingSlot);
                fprintf(f, "    rng: 0x%08X 0x%08X 0x%08X 0x%08X\n",
                        snap.xorShift[0], snap.xorShift[1], snap.xorShift[2], snap.xorShift[3]);
                ci++;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    fprintf(f, "\n");
    fclose(f);
}

// Manifest: per-region FNV-1a hash summary appended to session text log.
// Quick visual scan shows which regions diverge between LIVE and RESIM.
static void Rollback_DumpManifest(int session, int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    char path[260];
    snprintf(path, sizeof(path), "%s/session_%d.txt", g_diagDir, session);

    FILE* f = fopen(path, "a");
    if (!f) return;

    int32_t mf = side == 0 ? frame : frame + 1;
    fprintf(f, "=== MANIFEST %s frame=%d regions=%d ===\n",
            side == 0 ? "LIVE" : "RESIM", mf, count);
    for (int i = 0; i < count; i++) {
        uint64_t hash = fnv1a_64(regions[i].ptr, regions[i].size);
        fprintf(f, "  [%3d] addr=0x%016llX size=%-8u hash=0x%016llX\n",
                i, (uint64_t)regions[i].ptr, (uint32_t)regions[i].size, hash);
    }
    fprintf(f, "\n");
    fclose(f);
}

// NDJSON: one JSON line per dump frame for script/AI consumption.
// All entries go into a single rdiag.ndjson file (no separate sessions).
// side=0 → "ground_truth" (pre-resim state), side=1 → "resim" (post-resim state)
// Schema:  v, ts, frame, side, regions[{idx,addr,size,hash}], chars[{...}]
static void Rollback_DumpNDJSON(int32_t frame, uint8_t side) {
    if (!g_diagDir[0]) return;

    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    if (!g_diagNdjsonFile) return;

    // Build ISO timestamp
    char tsBuf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    int32_t nf = side == 0 ? frame : frame + 1;
    fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"%s\",\"regions\":[",
            tsBuf, nf, side == 0 ? "ground_truth" : "resim");

    for (int i = 0; i < count; i++) {
        uint64_t rh = fnv1a_64(regions[i].ptr, regions[i].size);
        if (i > 0) fprintf(g_diagNdjsonFile, ",");
        fprintf(g_diagNdjsonFile, "{\"idx\":%d,\"addr\":\"0x%llX\",\"size\":%llu,\"hash\":\"0x%016llX\"}",
                i, (uint64_t)regions[i].ptr, (uint64_t)regions[i].size, rh);
    }

    fprintf(g_diagNdjsonFile, "],\"chars\":[");

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    bool firstChar = true;
    if (sCharacter) {
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
            int ci = 0;
            while (node) {
                uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (chr) {
                    CharDiagSnapshot snap;
                    CaptureCharSnapshot(chr, ti, ci, snap);
                    uint64_t fcnt = *(uint64_t*)(snap.cchr + 0x18);
                    uint32_t mHeld = *(uint32_t*)(snap.cchr + 0x14);

                    if (!firstChar) fprintf(g_diagNdjsonFile, ",");
                    firstChar = false;
                    fprintf(g_diagNdjsonFile, "{\"team\":%d,\"slot\":%d,"
                            "\"pos\":[%.4f,%.4f,%.4f],"
                            "\"act\":\"0x%08X\",\"fl\":\"0x%08X\","
                            "\"mHeld\":\"0x%08X\",\"fcnt\":%llu,"
                            "\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                            ti, ci,
                            snap.posX, snap.posY, snap.posZ,
                            snap.actionId, snap.flags14F0,
                            mHeld, fcnt,
                            snap.xorShift[0], snap.xorShift[1],
                            snap.xorShift[2], snap.xorShift[3]);
                    ci++;
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    fprintf(g_diagNdjsonFile, "]}\n");
    DWORD now = GetTickCount();
    if (now - g_diagNdjsonFlush >= 1000) {
        fflush(g_diagNdjsonFile);
        g_diagNdjsonFlush = now;
    }
}

void Rollback_QueuePendingResimDump(int32_t frame)
{
    if (!Netplay_IsActive())
    {
        Rollback_DumpNDJSON(frame, 1);
        return;
    }

    const FrameDiagSnapshot* snap = Rollback_GetResimSnapshot(frame);
    if (!snap) return;

    const CharDiagSnapshot* p1Post = nullptr;
    const CharDiagSnapshot* p2Post = nullptr;
    if (!Rollback_FindP1P2Post(*snap, &p1Post, &p2Post)) return;

    PendingResimDump dump;
    dump.frame = frame;
    Rollback_FillResimCharState(dump.p1Post, *p1Post);
    Rollback_FillResimCharState(dump.p2Post, *p2Post);

    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % PENDING_RESIM_DUMP_RING;
    g_pendingResimDumps[idx] = dump;
    g_pendingResimDumpValid[idx] = true;
}

extern "C" void Rollback_SendGroundTruth(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;

    GroundTruthPacket gtPkt = {};
    gtPkt.type = PACKET_GROUND_TRUTH;
    gtPkt.startFrame = startFrame;
    gtPkt.endFrame = endFrame;

    int32_t frameCount = 0;
    for (int32_t f = startFrame; f < endFrame && frameCount < MAX_RESIM_FRAMES; f++)
    {
        uint32_t liveIdx = (uint32_t)f % 256;
        auto& p1Live = g_liveRef[0][liveIdx];
        auto& p2Live = g_liveRef[1][liveIdx];

        auto& rf = gtPkt.frames[frameCount];
        rf.frame = f;

        auto fillState = [](ResimCharState& dst, const LiveFrameRecord& src) {
            dst.actionId = src.actionId;
            dst.posX     = src.posX;
            dst.posY     = src.posY;
            dst.posZ     = src.posZ;
            dst.flags    = src.flags14F0;
            dst.mHeld    = src.mHeld;
            dst.frameCnt = src.frameCnt;
            for (int ri = 0; ri < 4; ri++)
                dst.rng[ri] = src.rng[ri];
        };

        fillState(rf.p1Live, p1Live);
        fillState(rf.p2Live, p2Live);
        frameCount++;
    }
    gtPkt.frameCount = (uint32_t)frameCount;
    Netplay_SendGroundTruth(gtPkt);
}

extern "C" void Rollback_ProcessInboundGroundTruth()
{
    GroundTruthPacket gtPkt;
    while (Netplay_GetInboundGroundTruth(gtPkt))
    {
        Rollback_DiagInit();
        if (!g_diagNdjsonFile) continue;

        char tsBuf[32] = {};
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        for (uint32_t fi = 0; fi < gtPkt.frameCount; fi++)
        {
            auto& rf = gtPkt.frames[fi];

            uint32_t pidx = (uint32_t)(rf.frame >= 0 ? rf.frame : 0) % PENDING_RESIM_DUMP_RING;
            bool found = g_pendingResimDumpValid[pidx] && g_pendingResimDumps[pidx].frame == rf.frame;
            PendingResimDump resimDump = found ? g_pendingResimDumps[pidx] : PendingResimDump{};
            if (found) g_pendingResimDumpValid[pidx] = false;

            if (!found) continue;

            // Ground truth (received remote data)
            fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"ground_truth\","
                       "\"inputs\":{\"local\":\"-\",\"usedRemote\":\"-\",\"recvRemote\":\"-\"},"
                       "\"chars\":[",
                    tsBuf, rf.frame);
            fprintf(g_diagNdjsonFile, "{\"team\":0,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    rf.p1Live.posX, rf.p1Live.posY, rf.p1Live.posZ, rf.p1Live.actionId, rf.p1Live.flags, rf.p1Live.mHeld, (unsigned long long)rf.p1Live.frameCnt,
                    rf.p1Live.rng[0], rf.p1Live.rng[1], rf.p1Live.rng[2], rf.p1Live.rng[3]);
            fprintf(g_diagNdjsonFile, ",{\"team\":1,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    rf.p2Live.posX, rf.p2Live.posY, rf.p2Live.posZ, rf.p2Live.actionId, rf.p2Live.flags, rf.p2Live.mHeld, (unsigned long long)rf.p2Live.frameCnt,
                    rf.p2Live.rng[0], rf.p2Live.rng[1], rf.p2Live.rng[2], rf.p2Live.rng[3]);
            fprintf(g_diagNdjsonFile, "]}\n");

            // Resim (local corrected data)
            fprintf(g_diagNdjsonFile, "{\"v\":1,\"ts\":\"%s\",\"frame\":%d,\"side\":\"resim\","
                       "\"inputs\":{\"local\":\"-\",\"usedRemote\":\"-\",\"recvRemote\":\"-\"},"
                       "\"chars\":[",
                    tsBuf, rf.frame);
            fprintf(g_diagNdjsonFile, "{\"team\":0,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    resimDump.p1Post.posX, resimDump.p1Post.posY, resimDump.p1Post.posZ, resimDump.p1Post.actionId, resimDump.p1Post.flags, resimDump.p1Post.mHeld, (unsigned long long)resimDump.p1Post.frameCnt,
                    resimDump.p1Post.rng[0], resimDump.p1Post.rng[1], resimDump.p1Post.rng[2], resimDump.p1Post.rng[3]);
            fprintf(g_diagNdjsonFile, ",{\"team\":1,\"slot\":0,\"pos\":[%.4f,%.4f,%.4f],\"act\":\"0x%08X\",\"fl\":\"0x%08X\",\"mHeld\":\"0x%08X\",\"fcnt\":%llu,\"rng\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                    resimDump.p2Post.posX, resimDump.p2Post.posY, resimDump.p2Post.posZ, resimDump.p2Post.actionId, resimDump.p2Post.flags, resimDump.p2Post.mHeld, (unsigned long long)resimDump.p2Post.frameCnt,
                    resimDump.p2Post.rng[0], resimDump.p2Post.rng[1], resimDump.p2Post.rng[2], resimDump.p2Post.rng[3]);
            fprintf(g_diagNdjsonFile, "]}\n");
        }
        DWORD now = GetTickCount();
        if (now - g_diagNdjsonFlush >= 1000) {
            fflush(g_diagNdjsonFile);
            g_diagNdjsonFlush = now;
        }
    }
}

void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;

    // Re-entry guard: if F7 is still held/read during re-sim, the game's
    // input processing will try to start another ReSimulate from within
    // oMvc3Main_move.  Just return — we're already in one.
    if (g_isResimulating) {
        HookLog("Rollback: ReSimulate re-entered — ignoring (already re-simulating)\n");
        return;
    }

    if (!g_sMvc3Main) {
        HookLog("Rollback: cannot re-simulate — sMvc3Main pointer not captured\n");
        return;
    }

    g_isResimulating = true;

    HookLog("Rollback: [%s] re-simulating frames %d \u2192 %d\n",
            Netplay_IsHost() ? "host" : "client", startFrame, endFrame);

    //Are these really necessary? Might as well leave them in

    Rollback_PatchSleep(true); // NOP Sleep calls so frames run at full speed
    Rollback_PatchPresent(true); // NOP Present inside post-render sync
    Rollback_PatchSwapchain(true); // NOP swapchain vtable+0x50 calls
    Rollback_PatchDrawCalls(true); // NOP remaining render/draw calls

    // Save the live input history before rewinding.  The re-sim loop calls
    // SilentFrameStart/End which shifts g_oldInputs with zeros (re-sim
    // bypasses the accumulation).  Restoring after the loop prevents the
    // outbound packet from carrying a corrupted timeline.
    Controller_SaveInputHistory();

    // Diagnostic dump: capture ground truth BEFORE rewinding.
    // Gives the live state of all regions + character state to compare
    // against what the re-simulation produces.
    // When g_f7DiagSession is set (F7 mode), skip the ground truth dump —
    // the F7 state machine already captured per-frame live dumps during
    // the recording phase using that session.  We just reuse the session
    // ID so resim dumps land alongside the live dumps.
    {
        int diagSid;
        if (g_f7DiagSession >= 0) {
            diagSid = g_f7DiagSession;
            HookLog("Rollback: diag session %d (F7, reuse) for frames %d\u2192%d\n",
                    diagSid, startFrame, endFrame);
        } else {
            Rollback_DiagInit();
            g_diagSessionId++;
            diagSid = g_diagSessionId;
            uint64_t sr = *(uint64_t*)_addr(PTR_SREPLAY);
            int32_t liveFrame = sr ? *(int32_t*)(sr + 0x54) : startFrame;
            g_diagWriteCount = 0;
            HookLog("Rollback: diag session %d for frames %d\u2192%d (live=%d)\n",
                    diagSid, startFrame, endFrame, liveFrame);
            // session_* files disabled — use rdiag.ndjson instead
            //Rollback_DumpBinary(diagSid, liveFrame, 0);
            //Rollback_DumpCharText(diagSid, liveFrame, 0);
            //Rollback_DumpManifest(diagSid, liveFrame, 0);
        }
        // Sync the global so the resim loop writes to the same session
        g_diagSessionId = diagSid;
    }

    // 1. Rewind to the starting frame boundary
    Rollback_RestoreSnapshot(startFrame);

    // Diagnostic: log whether we actually have received input for every frame
    // in the resim range before we start stepping.
    Controller_LogReceivedInputAvailability(startFrame, endFrame);

    // [EXPERIMENTAL] mJobThreadNumInit=1 — see mvc3/rollback R3-SIMLOOP
    // uint64_t sMain_job = *(uint64_t*)_addr(PTR_SMAIN);
    // uint32_t saved_job_thread_num_init = 0;
    // uint32_t* pJobThreadNumInit = nullptr;
    // if (sMain_job) {
    //     pJobThreadNumInit = (uint32_t*)(sMain_job + 0xA4); // mJobThreadNumInit at offset 164
    //     saved_job_thread_num_init = *pJobThreadNumInit;
    //     *pJobThreadNumInit = 1;
    // }

    // 2. Step forward (no snapshot saves — the re-simmed frames are now
    //    correct, and the next normal-frame save at curFrame covers us).
    //    sMain__move is hooked (skip frame-pacing) and the render function
    //    is hooked (skip Present), so the full oMvc3Main_move game tick
    //    runs at speed without timing waits or actual rendering.
    //    The hkMvc3Main_move hook returns early during re-sim so the
    //    game's own frame loop doesn't double-execute.
    //    Controller_SilentFrameStart/End maintains the local input shift
    //    buffer without draining packets or sending outbound data.  Local
    //    and remote input for each re-simmed frame are injected from the
    //    snapshot slot via Controller_InjectLocalInput and
    //    Controller_InjectRemoteInput before the game tick.
    for (int32_t f = startFrame; f < endFrame; ++f) {
        g_resimulateCurrentFrame = f;
        // [EXPERIMENTAL] g_virtual_qpc = g_qpc_history[f % ROLLBACK_WINDOW];
        // Re-resolve remote input so re-sim uses the latest received data,
        // not the stale prediction that triggered the conflict.
        if (Netplay_IsActive()) {
            Controller_ResolveRemoteInput(f);
        }
        Controller_SilentFrameStart();
        // Inject the local and remote inputs saved in this frame's
        // snapshot slot.  hkprocessFrameInput reads g_recordedGameInput
        // for the local team and g_usedRemoteInput for the remote team
        // during re-sim; pre-populating from the snapshot makes the hook
        // replay the correct historical inputs without relying on the
        // shift-buffer save/restore that was global-static-buffer-
        // vulnerable.
        {
            int si = f % ROLLBACK_WINDOW;
            if (si < 0) si += ROLLBACK_WINDOW;
            Controller_InjectLocalInput(f, g_slots[si].localInput);
            if (!Netplay_IsActive()) {
                Controller_InjectRemoteInput(f, g_slots[si].remoteInput);
            }
        }
        // Capture PRE-state before the tick
        Rollback_CaptureFrameDiagSnapshot(f, /*isResim=*/true, /*postOnly=*/false);
        oMvc3Main_move(g_sMvc3Main);
        Controller_SilentFrameEnd();
        Controller_LogReSimFrame(f);

        // Discard the unconfirmed pending live JSON and write the corrected state
        if (Netplay_IsActive()) {
            Rollback_DiscardPendingNetplayLive(f);
            std::string json = Rollback_GenerateNetplayLiveJSON(f, true);
            Rollback_WriteNetplayLiveJSON(json);
        }

        // Capture POST-state after the tick (preserves the pre-state captured above)
        Rollback_CaptureFrameDiagSnapshot(f, /*isResim=*/true, /*postOnly=*/true);

        // Diagnostic dump: capture re-simmed state for this frame
        // session_* files disabled — use rdiag.ndjson instead
        //Rollback_DumpBinary(g_diagSessionId, f, 1);
        //Rollback_DumpCharText(g_diagSessionId, f, 1);
        //Rollback_DumpManifest(g_diagSessionId, f, 1);
        Rollback_QueuePendingResimDump(f);
    }

    // Restore the local input history so the next outbound packet carries
    // the original timeline, preventing the remote side from seeing spurious
    // conflicts on already-reconciled frames.
    Controller_RestoreInputHistory();

    // [EXPERIMENTAL] mJobThreadNumInit restore — see mvc3/rollback R3-SIMLOOP
    // if (pJobThreadNumInit) {
    //     *pJobThreadNumInit = saved_job_thread_num_init;
    // }

    // 3. Resynchronize performance counter state inside sMain to match
    //    current system time, preventing the game loop from seeing a
    //    negative or excessive frame-time delta that would cause a freeze.
    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    if (sMain) {
        //Same with this, unsure if this is really needed, might cause lag if disabled
        Rollback_ResyncMainTiming(sMain);
    }

    g_isResimulating = false;
    Rollback_PatchSleep(false); // restore Sleep calls
    Rollback_PatchPresent(false); // restore Present call
    Rollback_PatchSwapchain(false); // restore swapchain calls
    Rollback_PatchDrawCalls(false); // restore render/draw calls
    HookLog("Rollback: re-simulation complete (%d frames)\n",
            endFrame - startFrame);
}

void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame)
{
    g_rollbackReqStart = startFrame;
    g_rollbackReqEnd   = endFrame;
}

// ===========================================================================
// Post-Restore Cleanup — clear stale pointer/slot tables after snapshot restore
//
// The flat-memcpy restore of character objects brings back slot arrays at
// offset +0x6748 that reference heap objects (effects, projectiles, etc.)
// which were NOT saved in the snapshot.  Any subsequent call to
// MtObject__replaceSlot_6F30 on these slots will try to call vtable[0x78]
// (release) on a stale/corrupted object → page fault at NULL.
//
// We walk the team character lists (same traversal as BuildRegions) and zero
// the slot pointers so the game simply writes new references without first
// trying to release a dangling pointer.
//
// Slot array:  64 entries × 8 bytes = 512 bytes at character + 0x6748.
// Confirmed indices in use: 1, 14, 15 (from crash + decompiled callers).
// ===========================================================================
static void Rollback_ClearStalePointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    static constexpr int SLOT_CLEAR_COUNT = 64;
    static constexpr uintptr_t SLOT_ARRAY_OFFSET = 0x6748;

    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

        while (node) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character) {
                // Zero the slot array so stale pointers are never followed
                memset((void*)(character + SLOT_ARRAY_OFFSET), 0,
                       SLOT_CLEAR_COUNT * sizeof(uint64_t));
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    HookLog("Rollback: cleared character slot arrays after restore\n");
}

// ===========================================================================
// Post-Restore Cleanup — clear sGameEffect slot tables after snapshot restore
//
// The sGameEffect manager maintains two slot tables with MtObject* weak
// references to active effect objects:
//   slotTable10 — 10 entries × 8 bytes at +0x40  (80 bytes)
//   slotTable64 — 64 entries × 16 bytes at +0x1948 (1024 bytes)
//
// These entries point to heap-allocated effect nodes that were NOT saved in
// the snapshot (sUnit effect category traversal is skipped due to pool
// allocator corruption).  After restore any stale pointers in these tables
// would be dereferenced by the game's effect update path, causing crashes
// or visual corruption (broken animation positions).  Zeroing them lets the
// game allocate fresh effect slots on the next tick.
// ===========================================================================
static void Rollback_ClearGameEffectSlots()
{
    uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    if (!sGameEffect) return;

    static constexpr size_t SLOT_TABLE10_OFFSET = 0x40;
    static constexpr size_t SLOT_TABLE10_SIZE   = 80;   // 10 × 8 bytes
    static constexpr size_t SLOT_TABLE64_OFFSET = 0x1948;
    static constexpr size_t SLOT_TABLE64_SIZE   = 1024; // 64 × 16 bytes

    memset((void*)(sGameEffect + SLOT_TABLE10_OFFSET), 0, SLOT_TABLE10_SIZE);
    memset((void*)(sGameEffect + SLOT_TABLE64_OFFSET), 0, SLOT_TABLE64_SIZE);

    HookLog("Rollback: cleared sGameEffect slot tables after restore\n");
}

void Rollback_FreeSnapshots()
{
    // Make sure patched calls are restored before tearing down
    if (g_sleepPatched)
        Rollback_PatchSleep(false);
    if (g_presentPatched)
        Rollback_PatchPresent(false);
    if (g_swapchainPatched)
        Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false); // restore draw calls (no-op if unpatched)

    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].regionCount = 0;
        g_slots[i].frame       = -1;
    }
    g_sMvc3Main = 0;
}

// ===========================================================================
// Hook Setup — intercept sMvc3Main__move
//
// We hook the top-level frame update function (0x1402594A0) so we can:
//   1. Capture the sMvc3Main instance pointer for re-simulation calls
//   2. (Future) Gate execution during re-simulation or inject pre-frame logic
// ===========================================================================

static void __fastcall hkMvc3Main_move(uint64_t param_1)
{
    // Log EVERY call so we can tell if the game loop keeps running after re-sim
    static int32_t frameCounter = 0;
    frameCounter++;
   // HookLog("hkMvc3Main_move: frame=%d g_isResimulating=%d\n",
   //         frameCounter, (int)g_isResimulating);

    // Capture the main manager pointer on first call
    if (!g_sMvc3Main) {
        g_sMvc3Main = param_1;
        HookLog("Rollback: captured sMvc3Main @ 0x%llX\n", param_1);
    }

    // Pending snapshot-restore request from FrameStepper_Process (F6 key).
    // Execute at frame boundary (BEFORE oMvc3Main_move) to avoid mid-frame
    // memory corruption when restoring shared game state.
    static bool s_postRestoreTickDump = false;
    if (g_restoreRequestedFrame >= 0) {
        int32_t rf = g_restoreRequestedFrame;
        g_restoreRequestedFrame = -1;
        HookLog("Rollback: executing deferred restore to frame %d\n", rf);
        Rollback_RestoreSnapshot(rf);
        s_postRestoreTickDump = true;
    }

    // Pending re-sim request from HookUpdate thread (FrameStepper_Process).
    // Execute it HERE on the main game thread so sRender path runs safely.
    if (g_resimRequestedStart >= 0) {
        int32_t rs = g_resimRequestedStart;
        int32_t re = g_resimRequestedEnd;
        g_resimRequestedStart = -1;
        g_resimRequestedEnd   = -1;
        Rollback_ReSimulate(rs, re);
        // After re-sim, let the normal frame proceed
    }

    // F7 re-sim test: save snapshot, record N frames of normal play, restore
    // and re-sim with the captured inputs (exercises the full rollback path).
    if (g_f7State == 1) {
        g_f7StartFrame = (int32_t)Controller_GetNetFrameCount();
        Rollback_SaveSnapshot(g_f7StartFrame);
        // Initialize diag session so Rollback_ReSimulate reuses the same one
        // (live per-frame dumps during recording + resim dumps side by side).
        Rollback_DiagInit();
        g_f7DiagSession = g_diagSessionId;
        g_diagSessionId++;
        g_f7State = 2;
        HookLog("F7: snapshot saved at frame %d, recording %d frames\n",
                g_f7StartFrame, g_f7Remaining);
    }
    if (g_f7State == 2) {
        g_f7Remaining--;
        if (g_f7Remaining <= 0) {
            // Last recording frame — capture live state before re-sim
            // so it lines up with the resim dumps (same diag session).
            if (g_f7DiagSession >= 0) {
                uint64_t sr = *(uint64_t*)_addr(PTR_SREPLAY);
                int32_t liveNum = sr ? *(int32_t*)(sr + 0x54) : 0;
                Rollback_DumpNDJSON(liveNum, 0);
                // session_* files disabled — use rdiag.ndjson instead
                //Rollback_DumpManifest(g_f7DiagSession, liveNum, 0);
            }
            g_f7State = 0;
            HookLog("F7: %d frames recorded, restoring frame %d and re-simulating\n",
                    8, g_f7StartFrame);
            Rollback_ReSimulate(g_f7StartFrame, g_f7StartFrame + 8);
            g_f7DiagSession = -1;

            // Mirror the netplay post-resim setup:
            //   1. Re-save the snapshot at the current frame so the ring buffer
            //      reflects the corrected state (not the stale pre-resim state).
            //   2. Flag the first post-resim live tick for diagnostics.
            int32_t curFrame = (int32_t)Controller_GetNetFrameCount();
            if (curFrame >= 0) {
                Rollback_SaveSnapshot(curFrame);
                g_postResimFrame = curFrame;
            }
        }
    }

    if (g_isResimulating) {
        // Must call oMvc3Main_move during re-sim — the game has an internal
        // dispatch loop that calls 0x1402594A0 (sMvc3Main__move) and expects
        // it to actually process game state.  If we return SKIPPED the
        // dispatch loop condition is never met and it spins forever.
        // Calling oMvc3Main_move here processes the frame normally (timing
        // is NOP'd by hkMain_move + render calls through without issue).
        oMvc3Main_move(param_1);
        return;
    }

    // ==================================================================
    // Auto-snapshot + rollback for netplay (only on main game thread,
    // not during re-sim).  Only active when rollback frames > 0.
    // ==================================================================
    if (Netplay_IsActive() && !g_isResimulating && Controller_IsRollbackEnabled())
    {
        // Only snapshot + rollback when in-game (aNextBattle step >= 2,
        // i.e. countdown or fighting).  Menus/loading screens don't need
        // rollback state and the frame counter jumps around there.
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        static GetAmaterasu_t getAmaterasu = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasu();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;
        if (step >= 0)
        {
            // Clear post-resim boundary flag at start of each frame.
            // This ensures the flag is active for exactly one live tick
            // (the one immediately after Rollback_ReSimulate returns).
            if (!g_isResimulating)
                g_postResimFrame = -1;

            int32_t curFrame = (int32_t)Controller_GetNetFrameCount();

            // Save snapshot for the current frame BEFORE the game tick
            Rollback_SaveSnapshot(curFrame);

            // Consume any pending rollback request from controller.cpp
            if (g_rollbackReqStart >= 0)
            {
                int32_t rs = g_rollbackReqStart;
                int32_t re = g_rollbackReqEnd;
                g_rollbackReqStart = -1;
                g_rollbackReqEnd   = -1;
                Rollback_ReSimulate(rs, re);

                // Notify the remote peer which frames we just re-simulated,
                // including per-frame resim results for BOTH teams' point
                // characters so their NDJSON log shows both sides' states.
                {
                    ResimNotifyPacket rnPkt = {};
                    rnPkt.type = PACKET_RESIM_NOTIFY;
                    rnPkt.resimStartFrame = rs;
                    rnPkt.resimEndFrame = re;
                    rnPkt.correctedTeam = Netplay_IsHost() ? 1 : 0;
                    int32_t notifyStart = rs - 2;
                    if (notifyStart < 0) notifyStart = 0;
                    int32_t notifyEnd = re + 2;
                    int32_t frameCount = 0;
                    for (int32_t f = notifyStart; f < notifyEnd && frameCount < MAX_RESIM_FRAMES; f++)
                    {
                        auto& rf = rnPkt.frames[frameCount];
                        rf.frame = f;

                        auto fillLive = [](ResimCharState& dst, const LiveFrameRecord& src) {
                        dst.actionId = src.actionId;
                        dst.posX     = src.posX;
                        dst.posY     = src.posY;
                        dst.posZ     = src.posZ;
                        dst.flags    = src.flags14F0;
                        dst.mHeld    = src.mHeld;
                        dst.frameCnt = src.frameCnt;
                        for (int ri = 0; ri < 4; ri++)
                            dst.rng[ri] = src.rng[ri];
                    };

                    bool isResimFrame = (f >= rs && f < re);
                    if (isResimFrame)
                    {
                        // PRE state from g_liveRef (live state recorded during gameplay)
                        uint32_t lrIdx = (uint32_t)(f >= 0 ? f : 0) % 256;
                        fillLive(rf.p1Pre, g_liveRef[0][lrIdx]);
                        fillLive(rf.p2Pre, g_liveRef[1][lrIdx]);

                        // POST state from pending resim dump (resimulation output)
                        uint32_t pidx = (uint32_t)(f >= 0 ? f : 0) % PENDING_RESIM_DUMP_RING;
                        if (g_pendingResimDumpValid[pidx] && g_pendingResimDumps[pidx].frame == f) {
                            rf.p1Post = g_pendingResimDumps[pidx].p1Post;
                            rf.p2Post = g_pendingResimDumps[pidx].p2Post;
                        } else {
                            // Fallback: post == pre if dump not available
                            rf.p1Post = rf.p1Pre;
                            rf.p2Post = rf.p2Pre;
                        }
                    }
                    else
                    {
                        // Context frame: pre == post == live state from g_liveRef
                        uint32_t liveIdx = (uint32_t)(f >= 0 ? f : 0) % 256;
                        fillLive(rf.p1Pre,  g_liveRef[0][liveIdx]);
                        fillLive(rf.p1Post, g_liveRef[0][liveIdx]);
                        fillLive(rf.p2Pre,  g_liveRef[1][liveIdx]);
                        fillLive(rf.p2Post, g_liveRef[1][liveIdx]);
                    }

                        rf.usedInput = Controller_GetUsedRemoteInput(f);
                        rf.recvInput = Controller_GetReceivedRemoteInput(f);
                        frameCount++;
                    }
                    rnPkt.frameCount = (uint32_t)frameCount;
                    Netplay_SendResimNotify(rnPkt);
                }

                // Re-resolve the current frame's remote input — packets that
                // arrived during the resim may now provide real data where we
                // only had a prediction before.
                Controller_ResolveRemoteInput(curFrame);
                HookLog("Rollback: re-resolved remote input for frame %d\n", curFrame);

                // After re-sim, re-save the current frame's snapshot so it
                // reflects the corrected state at start of curFrame.
                Rollback_SaveSnapshot(curFrame);

                // Flag the first post-resim live tick for diagnostic logging.
                g_postResimFrame = curFrame;
                HookLog("Rollback: resim complete, live tick frame=%d (post-resim boundary)\n",
                        curFrame);
            }
        }
    }

    oMvc3Main_move(param_1);

    // Capture per-frame diag snapshot after every live frame tick
    Rollback_CaptureFrameDiagSnapshot((int32_t)Controller_GetNetFrameCount(), /*isResim=*/false);

    // Netplay per-frame live NDJSON logging — every live frame logged
    // so both sides have a complete frame-by-frame record for diagnostics.
    if (Netplay_IsActive() && !g_isResimulating) {
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        static GetAmaterasu_t getAmaterasu = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasu();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;
        if (step >= 0) {
            int32_t curFrame = (int32_t)Controller_GetNetFrameCount();
            if (curFrame >= 0) {
                Rollback_DumpNetplayLive(curFrame);
            }
        }
    }

    // F7 live capture: dump state after each recording frame tick so it appears
    // alongside the corresponding resim dump in the same diag session files.
    // Works for frames 1-7 (g_f7Remaining > 0).  Frame 8 is captured before
    // Rollback_ReSimulate (see g_f7State == 2 handler above).
    if (g_f7DiagSession >= 0 && g_f7State == 2) {
        uint64_t sr = *(uint64_t*)_addr(PTR_SREPLAY);
        int32_t liveNum = sr ? *(int32_t*)(sr + 0x54) : 0;
        Rollback_DumpNDJSON(liveNum, 0);
        // session_* files disabled — use rdiag.ndjson instead
        //Rollback_DumpManifest(g_f7DiagSession, liveNum, 0);
    }

    // Post-tick diagnostic: dump character animation state after restore+tick
    if (s_postRestoreTickDump) {
        s_postRestoreTickDump = false;
        uint64_t sChar = *(uint64_t*)_addr(PTR_SCHARACTER);
        if (sChar) {
            for (int ti = 0; ti < 2; ++ti) {
                uint64_t team = sChar + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
                uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
                while (node) {
                    uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                    if (chr) {
                        uint32_t* a = (uint32_t*)(chr + 0x1300);
                 //       HookLog("DIAG Post-tick char@0x%llX team%d: anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                  //              chr, ti,
                 //               a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                 //               a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
                    }
                    node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                }
            }
        }
    }
}

// ===========================================================================
// Hook — collision sound-event iterator (null-pointer bug workaround)
//
// During re-simulation the sound slot-index increments past per-entry
// capacity, triggering a null-pointer dereference inside this function.
// Sound is irrelevant during headless re-sim, so we intercept and return.
// ===========================================================================
static void __fastcall hkCollisionSoundUpdate(uint64_t param_1)
{
    // Bypass during re-simulation; process normally otherwise
    if (g_isResimulating) {
        return;
    }
    if (oCollisionSoundUpdate) {
        oCollisionSoundUpdate(param_1);
    }
}

// ===========================================================================
// Hook — render/present function (FUN_14053a250)
//
// Called at the end of sMvc3Main__move as the last operation on the render
// manager (sMain+0x40190).  Performs frame-rate checking, Present(), and
// swap-chain management.
//
// During re-sim (g_isResimulating) we skip this entirely to avoid the
// ~16ms VSYNC wait per frame.  The re-sim now runs on the main game thread
// (via g_resimRequestedStart/End consumed in hkMvc3Main_move), so skipping
// this is safe — no thread-safety issue with the render path.
// ===========================================================================
static void __fastcall hkCallSRender(uint64_t param_1)
{
    if (g_isResimulating) {
        return;
    }
    oCallSRender(param_1);
}

// ===========================================================================
// Hook — sMain__move (frame timing / Sleep / busy-wait)
//
// Reads QPC, computes frame deltas, NOPs Sleep calls, and spins in a
// busy-wait for frame pacing.  During re-simulation we skip it entirely
// (just set tick count to 1) because:
//   1. Sleep is already NOP'd by Rollback_PatchSleep, but the busy-wait
//      still spins for the full frame time (~16ms @ 60fps).
//   2. Accumulated/deadline timing values drift across calls, causing
//      progressively longer busy-waits that eventually hang forever.
//   3. We sync QPC state via Rollback_ResyncMainTiming after the loop.
// ===========================================================================
static void __fastcall hkMain_move(uint64_t param_1)
{
    if (g_isResimulating) {
        // Force tick=0 so the game's dispatcher loop won't call
        // sMvc3Main__move again.  Setting tick=1 was the root cause
        // of the infinite loop: the dispatcher checked tick, saw 1,
        // called sMvc3Main__move which hit hkMvc3Main_move (SKIPPED),
        // tick was never consumed, so the dispatcher looped forever.
        *(uint32_t*)(param_1 + 0x48) = 0;
        return;
    }
    oMain_move(param_1);
}

// ===========================================================================
// VTable patch — post-render swapchain operations (0x50 slot)
//
// The two vtable+0x50 calls at the end of sMvc3Main__move (on objects at
// sMain+0x40208 and sMain+0x40270) are swapchain / D3D operations that
// depend on the render function having set up the frame.  When we NOP the
// render function during re-sim these calls hang.  Since we can't easily
// MinHook each vtable entry, we temporarily replace them with a NOP stub
// for the duration of the re-sim loop.
// ===========================================================================
void Rollback_InitHooks()
{
    // [EXPERIMENTAL] QPC hook install — see mvc3/rollback W3-C2
    // g_main_thread_id = GetCurrentThreadId();
    // {
    //     HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    //     void* qpc_target = (void*)GetProcAddress(hKernel, "QueryPerformanceCounter");
    //     if (qpc_target) {
    //         MH_STATUS sq = MH_CreateHook(qpc_target, (LPVOID)&hooked_qpc, (void**)&g_real_qpc);
    //         if (sq == MH_OK) {
    //             MH_EnableHook(qpc_target);
    //             HookLog("Rollback: QPC hook installed (main_tid=%lu)\n", g_main_thread_id);
    //         } else {
    //             HookLog("Rollback: WARN — QPC hook failed (%d), resim won't be deterministic\n", sq);
    //         }
    //     } else {
    //         HookLog("Rollback: WARN — could not resolve QueryPerformanceCounter\n");
    //     }
    // }

    MH_STATUS s = MH_CreateHook(
        (LPVOID)_addr(FN_SMVC3MAIN_MOVE),
        (LPVOID)&hkMvc3Main_move,
        (LPVOID*)&oMvc3Main_move
    );

    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMVC3MAIN_MOVE));
        HookLog("Rollback: hooked sMvc3Main__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMVC3MAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMvc3Main__move (%d)\n", s);
    }

    // Collision sound-event iterator — NOP during re-sim
    s = MH_CreateHook(
        (LPVOID)_addr(FN_COLLISION_SOUND_UPDATE),
        (LPVOID)&hkCollisionSoundUpdate,
        (LPVOID*)&oCollisionSoundUpdate
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_COLLISION_SOUND_UPDATE));
        HookLog("Rollback: hooked collision sound update (0x%llX)\n",
                (uint64_t)_addr(FN_COLLISION_SOUND_UPDATE));
    } else {
        HookLog("Rollback: failed to hook collision sound update (%d)\n", s);
    }

    // Render/present function — NOP during re-sim so the full game tick
    // (oMvc3Main_move) can be called from Rollback_ReSimulate without
    // actually drawing to the screen.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_CALL_SRENDER),
        (LPVOID)&hkCallSRender,
        (LPVOID*)&oCallSRender
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_CALL_SRENDER));
        HookLog("Rollback: hooked sRender call (0x%llX)\n",
                (uint64_t)_addr(FN_CALL_SRENDER));
    } else {
        HookLog("Rollback: failed to hook sRender call (%d)\n", s);
    }

    // sMain__move (frame-timing / QPC / Sleep / busy-wait) — NOP during re-sim
    // so the full game tick runs at full speed without frame-pacing waits.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_SMAIN_MOVE),
        (LPVOID)&hkMain_move,
        (LPVOID*)&oMain_move
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMAIN_MOVE));
        HookLog("Rollback: hooked sMain__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMain__move (%d)\n", s);
    }

}