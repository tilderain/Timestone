#pragma once
#include <cstdint>

extern void HookLog(const char* fmt, ...);

static constexpr int ROLLBACK_WINDOW_DIAG = 16;
static constexpr int MAX_CHARS_PER_TEAM = 4;
static constexpr int CCHRINPUT_SIZE = 0x5A0;
static constexpr int ANIM_FIELD_COUNT = 16;
static constexpr int XORSHIFT_SIZE = 4;

struct CharDiagSnapshot {
    uint8_t cchr[CCHRINPUT_SIZE];
    float posX, posY, posZ;
    uint32_t actionId;
    uint32_t flags14F0;
    uint32_t flags14F4;
    uint32_t flags14F8;
    uint32_t flags1504;
    uint32_t flags1510;
    uint32_t flags1514;
    uint32_t flags1524;
    uint32_t animFields[ANIM_FIELD_COUNT];
    uint64_t uCharAddr;
    uint32_t teamIdx;
    uint32_t charSlot;
    uint32_t xorShift[XORSHIFT_SIZE];
};

struct CharSnapshotPair {
    CharDiagSnapshot pre;
    CharDiagSnapshot post;
};

struct FrameDiagSnapshot {
    int32_t frame;
    int charCount;
    CharSnapshotPair chars[6];
};

struct LiveFrameRecord {
    uint32_t mHeld;
    uint32_t m18;
    uint32_t m1C;
    uint32_t mRemapped;
    uint32_t mMask;
    uint32_t mProcessed;
    uint64_t frameCnt;
    uint32_t h590;
    uint32_t h594;
    uint64_t heldR0;
    uint32_t relHead;
    uint64_t relR0;
    uint64_t prsR0;
    uint32_t dir0;
    uint32_t dir4;
    uint64_t dirRing0;
    uint64_t dirRingSlot;
    uint64_t dirRing1;
    uint64_t dirRing2;
    uint32_t actionId;
    uint32_t flags14F0;
    float posX;
    float posY;
    float posZ;
};

extern FrameDiagSnapshot g_liveFrameSnap[ROLLBACK_WINDOW_DIAG];
extern FrameDiagSnapshot g_resimFrameSnap[ROLLBACK_WINDOW_DIAG];
extern int g_resimCharCounter;
extern int g_diagLiveCharCount[ROLLBACK_WINDOW_DIAG][2];

void DiagLogPreOverride(uint64_t cChrInput, uint32_t teamIdx);
void DiagLogPostOverride(uint64_t cChrInput, uint32_t teamIdx);
void DiagCaptureLiveRef(uint64_t cChrInput, uint32_t teamIdx, int32_t liveFrame);
void DiagLogPostResim(uint64_t cChrInput, uint32_t teamIdx, int32_t frame);
void DiagLogActDbg(uint64_t cChrInput, uint32_t teamIdx, int32_t curFrame, bool isResim);

void CaptureCharSnapshot(uint64_t uChar, uint32_t teamIdx, uint32_t charSlot, CharDiagSnapshot& snap);

extern LiveFrameRecord g_liveRef[2][256];

void Diag_InitHooks();
void Diag_ResetBuffers();
