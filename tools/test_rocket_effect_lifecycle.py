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


class RocketEffectLifecycleTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_short_factory_restore_and_owner_cleanup(self):
        root = Path(__file__).resolve().parents[1] / "UMVC3Hook/code"
        source = (root / "rollback_effect.cpp").read_text()
        snapshot = (root / "rollback_snapshot.h").read_text()
        restore = source[source.index("int Effect_RestoreRecipes("):]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define __fastcall
#define _addr(address) (address)
constexpr uintptr_t VTABLE_URROCKET_RACCOON = 0x140A7DF30;
constexpr uint32_t ROCKET_JETPACK_OWNER_OFFSETS[] = {0x68F8, 0x6900, 0x6930};
struct ConcreteShotSnapshot {
    struct CallbackEffectState {
        bool hasState = true;
        uint8_t unitState = 1;
        uint64_t resource = 0;
        bool CompletedIndependent() const { return hasState && unitState != 1 && unitState != 2; }
    };
};
''' + block(snapshot, "struct RollbackEffectRecipe {") + ";\n" + block(
            snapshot, "enum class RollbackCharacterEffectReferenceKind") + ";\n" + block(
            snapshot, "struct RollbackCharacterEffectReference {") + r''';
struct RollbackSnapshotSlot {
    std::vector<RollbackEffectRecipe> effectRecipes;
    std::vector<RollbackCharacterEffectReference> characterEffectReferences;
};
std::vector<RollbackEffectRecipe> g_liveEffectRecipes;
uint64_t g_effectSpawnSequence = 0;
alignas(16) uint8_t owner[0x6970] = {};
alignas(16) uint8_t effect[0x2C0] = {};
uint64_t vtable[32] = {};
bool failFactory = false, failUnit = false;
unsigned registered = 0, retired = 0, timelines = 0, attached = 0;
void* Factory(void*, int, int, void*, int, int, int, void*) {
    if (failFactory) return nullptr;
    std::memset(effect, 0, sizeof(effect));
    *(uint64_t*)effect = (uint64_t)vtable;
    return effect;
}
void* (*oEffectCreateShort)(void*, int, int, void*, int, int, int, void*) = Factory;
void* GetUnit() { return failUnit ? nullptr : owner; }
void AddBottom(void* unit, uint32_t category, void* object, void* mask) {
    assert(unit == owner && category == 13 && object == effect && !mask);
    ++registered;
}
void Attach(void* object, void* parent, uint32_t joint) {
    assert(object == effect && parent == owner && joint == 0x17);
    ++attached;
}
void Retire(void* object) { assert(object == effect); ++retired; }
uintptr_t FN_SUNIT_GET_INSTANCE = (uintptr_t)GetUnit;
uintptr_t FN_SUNIT_ADD_BOTTOM = (uintptr_t)AddBottom;
void InvalidateMemoryQueryCache() {}
void HookLog(const char*, ...) {}
bool ConcreteEffectSnapshot_Restore(void* object,
    const ConcreteShotSnapshot::CallbackEffectState&, bool = true) {
    assert(object == effect); ++timelines; return true;
}
''' + block(source, "static bool IsRocketShortEffectReferenced(") + "\n" + block(
            source, "static void* __fastcall hkEffectCreateShort(") + "\n" + block(
            source, "void Effect_RestoreCharacterReferences(") + r'''
int Restore(RollbackSnapshotSlot& slot) {
    int restored = 0, failed = 0;
    for (auto& saved : slot.effectRecipes) {
        void* sGameEffect = saved.sGameEffect;
        void* attachTo = saved.attachTo;
''' + block(restore, "if (saved.rocketShort) {") + r'''
    }
    return failed ? -1 : restored;
}
int main() {
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    vtable[0x88 / 8] = (uint64_t)Attach;
    vtable[0x78 / 8] = (uint64_t)Retire;
    for (int id : {0x17, 0x29}) {
        hkEffectCreateShort(owner, 100, id, owner, 0x100, -1, 3, nullptr);
        assert(g_liveEffectRecipes.back().rocketShort);
        assert(g_liveEffectRecipes.back().a3 == id);
    }
    assert(g_liveEffectRecipes.size() == 2 && g_effectSpawnSequence == 2);
    hkEffectCreateShort(owner, 100, 0x18, owner, 0, -1, 3, nullptr);
    hkEffectCreateShort(owner, 99, 0x17, owner, 0, -1, 3, nullptr);
    *(uint64_t*)owner = 0;
    hkEffectCreateShort(owner, 100, 0x17, owner, 0, -1, 3, nullptr);
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    failFactory = true;
    hkEffectCreateShort(owner, 100, 0x17, owner, 0, -1, 3, nullptr);
    failFactory = false;
    assert(g_liveEffectRecipes.size() == 2);

    auto recipe = g_liveEffectRecipes[0];
    for (uint32_t offset : ROCKET_JETPACK_OWNER_OFFSETS) {
        *(void**)(owner + offset) = effect;
        assert(IsRocketShortEffectReferenced(recipe));
        *(void**)(owner + offset) = nullptr;
        assert(!IsRocketShortEffectReferenced(recipe));
    }
    recipe.a8 = owner;
    recipe.shortParentNo = 0x17;
    recipe.shortControlState = 0x00000010200028C0;
    RollbackSnapshotSlot slot;
    slot.effectRecipes.push_back(recipe);
    assert(Restore(slot) == 1);
    assert(registered == 1 && timelines == 1 && retired == 0 && attached == 1);
    assert(*(void**)(effect + 0x280) == owner);
    assert(*(uint64_t*)(effect + 0x290) == recipe.shortControlState);

    // A dead-but-occupied owner must be recreated dead, not dropped or revived.
    slot.effectRecipes[0].state.unitState = 3;
    assert(Restore(slot) == 1);
    assert(registered == 2 && retired == 1 && timelines == 1);
    RollbackCharacterEffectReference reference{};
    reference.character = owner;
    reference.expectedCharacterVtable = VTABLE_URROCKET_RACCOON;
    reference.offset = 0x6900;
    reference.kind = RollbackCharacterEffectReferenceKind::Reconstructed;
    reference.effectSpawnSequence = recipe.spawnSequence;
    slot.characterEffectReferences.push_back(reference);
    Effect_RestoreCharacterReferences(slot);
    assert(*(void**)(owner + 0x6900) == effect);
    slot.characterEffectReferences[0].kind = RollbackCharacterEffectReferenceKind::None;
    Effect_RestoreCharacterReferences(slot);
    assert(*(void**)(owner + 0x6900) == nullptr);

    failFactory = true;
    slot.effectRecipes[0].restoredPtr = nullptr;
    assert(Restore(slot) == -1 && !slot.effectRecipes[0].restoredPtr);
    assert(registered == 2);
    failFactory = false;
    failUnit = true;
    assert(Restore(slot) == -1 && registered == 2);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "rocket_effect_lifecycle")
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-x",
                            "c++", "-", "-o", executable], input=harness,
                           text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
