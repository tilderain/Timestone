import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class RestoredShotPoseTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_track_setup_and_saved_pose_without_frame_advance(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_model_policy.cpp").read_text()
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>
#define __fastcall
#define _addr(address) (address)
constexpr uintptr_t DTI_USHOT_VULCAN_DOG_MISSILE = 0x140D4B888;
constexpr uintptr_t DTI_USHOT_VULCAN_PRICKLE_BOARD = 0x140D4C528;
constexpr uintptr_t DTI_USHOT_VULCAN_ITEM_THROW = 0x140D4B980;
constexpr uintptr_t DTI_USHOT_VULCAN_LEGION_TIGER = 0x140D4ABB8;
alignas(16) uint8_t shot[0x2150];
alignas(16) uint8_t model[0x100], joints[8 * 0xC0], jointWork[0x200];
uint64_t vtable[32] = {};
unsigned setups = 0, samples = 0, worldUpdates = 0, childUpdates = 0;
float sampledFrame = -1;
bool bindSucceeded = true;
void Setup(void* object) {
    assert(object == shot && (*(uint16_t*)(shot + 0x56A) & 2));
    assert(*(uint64_t*)(shot + 0x100) && *(uint32_t*)(shot + 0x530));
    ++setups;
    std::memset(shot + 0x568, 0, 0xD8);
    std::memset(shot + 0x550, 0, 0x10);
    for (size_t offset : {0x640, 0x660, 0x680, 0x688, 0x690, 0x698})
        *(uint64_t*)(shot + offset) = bindSucceeded ? 0x12340000 + offset : 0;
    // Fresh per-joint tracks must be initialized before sampling.
    shot[0x500] = 1;
}
void Sample(void* object) {
    assert(object == shot && shot[0x500] == 1);
    ++samples;
    sampledFrame = *(float*)(shot + 0x5AC);
    // Native sampling can change derived playback/root values.
    *(float*)(shot + 0x5C4) = sampledFrame;
    *(float*)(shot + 0x5B0) = sampledFrame;
    std::memset(shot + 0x1B0, 0, 0x70);
    shot[0x500] = 2;
}
void World(void* object) {
    assert(object == shot && samples == 1 && shot[0x1B0] == 0x5A);
    ++worldUpdates;
}
void Children(void* object) {
    assert(object == shot && worldUpdates == 1);
    ++childUpdates;
}
uintptr_t FN_UMODEL_SETUP_MOTION = (uintptr_t)Setup;
uintptr_t FN_UMODEL_UPDATE_MOTION_EVALUATION = (uintptr_t)Sample;
''' + block(source, "bool PrimaryMotionBindingsReady(void* shot)\n{") + "\n" + block(
            source, "void Rollback_EvaluateRestoredShotPose(") + r'''
bool g_isResimulating = true;
std::unordered_set<void*> g_resimSpawnedPendulums;
struct RollbackShotRecipe { bool reconstructed = false; };
uint64_t observedDti = 0x140D4C4C8;
unsigned rebinds = 0;
uint64_t GetObjectDti(uint64_t) { return observedDti; }
void ProcessPendulumMotionIfReady(void*) { ++rebinds; }
void EvaluatePendulumPose(void*) { ++rebinds; }
void LoadPendulumMotionSynchronously(void*) {}
void InvalidateMemoryQueryCache() {}
void Effect_RetireOne(void*, void*) {}
uint64_t unit = 0;
uintptr_t PTR_SUNIT = (uintptr_t)&unit;
''' + block(source, "void RepairResimPendulumModel(") + "\n" + block(
            source, "struct ShotModelPolicy {") + ";\n" + block(
            source, "constexpr ShotModelPolicy SHOT_MODEL_POLICIES[] =") + ";\n" + block(
            source, "const ShotModelPolicy* FindShotModelPolicy(") + r'''
unsigned nativeSetups = 0, sleeps = 0, readyAfter = 1;
bool IsAccessibleRange(uint64_t address, size_t size) {
    return address == (uint64_t)joints && size <= sizeof(joints);
}
void Sleep(unsigned milliseconds) { assert(milliseconds == 1); ++sleeps; }
void HookLog(const char*, ...) {}
void NativeSetup(void* object) {
    assert(object == shot);
    ++nativeSetups;
    if (nativeSetups < readyAfter) return;
    *(uint64_t*)(shot + 0x100) = (uint64_t)model;
    *(uint32_t*)(shot + 0x530) = 8;
    *(uint64_t*)(shot + 0x538) = (uint64_t)joints;
    *(uint64_t*)(shot + 0x11B0) = (uint64_t)jointWork;
    // Subclass setup selects its initial motion/root, before the saved overlay.
    *(uint16_t*)(shot + 0x568) = UINT16_MAX;
    std::memset(shot + 0x1B0, 0, 0x70);
}
''' + block(source, "bool IsShotModelReady(") + "\n" + block(
            source, "bool RetryShotModelSetup(") + "\n" + block(
            source, "void Rollback_PrepareShotModelAfterSpawn(") + r'''
void Reset(uint16_t motion) {
    std::memset(shot, 0, sizeof(shot));
    *(uint64_t*)shot = (uint64_t)vtable;
    *(uint64_t*)(shot + 0x100) = (uint64_t)model;
    *(uint32_t*)(shot + 0x530) = 8;
    *(uint64_t*)(shot + 0x538) = (uint64_t)joints;
    *(uint64_t*)(shot + 0x11B0) = (uint64_t)jointWork;
    *(uint32_t*)(shot + 0x550) = 1;
    *(uint32_t*)(shot + 0x554) = 4;
    *(uint32_t*)(shot + 0x558) = 0x20000044;
    *(uint16_t*)(shot + 0x568) = motion;
    *(uint16_t*)(shot + 0x56A) = 1;
    *(uint32_t*)(shot + 0x570) = 0x44;
    *(float*)(shot + 0x5AC) = 22;
    *(float*)(shot + 0x5B0) = 21;
    *(float*)(shot + 0x5C4) = 21;
    std::memset(shot + 0x1B0, 0x5A, 0x70);
    setups = samples = worldUpdates = childUpdates = rebinds = 0;
    sampledFrame = -1;
    bindSucceeded = true;
}
void ClearFreshModel() {
    *(uint64_t*)(shot + 0x100) = 0;
    *(uint32_t*)(shot + 0x530) = 0;
    *(uint64_t*)(shot + 0x538) = 0;
    *(uint64_t*)(shot + 0x11B0) = 0;
}
int main() {
    vtable[0x28 / 8] = (uint64_t)NativeSetup;
    vtable[0x90 / 8] = (uint64_t)World;
    vtable[0x98 / 8] = (uint64_t)Children;
    for (uint64_t dti : {DTI_USHOT_VULCAN_DOG_MISSILE,
                         DTI_USHOT_VULCAN_PRICKLE_BOARD,
                         uintptr_t(0x140D4C4C8)}) {
        for (uint16_t motion : {uint16_t(0), uint16_t(1), uint16_t(0xCB)}) {
            Reset(motion);
            uint8_t savedMotion[0x180], savedRoot[0x70], savedModel[0x10];
            std::memcpy(savedMotion, shot + 0x560, sizeof(savedMotion));
            std::memcpy(savedRoot, shot + 0x1B0, sizeof(savedRoot));
            std::memcpy(savedModel, shot + 0x550, sizeof(savedModel));
            Rollback_EvaluateRestoredShotPose(shot, dti);
            assert(setups == 1 && samples == 1 && worldUpdates == 1 && childUpdates == 1);
            assert(sampledFrame == 21 && shot[0x500] == 2);
            assert(std::memcmp(shot + 0x1B0, savedRoot, sizeof(savedRoot)) == 0);
            assert(std::memcmp(shot + 0x550, savedModel, sizeof(savedModel)) == 0);
            for (auto range : {std::pair<size_t, size_t>{0x08, 0xD8},
                               {0xE8, 0x18}, {0x108, 0x18},
                               {0x140, 0x20}, {0x168, 0x08}})
                assert(std::memcmp(shot + 0x560 + range.first,
                                   savedMotion + range.first, range.second) == 0);
            for (size_t offset : {0x640, 0x660, 0x680, 0x688, 0x690, 0x698})
                assert(*(uint64_t*)(shot + offset) == 0x12340000 + offset);
            RollbackShotRecipe recipe{true};
            RepairResimPendulumModel(shot, &recipe);
            assert(rebinds == 0); // Ready reconstruction is not restarted at hitbox time.
        }
    }
    Reset(0);
    Rollback_EvaluateRestoredShotPose(shot, 0x999);
    Rollback_EvaluateRestoredShotPose(nullptr, DTI_USHOT_VULCAN_PRICKLE_BOARD);
    assert(setups == 0 && samples == 0);
    Reset(UINT16_MAX);
    Rollback_EvaluateRestoredShotPose(shot, DTI_USHOT_VULCAN_PRICKLE_BOARD);
    assert(setups == 0 && samples == 0);
    Reset(0);
    bindSucceeded = false;
    Rollback_EvaluateRestoredShotPose(shot, DTI_USHOT_VULCAN_PRICKLE_BOARD);
    assert(setups == 1 && samples == 0 && worldUpdates == 0);

    for (uint64_t dti : {DTI_USHOT_VULCAN_PRICKLE_BOARD, uintptr_t(0x140D4C4C8)}) {
        std::vector<uintptr_t> waited;
        for (unsigned object = 0; object < 2; ++object) {
            Reset(0);
            // Hitstop: the evaluated pose is 13, not the fallback's age - 1.
            *(float*)(shot + 0x5AC) = 13;
            *(float*)(shot + 0x5B0) = 13;
            *(float*)(shot + 0x5C4) = 13;
            uint8_t savedMotion[0x180], savedRoot[0x70], savedModel[0x10];
            std::memcpy(savedMotion, shot + 0x560, sizeof(savedMotion));
            std::memcpy(savedRoot, shot + 0x1B0, sizeof(savedRoot));
            std::memcpy(savedModel, shot + 0x550, sizeof(savedModel));
            ClearFreshModel();
            nativeSetups = sleeps = 0;
            readyAfter = object == 0 ? 3 : 1;
            Rollback_PrepareShotModelAfterSpawn(shot, dti, waited);
            assert(IsShotModelReady(shot));
            assert(nativeSetups == readyAfter && sleeps == readyAfter - 1);
            assert(waited.size() == 1 && waited[0] == dti);
            assert(*(uint16_t*)(shot + 0x568) == UINT16_MAX);
            // The production overlay runs after preparation, not before setup.
            std::memcpy(shot + 0x550, savedModel, sizeof(savedModel));
            std::memcpy(shot + 0x560, savedMotion, sizeof(savedMotion));
            std::memcpy(shot + 0x1B0, savedRoot, sizeof(savedRoot));
            Rollback_EvaluateRestoredShotPose(shot, dti);
            assert(samples == 1 && sampledFrame == 13);
            assert(*(float*)(shot + 0x5AC) == 13 && *(float*)(shot + 0x5C4) == 13);
            RollbackShotRecipe recipe{true};
            RepairResimPendulumModel(shot, &recipe);
            assert(rebinds == 0);
            unsigned before = nativeSetups;
            Rollback_PrepareShotModelAfterSpawn(shot, dti, waited);
            assert(nativeSetups == before); // Ready model is not set up again.
        }
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="restored-shot-pose-") as tmp:
            cpp = Path(tmp) / "test.cpp"
            cpp.write_text(harness)
            exe = Path(tmp) / "test"
            subprocess.run(["g++", "-std=c++20", "-O2", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_pose_sampling_follows_pending_flush(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_restore.cpp").read_text()
        self.assertLess(source.index("flushPendingUnits(sShot);"),
                        source.index("Rollback_EvaluateRestoredShotPose("))
        self.assertLess(source.index("Rollback_PrepareShotModelAfterSpawn("),
                        source.index("ConcreteShotSnapshot_Restore(fresh,"))


if __name__ == "__main__":
    unittest.main()
