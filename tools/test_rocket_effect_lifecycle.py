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
        capture = source[source.index("void Effect_CaptureRecipes("):]
        restore = source[source.index("int Effect_RestoreRecipes("):]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "rollback_character_restore.h"
#define __fastcall
#define _addr(address) ResolveAddress((uintptr_t)(address))
uintptr_t ResolveAddress(uintptr_t address);
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
    std::vector<void*> retainedEffectResources;
};
std::vector<RollbackEffectRecipe> g_liveEffectRecipes;
uint64_t g_effectSpawnSequence = 0;
alignas(16) uint8_t owner[0x6970] = {};
alignas(16) uint8_t effect[0x2C0] = {};
uint64_t vtable[32] = {};
bool failFactory = false, failUnit = false;
bool retainTestResource = false;
int resourceReferences = 0;
void AddRef(void* resource) { assert(resource == owner); ++resourceReferences; }
void ReleaseRef(void* resource) { assert(resource == owner); --resourceReferences; }
uintptr_t ResolveAddress(uintptr_t address) {
    if (address == 0x14050D100) return (uintptr_t)AddRef;
    if (address == 0x14050D5A0) return (uintptr_t)ReleaseRef;
    return address;
}
unsigned registered = 0, retired = 0, timelines = 0, attached = 0;
void* Factory(void*, int, int, void*, int, int, int, void*) {
    if (failFactory) return nullptr;
    std::memset(effect, 0, sizeof(effect));
    *(uint64_t*)effect = (uint64_t)vtable;
    *(uint32_t*)(effect + 0x10) = 1;
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
void Retire(void* object) {
    assert(object == effect); ++retired;
    *(uint32_t*)(effect + 0x10) = 3;
}
uintptr_t FN_SUNIT_GET_INSTANCE = (uintptr_t)GetUnit;
uintptr_t FN_SUNIT_ADD_BOTTOM = (uintptr_t)AddBottom;
void InvalidateMemoryQueryCache() {}
void HookLog(const char*, ...) {}
bool ConcreteEffectSnapshot_Restore(void* object,
    const ConcreteShotSnapshot::CallbackEffectState&, bool = true) {
    assert(object == effect); ++timelines; return true;
}
bool ConcreteEffectSnapshot_Capture(void* object,
    ConcreteShotSnapshot::CallbackEffectState& state) {
    state.hasState = true;
    state.unitState = *(uint32_t*)((uint8_t*)object + 0x10) & 7;
    state.resource = retainTestResource ? (uint64_t)owner : 0;
    return true;
}
bool IsEffectObject(void* object) { return object == effect; }
bool HasCallableDtiVirtual(void* object) { return object == owner || object == effect; }
uint64_t GetObjectDti(uint64_t object) {
    if (object != (uint64_t)owner) return 0;
    if (*(uint64_t*)owner == VTABLE_URROCKET_RACCOON) return 0x140D459D8;
    if (*(uint64_t*)owner == 0x140D45DD8) return 0x140D45DD8; // mock Storm identity
    return 0;
}
bool IsActiveUnit(void* object) { return (*(uint32_t*)((uint8_t*)object + 0x10) & 7) < 3; }
bool IsRollbackEffectCategory(void*) { return true; }
bool Effect_IsExAtkSpaceTransitionEffect(void*) { return false; }
bool IsPersistentCharacterEffect(void*) { return false; }
''' + block(source, "static bool IsCharacterShortEffectReferenced(") + "\n" + block(
            source, "static void* __fastcall hkEffectCreateShort(") + "\n" + block(
            source, "void Effect_RestoreCharacterReferences(") + "\n" + block(
            source, "void Effect_ReleaseSnapshotResources(") + r'''
void Capture(RollbackSnapshotSlot& slot) {
    Effect_ReleaseSnapshotResources(slot);
    slot.effectRecipes.clear();
    size_t capturedCount = 0;
    const std::vector<void*> exAtkChildren;
''' + block(capture, "for (const RollbackEffectRecipe& recipe : g_liveEffectRecipes)") + r'''
}
int Restore(RollbackSnapshotSlot& slot) {
    int restored = 0, failed = 0;
    for (auto& saved : slot.effectRecipes) {
        void* sGameEffect = saved.sGameEffect;
        void* attachTo = saved.attachTo;
''' + block(restore, "if (saved.shortFactory) {") + r'''
    }
    return failed ? -1 : restored;
}
int main() {
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    vtable[0x88 / 8] = (uint64_t)Attach;
    vtable[0x78 / 8] = (uint64_t)Retire;
    for (int id : {0x17, 0x29}) {
        hkEffectCreateShort(owner, 100, id, owner, 0x100, -1, 3, nullptr);
        assert(g_liveEffectRecipes.back().shortFactory);
        assert(g_liveEffectRecipes.back().a3 == id);
    }
    // The mock reuses one address; its tracking entry must be replaced.
    assert(g_liveEffectRecipes.size() == 1 && g_effectSpawnSequence == 2);
    hkEffectCreateShort(owner, 100, 0x18, owner, 0, -1, 3, (void*)1);
    *(uint64_t*)owner = 0;
    hkEffectCreateShort(owner, 100, 0x17, owner, 0, -1, 3, nullptr);
    *(uint64_t*)owner = VTABLE_URROCKET_RACCOON;
    failFactory = true;
    hkEffectCreateShort(owner, 100, 0x17, owner, 0, -1, 3, nullptr);
    failFactory = false;
    assert(g_liveEffectRecipes.size() == 1);

    auto recipe = g_liveEffectRecipes[0];
    for (uint32_t offset : ROCKET_JETPACK_OWNER_OFFSETS) {
        *(void**)(owner + offset) = effect;
        assert(IsCharacterShortEffectReferenced(recipe));
        *(void**)(owner + offset) = nullptr;
        assert(!IsCharacterShortEffectReferenced(recipe));
    }
    recipe.a8 = owner;
    recipe.shortParentNo = 0x17;
    recipe.shortControlState = 0x00000010200028C0;
    recipe.shortFlags = 0x200028C0;
    RollbackSnapshotSlot slot;
    slot.effectRecipes.push_back(recipe);
    assert(Restore(slot) == 1);
    assert(registered == 1 && timelines == 1 && retired == 0 && attached == 1);
    assert(*(void**)(effect + 0x280) == owner);
    assert(*(uint64_t*)(effect + 0x290) == recipe.shortControlState);
    assert(*(uint32_t*)(effect + 0x298) == recipe.shortFlags);

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

    // Native doFinish clears ownership while particle tails can still be alive.
    // Repeated rollback must not reconstruct those unowned looping emitters.
    failUnit = false;
    g_liveEffectRecipes = {recipe};
    *(void**)(effect + 0x288) = owner;
    *(uint32_t*)(effect + 0x10) = 2;
    for (uint32_t offset : ROCKET_JETPACK_OWNER_OFFSETS) {
        *(void**)(owner + offset) = effect;
        Capture(slot);
        assert(slot.effectRecipes.size() == 1);
        *(void**)(owner + offset) = nullptr;
        for (unsigned rollback = 0; rollback < 10; ++rollback) {
            Capture(slot);
            assert(slot.effectRecipes.empty());
            assert(Restore(slot) == 0);
        }
    }
    // Dead-but-occupied owners still drive Rocket's gravity/latch cleanup.
    *(uint32_t*)(effect + 0x10) = 3;
    *(void**)(owner + 0x6900) = effect;
    Capture(slot);
    assert(slot.effectRecipes.size() == 1);
    assert(slot.effectRecipes[0].state.CompletedIndependent());
    *(void**)(owner + 0x6900) = nullptr;
    Capture(slot);
    assert(slot.effectRecipes.empty());

    // General independent effects are not subject to Rocket's owner policy.
    g_liveEffectRecipes[0].shortFactory = false;
    *(uint32_t*)(effect + 0x10) = 2;
    Capture(slot);
    assert(slot.effectRecipes.size() == 1);

    // Storm's flight effect must use the short factory and preserve its flags,
    // attachment and +294 mask, not just a live owner pointer.
    *(uint64_t*)owner = 0x140D45DD8;
    hkEffectCreateShort(owner, 100, 0x23, owner, -1, -1, 3, nullptr);
    auto storm = g_liveEffectRecipes.back();
    *(void**)(owner + 0x6900) = effect;
    assert(IsCharacterShortEffectReferenced(storm));
    storm.a8 = owner;
    storm.shortParentNo = 0x17;
    storm.shortControlState = uint64_t(0x10) << 32;
    storm.shortFlags = 0x200028C0;
    slot.effectRecipes = {storm};
    assert(Restore(slot) == 1);
    assert(*(uint32_t*)(effect + 0x294) == 0x10);
    assert(*(uint32_t*)(effect + 0x298) == 0x200028C0);
    retainTestResource = true;
    g_liveEffectRecipes = {storm};
    Capture(slot);
    assert(resourceReferences == 1);
    Capture(slot); // replacing a ring slot balances its previous retention
    assert(resourceReferences == 1);
    Effect_ReleaseSnapshotResources(slot);
    assert(resourceReferences == 0 && slot.retainedEffectResources.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "rocket_effect_lifecycle")
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-x",
                            "c++", "-", "-I", str(root),
                            str(root / "rollback_character_restore.cpp"),
                            "-o", executable], input=harness,
                           text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
