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
static constexpr uintptr_t ADDR_INPUT_MASK_POST_PROCESS  = 0x14002E5A0;
static constexpr uintptr_t ADDR_GET_TEAM_INDEX           = 0x140004AC0;

static constexpr uintptr_t PTR_SCHARACTER  = 0x140d44a70;
static constexpr uintptr_t TEAM0_OFFSET    = 0x40;
static constexpr uintptr_t TEAM1_OFFSET    = 0x300;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;

static int32_t Diag_GetGameFrameCount()
{
    static constexpr uintptr_t G_PSREPLAY_PTR = 0x140d4d9d8;
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (!sReplay) return -1;
    return *(int32_t*)(sReplay + 0x54);
}

static void(__fastcall* oCharacterMoveBase)(uint64_t uChar) = nullptr;
static void(__fastcall* oInputMaskPostProcess)(uint64_t cChrInput) = nullptr;

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

static void LogSnapshotDiff(const CharDiagSnapshot& live, const CharDiagSnapshot& resim,
                            int frame, int teamIdx, const char* phase)
{
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
}

void DiagLogPostResim(uint64_t cChrInput, uint32_t teamIdx, int32_t frame)
{
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

    oCharacterMoveBase(uChar);

    CharDiagSnapshot postSnap;
    CaptureCharSnapshot(uChar, teamIdx, charSlot, postSnap);

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
            HookLog("DIRHIST_DIAG frame=%d team=%d dirHist[%u]: before=0x%016llX after=0x%016llX h590=0x%08X h594=0x%08X cntr=%llu\n",
                    g_resimulateCurrentFrame, teamIdx, slotIdx,
                    dirHistSlot_before, dirHistSlot, head590, head594, frameCnt);

            uint32_t dirMask = (uint32_t)(dirHistSlot >> 32);
            uint32_t dirMaskBefore = (uint32_t)(dirHistSlot_before >> 32);
            if (dirMask != dirMaskBefore) {
                HookLog("DIRHIST_DIAG   direction mask: 0x%04X \u2192 0x%04X\n", dirMaskBefore, dirMask);
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
        (LPVOID)_addr(ADDR_CHARACTER_MOVE_BASE),
        (LPVOID)&hkCharacterMoveBase,
        (LPVOID*)&oCharacterMoveBase
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_CHARACTER_MOVE_BASE));
    else
        HookLog("Diag: Failed to hook uCharacter__move_base (%d)\n", s);

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS),
        (LPVOID)&hkInputMaskPostProcess,
        (LPVOID*)&oInputMaskPostProcess
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS));
    else
        HookLog("Diag: Failed to hook inputMaskPostProcess (%d)\n", s);

    HookLog("Diag: Init complete — diagnostic hooks installed\n");
}
