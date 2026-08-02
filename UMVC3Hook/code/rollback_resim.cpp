#include "../pch.h"

#include "rollback.h"
#include "rollback_cleanup.h"
#include "rollback_hooks.h"
#include "rollback_profile.h"
#include "rollback_resim.h"
#include "rollback_snapshot.h"
#include "rollback_tick.h"
#include "rollback_ndjson.h"
#include "controller.h"
#include "autotest.h"
#include "mk10utils.h"
#include "netplay.h"

#include <string>

extern void HookLog(const char* fmt, ...);
extern bool g_multiResimEnabled;
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" bool FrameStepper_IsMatchRestartPending();

static_assert(sizeof(RollbackRegion) <= 64);

bool g_isResimulating = false;
int32_t g_resimulateCurrentFrame = 0;
int32_t g_postResimFrame = -1;
int32_t g_resimReentranceDepth = 0;
int32_t g_restoreRequestedFrame = -1;
int g_f7State = 0;
int32_t g_f7StartFrame = 0;
int g_f7Remaining = 0;
int g_f7DiagSession = -1;

static constexpr uintptr_t PTR_SREPLAY = 0x140d4d9d8;
static constexpr uintptr_t PTR_SMAIN = 0x140e177e8;
static int32_t g_requestStart = -1;
static int32_t g_requestEnd = -1;
static uint64_t g_mainInstance = 0;
static RollbackMainMoveFn g_originalMainMove = nullptr;

static bool CanReuseRestoredSnapshot(int32_t frame)
{
    int slotIndex = frame % ROLLBACK_WINDOW;
    if (slotIndex < 0) slotIndex += ROLLBACK_WINDOW;
    const RollbackSnapshotSlot& slot = g_rollbackSnapshotSlots[slotIndex];
    if (slot.frame != frame || !slot.data || slot.size == 0 ||
        !slot.shotRecipes.empty() || !slot.effectRecipes.empty()) {
        return false;
    }

    for (int i = 0; i < slot.regionCount; ++i) {
        const RollbackRegion& region = slot.regions[i];
        if (region.ownerSlot && *(const uint64_t*)region.ownerSlot != (uint64_t)region.ptr)
            return false;
        if (region.expectedVtable &&
            *(const uint64_t*)region.ptr != region.expectedVtable) {
            return false;
        }
    }
    return true;
}

extern "C" void Controller_InjectLocalInput(int32_t, uint32_t);
extern "C" void Controller_InjectRemoteInput(int32_t, uint32_t);
extern "C" void Controller_SilentFrameStart();
extern "C" void Controller_SilentFrameEnd();
extern "C" void Controller_SaveInputHistory();
extern "C" void Controller_RestoreInputHistory();
extern "C" void Controller_DumpOldInputs(const char*);
extern "C" void Controller_ResolveRemoteInput(int32_t);
extern "C" void Rollback_UpdateSnapshotInputs(int32_t, uint32_t, uint32_t);

void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (Netplay_IsActive() &&
        (FrameStepper_IsMatchRestartPending() ||
         !FrameStepper_IsBattleSynchronized())) {
        HookLog("Rollback: dropped re-simulation request during match reset "
                "[%d,%d)\n", startFrame, endFrame);
        return;
    }
    g_requestStart = startFrame;
    g_requestEnd = endFrame;
}

bool Rollback_ConsumeReSimRequest(int32_t& startFrame, int32_t& endFrame)
{
    if (Netplay_IsActive() &&
        (FrameStepper_IsMatchRestartPending() ||
         !FrameStepper_IsBattleSynchronized())) {
        Rollback_ClearPendingReSimulation();
        return false;
    }
    if (g_requestStart < 0) return false;
    startFrame = g_requestStart;
    endFrame = g_requestEnd;
    g_requestStart = -1;
    g_requestEnd = -1;
    return true;
}

void Rollback_ClearPendingReSimulation()
{
    g_requestStart = -1;
    g_requestEnd = -1;
    g_restoreRequestedFrame = -1;
    g_f7State = 0;
    g_f7Remaining = 0;
    g_f7DiagSession = -1;
}

void Rollback_SetMainInstance(uint64_t main)
{
    if (!g_mainInstance) g_mainInstance = main;
}

void Rollback_InitReSimulationHooks()
{
    Rollback_InitMainHooks(&g_originalMainMove);
}

void Rollback_ResetReSimulation()
{
    g_mainInstance = 0;
    g_isResimulating = false;
    g_resimulateCurrentFrame = 0;
    g_postResimFrame = -1;
    g_resimReentranceDepth = 0;
    Rollback_ClearPendingReSimulation();
    g_f7StartFrame = 0;
}

void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;
    if (Netplay_IsActive() &&
        (FrameStepper_IsMatchRestartPending() ||
         !FrameStepper_IsBattleSynchronized())) {
        HookLog("Rollback: blocked re-simulation during match reset "
                "[%d,%d)\n", startFrame, endFrame);
        Rollback_ClearPendingReSimulation();
        return;
    }
    if (g_isResimulating) {
        ++g_resimReentranceDepth;
        HookLog("Rollback: ignoring nested re-simulation depth=%d\n",
                g_resimReentranceDepth);
        return;
    }
    if (!g_mainInstance) {
        HookLog("Rollback: cannot re-simulate without sMvc3Main\n");
        return;
    }
    g_resimReentranceDepth = 0;

    RollbackProfile profile;
    Profile_BeginRollback(profile, startFrame, endFrame);
    const uint64_t profileStart = Profile_Now();
    g_isResimulating = true;

    uint64_t phaseStart = Profile_Now();
    Rollback_PatchSleep(true);
    Rollback_PatchPresent(true);
    Rollback_PatchSwapchain(true);
    Rollback_PatchDrawCalls(true);
    Profile_AddElapsed(&RollbackProfile::patchEnable, phaseStart);

    phaseStart = Profile_Now();
    Controller_SaveInputHistory();
    Profile_AddElapsed(&RollbackProfile::inputHistorySave, phaseStart);

    phaseStart = Profile_Now();
    if (g_f7DiagSession < 0)
        (void)NDJSON_BeginSession();
    Profile_AddElapsed(&RollbackProfile::diagSetup, phaseStart);

    int32_t liveFrame = -1;
    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (replay) liveFrame = *(int32_t*)(replay + 0x54);
    phaseStart = Profile_Now();
    if (liveFrame >= 0 && Netplay_IsActive()) {
        std::string json = NDJSON_GenerateNetplayLive(
            liveFrame, false, "pre_restore_prediction", startFrame, endFrame);
        if (!json.empty()) NDJSON_WriteNetplayLive(json);
    }
    Profile_AddElapsed(&RollbackProfile::preRestoreJson, phaseStart);

    Rollback_RestoreSnapshot(startFrame);
    const bool reuseStartSnapshot = CanReuseRestoredSnapshot(startFrame);

    phaseStart = Profile_Now();
    Controller_LogReceivedInputAvailability(startFrame, endFrame);
    Profile_AddElapsed(&RollbackProfile::inputAvailability, phaseStart);

    for (int32_t frame = startFrame; frame < endFrame; ++frame) {
        RollbackProfileTimer timer;
        g_resimulateCurrentFrame = frame;
        int slotIndex = frame % ROLLBACK_WINDOW;
        if (slotIndex < 0) slotIndex += ROLLBACK_WINDOW;
        uint32_t localInput = g_rollbackSnapshotSlots[slotIndex].localInput;
        uint32_t remoteInput = g_rollbackSnapshotSlots[slotIndex].remoteInput;
        if (Netplay_IsActive()) {
            Controller_ResolveRemoteInput(frame);
            remoteInput = Controller_GetUsedRemoteInput(frame);
        }
        Rollback_UpdateSnapshotInputs(frame, localInput, remoteInput);
        Controller_SilentFrameStart();
        if(!Netplay_IsActive()) Controller_InjectLocalInput(frame, localInput);
        Controller_InjectRemoteInput(frame, remoteInput);
        timer.Add(&RollbackProfile::frameInput);

        NDJSON_CaptureFrame(frame, true, false);
        timer.Add(&RollbackProfile::frameDiagPre);
        if (Netplay_IsActive()) {
            NDJSON_DiscardPendingNetplayLive(frame);
            std::string json = NDJSON_GenerateNetplayLive(frame, true);
            if (!json.empty()) NDJSON_WriteNetplayLive(json);
        } else {
            NDJSON_DumpFrame(frame, 1);
        }
        timer.Add(&RollbackProfile::frameJson);
        if (g_multiResimEnabled && (frame != startFrame || !reuseStartSnapshot)) {
            Rollback_SaveSnapshot(frame);
            Rollback_UpdateSnapshotInputs(frame, localInput, remoteInput);
        }
        timer.Add(&RollbackProfile::frameSnapshot);
        //g_originalMainMove(g_mainInstance);
        Rollback_HeadlessTick(g_mainInstance);
        timer.Add(&RollbackProfile::frameNativeTick,
                  &RollbackProfile::frameNativeTickMax);
        Controller_SilentFrameEnd();
        if (Netplay_IsActive()) {
            Controller_RecordFrameChecksum(frame);
            uint32_t checksum = 0;
            if (Controller_GetFrameChecksum(frame, &checksum))
                AutoTest_OnResimFrame(frame, checksum);
        }
        Controller_LogReSimFrame(frame);
        timer.Add(&RollbackProfile::frameInputEnd);
        NDJSON_CaptureFrame(frame, true, true);
        NDJSON_QueuePendingResimDump(frame);
        timer.Add(&RollbackProfile::frameDiagPost);
        timer.Finish();
    }

    Controller_DumpOldInputs("post-loop-pre-restore");

    phaseStart = Profile_Now();
    Controller_RestoreInputHistory();
    Profile_AddElapsed(&RollbackProfile::inputHistoryRestore, phaseStart);
    uint64_t main = *(uint64_t*)_addr(PTR_SMAIN);
    phaseStart = Profile_Now();
    if (main) Rollback_ResyncMainTiming(main);
    Profile_AddElapsed(&RollbackProfile::timingResync, phaseStart);
    g_isResimulating = false;

    // A rollback can begin after cChrTeam::active changed but before the model
    // callback made the incoming point visible. Normalize both current point
    // characters unless a live cut-in actually owns their render state.
    phaseStart = Profile_Now();
    Rollback_SyncActiveCharacterVisibility();
    Profile_AddElapsed(&RollbackProfile::cutinRepair, phaseStart);

    phaseStart = Profile_Now();
    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
    Profile_AddElapsed(&RollbackProfile::patchDisable, phaseStart);
    Profile_EndRollback(profile, profileStart);
    Profile_Dump(profile, g_ndjsonLoggingEnabled, g_multiResimEnabled);
}
