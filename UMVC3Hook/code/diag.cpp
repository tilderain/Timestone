#include "../pch.h"
#include "diag.h"
#include "controller.h"
#include "rollback.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstring>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t ADDR_CHARACTER_MOVE_BASE     = 0x14004BD30;
static constexpr uintptr_t ADDR_CHARACTER_PRE_MOVE      = 0x14005E390;
static constexpr uintptr_t ADDR_ACTION_SIDE_CAMERA_MOVE = 0x14001AA20;
static constexpr uintptr_t ADDR_INPUT_MASK_POST_PROCESS  = 0x14002E5A0;
static constexpr uintptr_t ADDR_GET_TEAM_INDEX           = 0x140004AC0;
static constexpr uintptr_t ADDR_TEAM_COMMIT_SWAP         = 0x140021510;
static constexpr uintptr_t ADDR_TEAM_UPDATE_AGGREGATE    = 0x140022870;
static constexpr uintptr_t ADDR_ACTION_FIGHTING_CORE     = 0x140006370;

static constexpr uintptr_t PTR_SCHARACTER  = 0x140d44a70;
static constexpr uintptr_t TEAM0_OFFSET    = 0x40;
static constexpr uintptr_t TEAM1_OFFSET    = 0x300;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;

static int32_t Diag_GetGameFrameCount()
{
    // Headless resimulation labels frames explicitly; sReplay remains on the
    // live timeline while the native tick is replayed.
    if (g_isResimulating)
        return g_resimulateCurrentFrame;

    static constexpr uintptr_t G_PSREPLAY_PTR = 0x140d4d9d8;
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (!sReplay) return -1;
    return *(int32_t*)(sReplay + 0x54);
}

static bool Diag_TraceLifecycleFrame(int32_t frame)
{
    return (frame >= 14 && frame <= 20) || (frame >= 328 && frame <= 333);
}

static void(__fastcall* oCharacterMoveBase)(uint64_t uChar) = nullptr;
static void(__fastcall* oCharacterPreMove)(uint64_t pre) = nullptr;
static void(__fastcall* oActionSideCameraMove)(uint64_t camera) = nullptr;
static void(__fastcall* oInputMaskPostProcess)(uint64_t cChrInput) = nullptr;
static bool(__fastcall* oTeamCommitSwap)(uint64_t team) = nullptr;
static void(__fastcall* oTeamUpdateAggregate)(uint64_t team) = nullptr;
static void(__fastcall* oActionFightingCore)(uint64_t action) = nullptr;

static int Diag_GetTeamIndex(uint64_t team)
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter || team == 0) return -1;
    if (team == sCharacter + TEAM0_OFFSET) return 0;
    if (team == sCharacter + TEAM1_OFFSET) return 1;
    return -1;
}

static void Diag_LogTeamState(const char* phase, uint64_t team, int32_t frame)
{
    if (!team || (!g_isResimulating && !Diag_TraceLifecycleFrame(frame))) return;
    HookLog("TEAMTRACE frame=%d resim=%d phase=%s team=%d team=0x%llX "
            "active=0x%llX flag=%08X mCPrgF=%08X pending=%08X,%08X\n",
            frame, g_isResimulating ? 1 : 0, phase, Diag_GetTeamIndex(team), team,
            *(uint64_t*)(team + 0x48), *(uint32_t*)(team + 0x58),
            *(uint32_t*)(team + 0xFC), *(uint32_t*)(team + 0xA0),
            *(uint32_t*)(team + 0xA4));
}

static bool __fastcall hkTeamCommitSwap(uint64_t team)
{
    int32_t frame = Diag_GetGameFrameCount();
    Diag_LogTeamState("commit-before", team, frame);
    bool result = oTeamCommitSwap(team);
    Diag_LogTeamState("commit-after", team, frame);
    return result;
}

static void __fastcall hkTeamUpdateAggregate(uint64_t team)
{
    int32_t frame = Diag_GetGameFrameCount();
    Diag_LogTeamState("aggregate-before", team, frame);
    oTeamUpdateAggregate(team);
    Diag_LogTeamState("aggregate-after", team, frame);
}

static void __fastcall hkActionFightingCore(uint64_t action)
{
    int32_t frame = Diag_GetGameFrameCount();
    if (Diag_TraceLifecycleFrame(frame)) {
        HookLog("ACTIONTRACE frame=%d resim=%d phase=before action=0x%llX\n",
                frame, g_isResimulating ? 1 : 0, action);
    }
    oActionFightingCore(action);
    if (Diag_TraceLifecycleFrame(frame)) {
        HookLog("ACTIONTRACE frame=%d resim=%d phase=after action=0x%llX\n",
                frame, g_isResimulating ? 1 : 0, action);
    }
}

FrameDiagSnapshot g_liveFrameSnap[ROLLBACK_WINDOW_DIAG];
FrameDiagSnapshot g_resimFrameSnap[ROLLBACK_WINDOW_DIAG];
int g_resimCharCounter = 0;
int g_diagLiveCharCount[ROLLBACK_WINDOW_DIAG][2];

LiveFrameRecord g_liveRef[2][256];

void CaptureCharSnapshot(uint64_t uChar, uint32_t teamIdx, uint32_t charSlot, CharDiagSnapshot& snap)
{
    std::memset(&snap, 0, sizeof(snap));
    if (!uChar) return;
    uint64_t cci = uChar + 0x21C8;
    std::memcpy(snap.cchr, (void*)cci, CCHRINPUT_SIZE);
    snap.posX = *(float*)(uChar + 0x50);
    snap.posY = *(float*)(uChar + 0x54);
    snap.posZ = *(float*)(uChar + 0x58);
    snap.actionId = *(uint32_t*)(uChar + 0x1310);
    snap.flags14F0 = *(uint32_t*)(uChar + 0x14F0);
    snap.flags14F4 = *(uint32_t*)(uChar + 0x14F4);
    snap.flags14F8 = *(uint32_t*)(uChar + 0x14F8);
    snap.flags1504 = *(uint32_t*)(uChar + 0x1504);
    snap.flags1510 = *(uint32_t*)(uChar + 0x1510);
    snap.flags1514 = *(uint32_t*)(uChar + 0x1514);
    snap.flags1524 = *(uint32_t*)(uChar + 0x1524);
    for (int i = 0; i < ANIM_FIELD_COUNT; i++)
        snap.animFields[i] = *(uint32_t*)(uChar + 0x1300 + i * 4);
    for (int i = 0; i < XORSHIFT_SIZE; i++)
        snap.xorShift[i] = *(uint32_t*)(uChar + 0x2D80 + i * 4);
    snap.uCharAddr = uChar;
    snap.teamIdx = teamIdx;
    snap.charSlot = charSlot;
}

static void __fastcall hkActionSideCameraMove(uint64_t camera)
{
    int32_t frame = Diag_GetGameFrameCount();
    bool trace = g_isResimulating;
    uint8_t pre[0x20] = {};
    uint64_t active[2] = {};
    float activePos[2][3] = {};
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        for (int team = 0; team < 2; ++team) {
            uint64_t teamObject = sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            active[team] = *(uint64_t*)(teamObject + 0x48);
            if (active[team]) {
                activePos[team][0] = *(float*)(active[team] + 0x50);
                activePos[team][1] = *(float*)(active[team] + 0x54);
                activePos[team][2] = *(float*)(active[team] + 0x58);
            }
        }
    }
    if (camera)
        std::memcpy(pre, (uint8_t*)camera + 0x80, sizeof(pre));

    oActionSideCameraMove(camera);

    if (camera) {
        uint8_t post[0x20] = {};
        std::memcpy(post, (uint8_t*)camera + 0x80, sizeof(post));
        bool changed = std::memcmp(pre, post, sizeof(pre)) != 0;
        if (trace || changed || Diag_TraceLifecycleFrame(frame)) {
            HookLog("CAMTRACE frame=%d resim=%d changed=%d caller=0x%llX camera=0x%llX "
                "active=0x%llX/0x%llX pos0=(%+.5f,%+.5f,%+.5f) "
                "pos1=(%+.5f,%+.5f,%+.5f) "
                "pre50=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "pre80=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "pre88=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "pre90=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "pre98=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "post80=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "post88=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "post90=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "post98=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
                frame, g_isResimulating ? 1 : 0, changed ? 1 : 0,
                (uint64_t)__builtin_return_address(0), camera,
                active[0], active[1],
                activePos[0][0], activePos[0][1], activePos[0][2],
                activePos[1][0], activePos[1][1], activePos[1][2],
                ((uint8_t*)camera)[0x50], ((uint8_t*)camera)[0x51],
                ((uint8_t*)camera)[0x52], ((uint8_t*)camera)[0x53],
                ((uint8_t*)camera)[0x54], ((uint8_t*)camera)[0x55],
                ((uint8_t*)camera)[0x56], ((uint8_t*)camera)[0x57],
                pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7],
                pre[8], pre[9], pre[10], pre[11], pre[12], pre[13], pre[14], pre[15],
                pre[16], pre[17], pre[18], pre[19], pre[20], pre[21], pre[22], pre[23],
                pre[24], pre[25], pre[26], pre[27], pre[28], pre[29], pre[30], pre[31],
                post[0], post[1], post[2], post[3], post[4], post[5], post[6], post[7],
                post[8], post[9], post[10], post[11], post[12], post[13], post[14], post[15],
                post[16], post[17], post[18], post[19], post[20], post[21], post[22], post[23],
                post[24], post[25], post[26], post[27], post[28], post[29], post[30], post[31]);
        }
    }
}

static void __fastcall hkCharacterPreMove(uint64_t pre)
{
    int32_t frame = Diag_GetGameFrameCount();
    bool trace = g_isResimulating;
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (trace && sCharacter) {
        HookLog("PRETRACE frame=%d pre=0x%llX ", frame, pre);
        for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
            uint64_t team = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t active = *(uint64_t*)(team + 0x48);
            HookLog("team%d flags=0x%08X active=0x%llX mCPrgF=0x%08X ",
                    teamIndex, *(uint32_t*)(team + 0x58), active,
                    *(uint32_t*)(team + 0xFC));
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
            for (int slot = 0; node && slot < 3; ++slot) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                HookLog("c%d=0x%llX:u10=%llX:c14F8=%08X:c150C=%08X:"
                        "151C=%08X:29D4=%08X:2AD0=%08X:2D94=%08X:"
                        "64D4=%08X:6554=%08X ", slot,
                        character,
                        character ? *(uint64_t*)(character + 0x10) : 0,
                        character ? *(uint32_t*)(character + 0x14F8) : 0,
                        character ? *(uint32_t*)(character + 0x150C) : 0,
                        character ? *(uint32_t*)(character + 0x151C) : 0,
                        character ? *(uint32_t*)(character + 0x29D4) : 0,
                        character ? *(uint32_t*)(character + 0x2AD0) : 0,
                        character ? *(uint32_t*)(character + 0x2D94) : 0,
                        character ? *(uint32_t*)(character + 0x64D4) : 0,
                        character ? *(uint32_t*)(character + 0x6554) : 0);
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
        HookLog("\n");
    }
    oCharacterPreMove(pre);
}

static void LogSnapshotDiff(const CharDiagSnapshot& live, const CharDiagSnapshot& resim,
                            int frame, int teamIdx, const char* phase)
{
    return;
    if (std::memcmp(live.cchr, resim.cchr, CCHRINPUT_SIZE) != 0) {
        HookLog("DASH_DIAG frame=%d team=%d %s cChrInput[0..%X] DIVERGED:\n",
                frame, teamIdx, phase, CCHRINPUT_SIZE);
        uint32_t* lw = (uint32_t*)live.cchr;
        uint32_t* rw = (uint32_t*)resim.cchr;
        for (int i = 0; i < CCHRINPUT_SIZE / 4; i++) {
            if (lw[i] != rw[i]) {
                HookLog("  +0x%03X: live=0x%08X resim=0x%08X", i * 4, lw[i], rw[i]);
                if (i * 4 >= 0x360 && i * 4 < 0x560) {
                    int dirSlot = (i * 4 - 0x360) / 8;
                    HookLog(" [dirHist[%d]]", dirSlot);
                }
                else if (i * 4 == 0x10) HookLog(" [mOldHeld]");
                else if (i * 4 == 0x14) HookLog(" [mHeld]");
                else if (i * 4 == 0x560) HookLog(" [mRemapped]");
                else if (i * 4 == 0x568) HookLog(" [mMask]");
                else if (i * 4 == 0x56C) HookLog(" [mProcessed]");
                else if (i * 4 == 0x590) HookLog(" [h590]");
                else if (i * 4 == 0x594) HookLog(" [h594]");
                HookLog("\n");
            }
        }
    }
    if (live.posX != resim.posX || live.posY != resim.posY || live.posZ != resim.posZ) {
        HookLog("DASH_DIAG frame=%d team=%d %s POS: live=(%+.4f,%+.4f,%+.4f) resim=(%+.4f,%+.4f,%+.4f)\n",
            frame, teamIdx, phase,
            live.posX, live.posY, live.posZ, resim.posX, resim.posY, resim.posZ);
    }
    if (live.actionId != resim.actionId) {
        HookLog("DASH_DIAG frame=%d team=%d %s actionId: live=0x%08X resim=0x%08X\n",
            frame, teamIdx, phase, live.actionId, resim.actionId);
    }
    if (live.flags14F0 != resim.flags14F0) {
        HookLog("DASH_DIAG frame=%d team=%d %s flags14F0: live=0x%08X resim=0x%08X\n",
            frame, teamIdx, phase, live.flags14F0, resim.flags14F0);
    }
    if (live.flags14F8 != resim.flags14F8) {
        HookLog("DASH_DIAG frame=%d team=%d %s flags14F8: live=0x%08X resim=0x%08X\n",
            frame, teamIdx, phase, live.flags14F8, resim.flags14F8);
    }
    if (live.flags1514 != resim.flags1514) {
        HookLog("DASH_DIAG frame=%d team=%d %s flags1514: live=0x%08X resim=0x%08X\n",
            frame, teamIdx, phase, live.flags1514, resim.flags1514);
    }
    for (int i = 0; i < XORSHIFT_SIZE; i++) {
        if (live.xorShift[i] != resim.xorShift[i]) {
            HookLog("DASH_DIAG frame=%d team=%d %s xorShift[%d]: live=0x%08X resim=0x%08X\n",
                frame, teamIdx, phase, i, live.xorShift[i], resim.xorShift[i]);
            break;
        }
    }
}

void DiagLogPreOverride(uint64_t cChrInput, uint32_t teamIdx)
{
    return;
    if (!cChrInput) return;
    uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
    uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
    uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
    extern int32_t g_resimulateCurrentFrame;
    int32_t f = g_resimulateCurrentFrame;
    uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
    uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
    uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
    uint32_t slotIdx = frameCnt & 0x3F;
    uint32_t dir0 = *(uint32_t*)(cChrInput + 0x1B0);
    uint32_t dir1 = *(uint32_t*)(cChrInput + 0x1B4);
    float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
    float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
    float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    HookLog("ResimDiag frame=%d team=%d pre: mOld=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X mRemapped=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
            f, teamIdx,
            *(uint32_t*)(cChrInput + 0x10),
            *(uint32_t*)(cChrInput + 0x14),
            *(uint32_t*)(cChrInput + 0x18),
            *(uint32_t*)(cChrInput + 0x1C),
            *(uint32_t*)(cChrInput + 0x560),
            *(uint32_t*)(cChrInput + 0x568),
            *(uint32_t*)(cChrInput + 0x56C),
            frameCnt, head590, head594,
            *(uint64_t*)(cChrInput + 0x28),
            *(uint32_t*)(cChrInput + 0x0A8),
            *(uint64_t*)(cChrInput + 0x0AC),
            *(uint64_t*)(cChrInput + 0x130),
            dir0, dir1,
            *(uint64_t*)(cChrInput + 0x360),
            *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
            *(uint64_t*)(cChrInput + 0x368),
            *(uint64_t*)(cChrInput + 0x370),
            actionId, flags14F0, px, py, pz);
}

void DiagLogPostOverride(uint64_t cChrInput, uint32_t teamIdx)
{
    return;
    if (!cChrInput) return;
    uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
    uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
    uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
    extern int32_t g_resimulateCurrentFrame;
    int32_t f = g_resimulateCurrentFrame;
    uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
    uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
    uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
    uint32_t slotIdx = frameCnt & 0x3F;
    uint32_t dir0_p = *(uint32_t*)(cChrInput + 0x1B0);
    uint32_t dir1_p = *(uint32_t*)(cChrInput + 0x1B4);
    float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
    float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
    float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    HookLog("ResimDiag frame=%d team=%d post: mOld=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
            f, teamIdx,
            *(uint32_t*)(cChrInput + 0x10),
            *(uint32_t*)(cChrInput + 0x14),
            *(uint32_t*)(cChrInput + 0x18),
            *(uint32_t*)(cChrInput + 0x1C),
            *(uint32_t*)(cChrInput + 0x568),
            *(uint32_t*)(cChrInput + 0x56C),
            frameCnt, head590, head594,
            *(uint64_t*)(cChrInput + 0x28),
            *(uint32_t*)(cChrInput + 0x0A8),
            *(uint64_t*)(cChrInput + 0x0AC),
            *(uint64_t*)(cChrInput + 0x130),
            dir0_p, dir1_p,
            *(uint64_t*)(cChrInput + 0x360),
            *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
            *(uint64_t*)(cChrInput + 0x368),
            *(uint64_t*)(cChrInput + 0x370),
            actionId, flags14F0, px, py, pz);

    uint32_t liveIdx = (uint32_t)(f >= 0 ? f : 0) % 256;
    auto& lr = g_liveRef[teamIdx][liveIdx];
    HookLog("ResimDiag frame=%d team=%d live: mHeld=0x%08X m18=0x%08X m1C=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
            f, teamIdx,
            lr.mHeld, lr.m18, lr.m1C, lr.mMask, lr.mProcessed,
            lr.frameCnt, lr.h590, lr.h594,
            lr.heldR0, lr.relHead, lr.relR0, lr.prsR0,
            lr.dir0, lr.dir4,
            lr.dirRing0, lr.dirRingSlot, lr.dirRing1, lr.dirRing2,
            lr.actionId, lr.flags14F0,
            lr.posX, lr.posY, lr.posZ);

    uint32_t intendedInput = Controller_GetExpectedInput(teamIdx, f);
    uint32_t actualHeld = *(uint32_t*)(cChrInput + 0x14);
    if (g_isResimulating && intendedInput != actualHeld) {
        HookLog("INJECTION MISMATCH frame=%d team=%d: intended 0x%08X but cChrInput+0x14 = 0x%08X\n",
                f, teamIdx, intendedInput, actualHeld);
    }

    if (actionId != lr.actionId) {
        HookLog("ActDiv frame=%d team=%d resim_act=0x%08X live_act=0x%08X mHeld_post=0x%08X mHeld_live=0x%08X fl=0x%08X\n",
            f, teamIdx, actionId, lr.actionId,
            *(uint32_t*)(cChrInput + 0x14), lr.mHeld, flags14F0);
    }
}

void DiagCaptureLiveRef(uint64_t cChrInput, uint32_t teamIdx, int32_t liveFrame)
{
    if (!cChrInput || liveFrame < 0) return;
    uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
    uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
    uint32_t liveIdx = (uint32_t)liveFrame % 256;
    auto& rec = g_liveRef[teamIdx][liveIdx];
    rec.mHeld       = *(uint32_t*)(cChrInput + 0x14);
    rec.m18         = *(uint32_t*)(cChrInput + 0x18);
    rec.m1C         = *(uint32_t*)(cChrInput + 0x1C);
    rec.mRemapped   = *(uint32_t*)(cChrInput + 0x560);
    rec.mMask       = *(uint32_t*)(cChrInput + 0x568);
    rec.mProcessed  = *(uint32_t*)(cChrInput + 0x56C);
    rec.frameCnt    = frameCnt;
    rec.h590        = *(uint32_t*)(cChrInput + 0x590);
    rec.h594        = *(uint32_t*)(cChrInput + 0x594);
    rec.heldR0      = *(uint64_t*)(cChrInput + 0x28);
    rec.relHead     = *(uint32_t*)(cChrInput + 0x0A8);
    rec.relR0       = *(uint64_t*)(cChrInput + 0x0AC);
    rec.prsR0       = *(uint64_t*)(cChrInput + 0x130);
    rec.dir0        = *(uint32_t*)(cChrInput + 0x1B0);
    rec.dir4        = *(uint32_t*)(cChrInput + 0x1B4);
    rec.dirRing0    = *(uint64_t*)(cChrInput + 0x360);
    rec.dirRingSlot = *(uint64_t*)(cChrInput + 0x360 + (frameCnt & 0x3F) * 8);
    rec.dirRing1    = *(uint64_t*)(cChrInput + 0x368);
    rec.dirRing2    = *(uint64_t*)(cChrInput + 0x370);
    rec.actionId    = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
    rec.flags14F0   = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
    rec.posX        = uChar ? *(float*)(uChar + 0x50) : 0.0f;
    rec.posY        = uChar ? *(float*)(uChar + 0x54) : 0.0f;
    rec.posZ        = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    if (uChar) {
        for (int i = 0; i < 4; i++)
            rec.rng[i] = *(uint32_t*)(uChar + 0x2D80 + i * 4);
    } else {
        std::memset(rec.rng, 0, sizeof(rec.rng));
    }
}

void DiagLogPostResim(uint64_t cChrInput, uint32_t teamIdx, int32_t frame)
{
        return;
    if (!cChrInput) return;
    uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
    uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
    uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
    uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
    uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
    uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
    uint32_t slotIdx = frameCnt & 0x3F;
    uint32_t dir0_pr = *(uint32_t*)(cChrInput + 0x1B0);
    uint32_t dir1_pr = *(uint32_t*)(cChrInput + 0x1B4);
    float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
    float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
    float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    HookLog("PostResim frame=%d team=%d mHeld=0x%08X m18=0x%08X m1C=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
            frame, teamIdx,
            *(uint32_t*)(cChrInput + 0x14),
            *(uint32_t*)(cChrInput + 0x18),
            *(uint32_t*)(cChrInput + 0x1C),
            *(uint32_t*)(cChrInput + 0x56C),
            frameCnt, head590, head594,
            *(uint64_t*)(cChrInput + 0x28),
            *(uint32_t*)(cChrInput + 0x0A8),
            *(uint64_t*)(cChrInput + 0x0AC),
            *(uint64_t*)(cChrInput + 0x130),
            dir0_pr, dir1_pr,
            *(uint64_t*)(cChrInput + 0x360),
            *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
            *(uint64_t*)(cChrInput + 0x368),
            *(uint64_t*)(cChrInput + 0x370),
            actionId, flags14F0, px, py, pz);
}

void DiagLogActDbg(uint64_t cChrInput, uint32_t teamIdx, int32_t curFrame, bool isResim)
{
    return;

    if (!cChrInput) return;
    uint64_t uc = *(uint64_t*)(cChrInput + 0x08);
    if (!uc) return;
    uint32_t act = *(uint32_t*)(uc + 0x1310);
    if (act == 0) return;
    uint64_t fcnt = *(uint64_t*)(cChrInput + 0x18);
    uint32_t slot = (uint32_t)(fcnt & 0x3F);
    uint32_t cBtn = *(uint16_t*)(cChrInput + 0x22);
    float px = *(float*)(uc + 0x50);
    float py = *(float*)(uc + 0x54);
    float pz = *(float*)(uc + 0x58);
    HookLog("ActDbg frame=%d team=%d act=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X cntr=%llu d[%u]=0x%016llX dir0=0x%08X dir4=0x%08X cBtn=0x%04X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f) %s\n",
            curFrame, teamIdx, act,
            *(uint32_t*)(cChrInput + 0x14),
            *(uint32_t*)(cChrInput + 0x18),
            *(uint32_t*)(cChrInput + 0x1C),
            fcnt, slot,
            *(uint64_t*)(cChrInput + 0x360 + slot * 8),
            *(uint32_t*)(cChrInput + 0x1B0),
            *(uint32_t*)(cChrInput + 0x1B4),
            cBtn,
            *(uint32_t*)(uc + 0x14F0),
            px, py, pz,
            isResim ? "RESIM" : "LIVE");
}

static void __fastcall hkCharacterMoveBase(uint64_t uChar)
{
    if (!uChar) {
        oCharacterMoveBase(uChar);
        return;
    }

    typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
    static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
    uint32_t teamIdx = fnGetTeamIdx(uChar);

    int32_t frame = Diag_GetGameFrameCount();
    int32_t diagFrame = g_isResimulating ? g_resimulateCurrentFrame : frame;

    static uint32_t s_frameCharCount[2] = {0, 0};
    static int32_t s_lastCountFrame = -1;
    if (diagFrame != s_lastCountFrame) {
        s_lastCountFrame = diagFrame;
        s_frameCharCount[0] = 0;
        s_frameCharCount[1] = 0;
    }
    uint32_t charSlot = s_frameCharCount[teamIdx];
    if (charSlot < MAX_CHARS_PER_TEAM) s_frameCharCount[teamIdx]++;
    if (charSlot >= MAX_CHARS_PER_TEAM) charSlot = MAX_CHARS_PER_TEAM - 1;
    uint32_t ringIdx = (uint32_t)(diagFrame >= 0 ? diagFrame : 0) % ROLLBACK_WINDOW_DIAG;

    CharDiagSnapshot preSnap;
    CaptureCharSnapshot(uChar, teamIdx, charSlot, preSnap);
    uint32_t preMoveCounter = *(uint32_t*)(uChar + 0x2D94);
    uint32_t prePrgF = *(uint32_t*)(uChar + 0x14F8);
    uint8_t prePackedState[4] = {};
    std::memcpy(prePackedState, (uint8_t*)uChar + 0x2888, sizeof(prePackedState));
    uint8_t preTargetState[8] = {};
    std::memcpy(preTargetState, (uint8_t*)uChar + 0x64D0, sizeof(preTargetState));
    uint8_t preEp[20] = {};
    std::memcpy(preEp, (uint8_t*)uChar + 0x3F84, sizeof(preEp));

    oCharacterMoveBase(uChar);

    CharDiagSnapshot postSnap;
    CaptureCharSnapshot(uChar, teamIdx, charSlot, postSnap);
    uint32_t postMoveCounter = *(uint32_t*)(uChar + 0x2D94);
    uint32_t postPrgF = *(uint32_t*)(uChar + 0x14F8);
    uint8_t postPackedState[4] = {};
    std::memcpy(postPackedState, (uint8_t*)uChar + 0x2888, sizeof(postPackedState));
    uint8_t postTargetState[8] = {};
    std::memcpy(postTargetState, (uint8_t*)uChar + 0x64D0, sizeof(postTargetState));
    uint8_t postEp[20] = {};
    std::memcpy(postEp, (uint8_t*)uChar + 0x3F84, sizeof(postEp));

    // The EP counter block is authoritative animation state. Trace it around
    // the native move so a rollback-only rewrite is distinguishable from a
    // restore gap.
    if (g_isResimulating) {
        HookLog("EPTRACE frame=%d resim=%d team=%u slot=%u char=0x%llX "
                "moveState=%02X,%02X,%02X,%02X "
                "pre=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                "post=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
                diagFrame, g_isResimulating ? 1 : 0, teamIdx, charSlot,
                uChar,
                *((uint8_t*)uChar + 0x2D94), *((uint8_t*)uChar + 0x2D95),
                *((uint8_t*)uChar + 0x2D96), *((uint8_t*)uChar + 0x2D97),
                preEp[0], preEp[1], preEp[2], preEp[3], preEp[4], preEp[5],
                preEp[6], preEp[7], preEp[8], preEp[9], preEp[10], preEp[11],
                preEp[12], preEp[13], preEp[14], preEp[15], preEp[16],
                preEp[17], preEp[18], preEp[19],
                postEp[0], postEp[1], postEp[2], postEp[3], postEp[4], postEp[5],
                postEp[6], postEp[7], postEp[8], postEp[9], postEp[10], postEp[11],
                 postEp[12], postEp[13], postEp[14], postEp[15], postEp[16],
                 postEp[17], postEp[18], postEp[19]);
        HookLog("FLAGTRACE frame=%d resim=1 team=%u slot=%u char=0x%llX "
                "prgF=%08X->%08X packed2888=%02X,%02X,%02X,%02X->%02X,%02X,%02X,%02X "
                "target64D0=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X->%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X\n",
                diagFrame, teamIdx, charSlot, uChar,
                prePrgF, postPrgF,
                prePackedState[0], prePackedState[1], prePackedState[2], prePackedState[3],
                postPackedState[0], postPackedState[1], postPackedState[2], postPackedState[3],
                preTargetState[0], preTargetState[1], preTargetState[2], preTargetState[3],
                preTargetState[4], preTargetState[5], preTargetState[6], preTargetState[7],
                postTargetState[0], postTargetState[1], postTargetState[2], postTargetState[3],
                postTargetState[4], postTargetState[5], postTargetState[6], postTargetState[7]);
        HookLog("TARGETTRACE frame=%d team=%u slot=%u char=0x%llX "
                "t0=0x%llX:%08X,%08X,%08X,%08X "
                "t1=0x%llX:%08X,%08X,%08X,%08X\n",
                diagFrame, teamIdx, charSlot, uChar,
                *(uint64_t*)(uChar + 0x6498),
                *(uint32_t*)(uChar + 0x64CC), *(uint32_t*)(uChar + 0x64D0),
                *(uint32_t*)(uChar + 0x64D4), *(uint32_t*)(uChar + 0x64D8),
                *(uint64_t*)(uChar + 0x6528),
                *(uint32_t*)(uChar + 0x655C), *(uint32_t*)(uChar + 0x6560),
                *(uint32_t*)(uChar + 0x6564), *(uint32_t*)(uChar + 0x6568));
    }

    if (Diag_TraceLifecycleFrame(diagFrame)) {
        HookLog("MOVETRACE frame=%d resim=%d team=%u slot=%u char=0x%llX "
                "caller=0x%llX 2D94=%08X->%08X\n",
                diagFrame, g_isResimulating ? 1 : 0, teamIdx, charSlot, uChar,
                (uint64_t)__builtin_return_address(0),
                preMoveCounter, postMoveCounter);
    }

    if (frame < 0) return;

    static int32_t s_lastDiagFrame = -1;
    if (diagFrame != s_lastDiagFrame) {
        s_lastDiagFrame = diagFrame;
        g_resimFrameSnap[ringIdx].charCount = 0;
        g_resimFrameSnap[ringIdx].frame = diagFrame;
        g_liveFrameSnap[ringIdx].charCount = 0;
        g_liveFrameSnap[ringIdx].frame = frame;
    }

    if (!g_isResimulating) {
        FrameDiagSnapshot& live = g_liveFrameSnap[ringIdx];
        int idx = live.charCount;
        if (idx < 6) {
            live.chars[idx].pre = preSnap;
            live.chars[idx].post = postSnap;
            live.charCount = idx + 1;
            live.frame = frame;
            g_diagLiveCharCount[ringIdx][teamIdx] = live.charCount;
        }

        static uint64_t s_lastPosFrame = 0;
        if (frame != (int32_t)s_lastPosFrame) {
            s_lastPosFrame = frame;
        }
    } else {
        FrameDiagSnapshot& live = g_liveFrameSnap[ringIdx];
        FrameDiagSnapshot& resim = g_resimFrameSnap[ringIdx];

        int liveIdx = -1;
        for (int i = 0; i < live.charCount && i < 6; i++) {
            if (live.chars[i].post.uCharAddr == uChar) {
                liveIdx = i;
                break;
            }
        }

        if (resim.charCount < 6) {
            int ri = resim.charCount;
            resim.chars[ri].pre = preSnap;
            resim.chars[ri].post = postSnap;
            resim.frame = diagFrame;
            resim.charCount = ri + 1;
        }

        if (liveIdx >= 0) {
            const CharDiagSnapshot& livePre = live.chars[liveIdx].pre;
            const CharDiagSnapshot& livePost = live.chars[liveIdx].post;

            if (std::memcmp(livePre.cchr, preSnap.cchr, CCHRINPUT_SIZE) != 0) {
                HookLog("DASH_DIAG frame=%d team=%d slot=%d ENTRY cChrInput DIVERGED (save/restore gap!):\n",
                        diagFrame, teamIdx, charSlot);
                uint32_t* lw = (uint32_t*)livePre.cchr;
                uint32_t* rw = (uint32_t*)preSnap.cchr;
                for (int i = 0; i < CCHRINPUT_SIZE / 4; i++) {
                    if (lw[i] != rw[i]) {
                        HookLog("  +0x%03X: live=0x%08X resim=0x%08X", i*4, lw[i], rw[i]);
                        if (i*4 == 0x10) HookLog(" [mOldHeld]");
                        else if (i*4 == 0x14) HookLog(" [mHeld]");
                        else if (i*4 == 0x560) HookLog(" [mRemapped]");
                        else if (i*4 == 0x568) HookLog(" [mMask]");
                        else if (i*4 == 0x56C) HookLog(" [mProcessed]");
                        else if (i*4 >= 0x360 && i*4 < 0x560) HookLog(" [dirHist[%d]]", (i*4-0x360)/8);
                        HookLog("\n");
                    }
                }
            }

            if (std::memcmp(livePost.cchr, postSnap.cchr, CCHRINPUT_SIZE) != 0) {
                HookLog("DASH_DIAG frame=%d team=%d slot=%d EXIT cChrInput DIVERGED (execution diff):\n",
                        diagFrame, teamIdx, charSlot);
                uint32_t* lw = (uint32_t*)livePost.cchr;
                uint32_t* rw = (uint32_t*)postSnap.cchr;
                for (int i = 0; i < CCHRINPUT_SIZE / 4; i++) {
                    if (lw[i] != rw[i]) {
                        HookLog("  +0x%03X: live=0x%08X resim=0x%08X", i*4, lw[i], rw[i]);
                        if (i*4 == 0x10) HookLog(" [mOldHeld]");
                        else if (i*4 == 0x14) HookLog(" [mHeld]");
                        else if (i*4 == 0x560) HookLog(" [mRemapped]");
                        else if (i*4 == 0x568) HookLog(" [mMask]");
                        else if (i*4 == 0x56C) HookLog(" [mProcessed]");
                        else if (i*4 >= 0x360 && i*4 < 0x560) HookLog(" [dirHist[%d]]", (i*4-0x360)/8);
                        HookLog("\n");
                    }
                }
            }

            if (livePost.posX != postSnap.posX || livePost.posY != postSnap.posY || livePost.posZ != postSnap.posZ) {
                HookLog("DASH_DIAG frame=%d team=%d slot=%d POS POST: live=(%+.4f,%+.4f,%+.4f) resim=(%+.4f,%+.4f,%+.4f)\n",
                        diagFrame, teamIdx, charSlot,
                        livePost.posX, livePost.posY, livePost.posZ,
                        postSnap.posX, postSnap.posY, postSnap.posZ);
                float liveDx = livePost.posX - livePre.posX;
                float liveDy = livePost.posY - livePre.posY;
                float resimDx = postSnap.posX - preSnap.posX;
                float resimDy = postSnap.posY - preSnap.posY;
                if (liveDx != resimDx) {
                    HookLog("DASH_DIAG frame=%d team=%d slot=%d VEL X: live=%.4f resim=%.4f diff=%.4f\n",
                        diagFrame, teamIdx, charSlot, liveDx, resimDx, liveDx - resimDx);
                }
                if (liveDy != resimDy) {
                    HookLog("DASH_DIAG frame=%d team=%d slot=%d VEL Y: live=%.4f resim=%.4f diff=%.4f\n",
                        diagFrame, teamIdx, charSlot, liveDy, resimDy, liveDy - resimDy);
                }
            }

            if (livePost.actionId != postSnap.actionId) {
                HookLog("DASH_DIAG frame=%d team=%d slot=%d actionId POST: live=0x%08X resim=0x%08X (pre: live=0x%08X resim=0x%08X)\n",
                        diagFrame, teamIdx, charSlot,
                        livePost.actionId, postSnap.actionId,
                        livePre.actionId, preSnap.actionId);
            }

            for (int i = 0; i < XORSHIFT_SIZE; i++) {
                if (livePre.xorShift[i] != preSnap.xorShift[i]) {
                    HookLog("DASH_DIAG frame=%d team=%d slot=%d RNG[%d] at ENTRY: live=0x%08X resim=0x%08X (SAVE/RESTORE GAP)\n",
                            diagFrame, teamIdx, charSlot, i, livePre.xorShift[i], preSnap.xorShift[i]);
                    break;
                }
            }
        }
    }
}

static void __fastcall hkInputMaskPostProcess(uint64_t cChrInput)
{
    uint64_t frameCnt_before = cChrInput ? *(uint64_t*)(cChrInput + 0x18) : 0;
    uint32_t head590_before = cChrInput ? *(uint32_t*)(cChrInput + 0x590) : 0;
    uint32_t head594_before = cChrInput ? *(uint32_t*)(cChrInput + 0x594) : 0;
    uint64_t dirHistSlot_before = 0;
    if (cChrInput) {
        uint32_t slotIdx = (uint32_t)(frameCnt_before & 0x3F);
        dirHistSlot_before = *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8);
    }

    oInputMaskPostProcess(cChrInput);

    if (cChrInput && g_isResimulating) {
        uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
        uint32_t slotIdx = (uint32_t)(frameCnt & 0x3F);
        uint64_t dirHistSlot = *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8);
        uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
        uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);

        if (dirHistSlot != dirHistSlot_before) {
            uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
            uint32_t teamIdx = 0;
            if (uChar) {
                typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
                static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
                teamIdx = fnGetTeamIdx(uChar);
            }
            //HookLog("DIRHIST_DIAG frame=%d team=%d dirHist[%u]: before=0x%016llX after=0x%016llX h590=0x%08X h594=0x%08X cntr=%llu\n",
            //        g_resimulateCurrentFrame, teamIdx, slotIdx,
            //        dirHistSlot_before, dirHistSlot, head590, head594, frameCnt);

            uint32_t dirMask = (uint32_t)(dirHistSlot >> 32);
            uint32_t dirMaskBefore = (uint32_t)(dirHistSlot_before >> 32);
            if (dirMask != dirMaskBefore) {
            //    HookLog("DIRHIST_DIAG   direction mask: 0x%04X \u2192 0x%04X\n", dirMaskBefore, dirMask);
            }
        }
    }
}

void Diag_ResetBuffers()
{
    std::memset(g_liveFrameSnap,  0, sizeof(g_liveFrameSnap));
    std::memset(g_resimFrameSnap, 0, sizeof(g_resimFrameSnap));
    std::memset(g_diagLiveCharCount, 0, sizeof(g_diagLiveCharCount));
    g_resimCharCounter = 0;
}

void Diag_InitHooks()
{
    MH_STATUS s;

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_CHARACTER_PRE_MOVE),
        (LPVOID)&hkCharacterPreMove,
        (LPVOID*)&oCharacterPreMove
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_CHARACTER_PRE_MOVE));
    }
    else
        HookLog("Diag: Failed to hook uCharacterPre__move (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_CHARACTER_MOVE_BASE),
        (LPVOID)&hkCharacterMoveBase,
        (LPVOID*)&oCharacterMoveBase
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_CHARACTER_MOVE_BASE));
    }
    else
        HookLog("Diag: Failed to hook uCharacter__move_base (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_ACTION_SIDE_CAMERA_MOVE),
        (LPVOID)&hkActionSideCameraMove,
        (LPVOID*)&oActionSideCameraMove
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_ACTION_SIDE_CAMERA_MOVE));
    }
    else
        HookLog("Diag: Failed to hook uActionSideCamera__move (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS),
        (LPVOID)&hkInputMaskPostProcess,
        (LPVOID*)&oInputMaskPostProcess
    );
    if (s == MH_OK)
    {
        //MH_EnableHook((LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS));
    }
    else
        HookLog("Diag: Failed to hook inputMaskPostProcess (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_TEAM_COMMIT_SWAP),
        (LPVOID)&hkTeamCommitSwap,
        (LPVOID*)&oTeamCommitSwap
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_TEAM_COMMIT_SWAP));
    }
    else
        HookLog("Diag: Failed to hook cChrTeam__commitPendingCharacterSwap (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_TEAM_UPDATE_AGGREGATE),
        (LPVOID)&hkTeamUpdateAggregate,
        (LPVOID*)&oTeamUpdateAggregate
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_TEAM_UPDATE_AGGREGATE));
    }
    else
        HookLog("Diag: Failed to hook cChrTeam__updateAggregateState (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_ACTION_FIGHTING_CORE),
        (LPVOID)&hkActionFightingCore,
        (LPVOID*)&oActionFightingCore
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_ACTION_FIGHTING_CORE));
    }
    else
        HookLog("Diag: Failed to hook sAction__stepFightingCore (%d)\n", s);

    HookLog("Diag: Init complete — diagnostic hooks installed\n");
}
