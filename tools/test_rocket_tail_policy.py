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


class RocketTailPolicyTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_animation_tail_and_hook_failure_passthrough(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_model_policy.cpp").read_text()
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define __fastcall
#define _addr(address) (address)
using LPVOID = void*;
using MH_STATUS = int;
constexpr MH_STATUS MH_OK = 0;
constexpr uintptr_t VTABLE_URROCKET_RACCOON = 0x140A7DF30;
constexpr uintptr_t FN_UCHRCHAIN_MOVE = 1;
constexpr uintptr_t FN_UCHRCHAIN_MOVE_AFTER = 2;
constexpr uintptr_t FN_MISSILE_EMPLACEMENT_MOVE = 3;
constexpr uintptr_t FN_WEAPON_CART_MOVE = 4;
alignas(16) uint8_t owner[0x20] = {};
alignas(16) uint8_t chain[0x1328] = {};
unsigned moves = 0, afters = 0, creates = 0, enables = 0;
unsigned failCreate = 0, failEnable = 0;
float tailPosition = 0;
bool g_isResimulating = false;
bool g_disableRocketTail = false;
using ShotMoveFn = void(*)(void*);
void NativeMove(void* object) { assert(object == chain); ++moves; }
void NativeAfter(void* object) {
    assert(object == chain); ++afters;
    tailPosition = 999; // Simulated chain output replaces the animation pose.
}
ShotMoveFn oChrChainMove = nullptr, oChrChainMoveAfter = nullptr;
ShotMoveFn oMissileEmplacementMove = nullptr, oWeaponCartMove = nullptr;
void hkMissileEmplacementMove(void*) {}
void hkWeaponCartMove(void*) {}
void HookLog(const char*, ...) {}
MH_STATUS MH_CreateHook(LPVOID address, LPVOID, void** original) {
    ++creates;
    if (creates == failCreate) return 1;
    *original = address == (LPVOID)FN_UCHRCHAIN_MOVE_AFTER
        ? (void*)NativeAfter : (void*)NativeMove;
    return MH_OK;
}
MH_STATUS MH_EnableHook(LPVOID) {
    ++enables;
    return enables == failEnable ? 1 : MH_OK;
}
''' + "\n".join(block(source, marker) for marker in [
            "bool ShouldSkipRocketTail(",
            "void __fastcall hkChrChainMove(",
            "void __fastcall hkChrChainMoveAfter(",
            "bool InstallPolicyHook(",
            "void Rollback_InitShotModelPolicyHooks(",
        ]) + r'''
void Install(unsigned createFailure = 0, unsigned enableFailure = 0) {
    creates = enables = 0;
    failCreate = createFailure;
    failEnable = enableFailure;
    oChrChainMove = oChrChainMoveAfter = nullptr;
    Rollback_InitShotModelPolicyHooks();
}
int main() {
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    *(uint32_t*)(owner + 0x10) = 2;
    *(void**)(chain + 0x1320) = owner;
    Install();
    assert(g_disableRocketTail && creates == 4 && enables == 4);
    for (unsigned frame = 0; frame < 100; ++frame) {
        g_isResimulating = frame % 2;
        tailPosition = float(frame); // Native character animation evaluation.
        hkChrChainMove(chain);
        hkChrChainMoveAfter(chain);
        assert(tailPosition == float(frame));
    }
    assert(moves == 0 && afters == 0);
    *(uint32_t*)(owner + 0x10) = 1;
    assert(ShouldSkipRocketTail(chain));
    for (uint32_t deadState : {0u, 3u, 4u}) {
        *(uint32_t*)(owner + 0x10) = deadState;
        assert(!ShouldSkipRocketTail(chain));
        hkChrChainMove(chain);
        hkChrChainMoveAfter(chain);
    }
    assert(moves == 3 && afters == 3); // Native dead-owner teardown remains reachable.
    *(uint32_t*)(owner + 0x10) = 2;
    *(uint64_t*)owner = 0x140A71F90; // Another character retains native jiggle.
    hkChrChainMove(chain);
    hkChrChainMoveAfter(chain);
    assert(moves == 4 && afters == 4 && tailPosition == 999);
    *(void**)(chain + 0x1320) = nullptr;
    hkChrChainMove(chain);
    hkChrChainMoveAfter(chain);
    assert(moves == 5 && afters == 5);
    assert(!ShouldSkipRocketTail(nullptr));
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    *(void**)(chain + 0x1320) = owner;
    for (unsigned phase : {1u, 2u}) {
        Install(phase, 0);
        assert(!g_disableRocketTail && !ShouldSkipRocketTail(chain));
        unsigned before = moves + afters;
        if (oChrChainMove) hkChrChainMove(chain);
        if (oChrChainMoveAfter) hkChrChainMoveAfter(chain);
        assert(moves + afters == before + 1);
        Install(0, phase);
        assert(!g_disableRocketTail && !ShouldSkipRocketTail(chain));
        before = moves + afters;
        hkChrChainMove(chain);
        hkChrChainMoveAfter(chain);
        assert(moves + afters == before + 2);
    }
    Install();
    assert(g_disableRocketTail && ShouldSkipRocketTail(chain));
}
'''
        with tempfile.TemporaryDirectory(prefix="rocket-tail-policy-") as tmp:
            cpp = Path(tmp) / "test.cpp"
            cpp.write_text("#include <initializer_list>\n" + harness)
            exe = Path(tmp) / "test"
            subprocess.run(["g++", "-std=c++20", "-O2", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
