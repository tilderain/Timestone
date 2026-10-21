import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_rocket_tail_policy import block


class CharacterTailReferencesTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_capture_remap_null_slots_and_kobun_owner_lookup(self):
        root = Path(__file__).resolve().parents[1] / "UMVC3Hook/code"
        source = (root / "rollback_model_policy.cpp").read_text()
        snapshot = (root / "rollback_snapshot.h").read_text()
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "rollback_character_restore.h"
#include "rollback_capture_set.h"
#define _addr(x) (x)
uintptr_t characterManager = 0, unitManager = 0;
uintptr_t PTR_SCHARACTER = (uintptr_t)&characterManager;
uintptr_t PTR_SUNIT = (uintptr_t)&unitManager;
constexpr uint32_t TEAM0_OFFSET=0x40, TEAM1_OFFSET=0x300;
constexpr uint32_t TEAM_FIRST_NODE_OFFSET=8, NODE_CHR_OFFSET=8, NODE_NEXT_OFFSET=0x10;
constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET=0xC38, SUNIT_CATEGORY_LISTS_OFFSET=0x50;
constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE=0x30, SUNIT_CHARACTER_CATEGORY=8;
constexpr uintptr_t VTABLE_URROCKET_RACCOON=1, VTABLE_UCAP_AMERICA=2,
    VTABLE_UDOCTOR_DOOM=3, VTABLE_UDOCTOR_STRANGE=4, VTABLE_UTRISH=5;
constexpr uint32_t UCAP_AMERICA_SHIELD_SLASH_OFFSET=0x68F0,
    UDOCTOR_DOOM_MISSILE_OFFSET=0x68F0, UTRISH_SPADA_BOOMERANG_OFFSET=0x68F0;
constexpr uintptr_t DTI_USHOT_OPTION_SHIELD_SLASH=6,
    DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT=7, DTI_USHOT_OPTION_SPADA_BOOMERANG=8;
constexpr uintptr_t DTI_UKOBUN=0x140D455B0, DTI_USHOT_VULCAN_KING_KOBUN=9;
''' + block(snapshot, "enum class RollbackCharacterShotReferenceRole") + ";\n" + block(
            snapshot, "struct RollbackCharacterShotReference {") + r''';
struct RollbackShotRecipe {
    void* ownerChr=nullptr;
    uint64_t spawnSequence=0;
    struct { uint64_t dti=0; std::vector<uint8_t> body; } concrete;
};
struct Region { void* ptr; size_t size; uint64_t expectedVtable; };
using RollbackRegion = Region;
struct RollbackSnapshotSlot {
    std::vector<RollbackCharacterShotReference> characterShotReferences;
    std::vector<RollbackShotRecipe> shotRecipes;
    std::vector<Region> regions;
    uint8_t* data;
    size_t size;
    int regionCount;
};
enum class RollbackShotReferenceKind { None, Shot };
struct RollbackShotReference {
    RollbackShotReferenceKind kind;
    uint64_t shotSpawnSequence;
};
alignas(16) uint8_t owner[0x8000]={}, savedBody[0x8000]={}, manager[0xD00]={};
alignas(16) uint8_t helper[0x14D0]={}, oldHelper[0x14D0]={};
uint64_t currentDti=0;
int oldShots[6]={}, newShots[6]={};
uint64_t GetObjectDti(uint64_t object) {
    if (object==(uint64_t)owner) return currentDti;
    if (object==(uint64_t)helper) return DTI_UKOBUN;
    return 0;
}
bool HasCallableDtiVirtual(void* object) { return object==helper; }
bool IsActiveUnit(void* object) { return object==helper; }
bool Shot_GetSpawnSequence(void* pointer, uint64_t& sequence) {
    for (int i=0;i<6;++i) if (pointer==&oldShots[i]) { sequence=i+1; return true; }
    return false;
}
void* Shot_ResolveReference(const RollbackShotReference& ref) {
    return ref.kind==RollbackShotReferenceKind::Shot && ref.shotSpawnSequence<=6
        ? &newShots[ref.shotSpawnSequence-1] : nullptr;
}
''' + "\n".join(block(source, marker) for marker in [
            "void Rollback_CaptureCharacterShotReferences(",
            "void Rollback_RestoreCharacterShotReferences(",
            "void RestoreKingKobunHelper(",
        ]) + r'''
int main() {
    characterManager=(uintptr_t)manager;
    *(void**)(manager+TEAM0_OFFSET+TEAM_FIRST_NODE_OFFSET+NODE_CHR_OFFSET)=owner;
    *(uint64_t*)owner=0x1234;
    RollbackSnapshotSlot slot;
    slot.regions={{owner,sizeof(owner),0x1234}};
    slot.regionCount=1; slot.data=savedBody; slot.size=sizeof(savedBody);
    for (uint64_t dti : {0x140D44E10ull,0x140D45020ull,0x140D45700ull,
                        0x140D45910ull,0x140D45CF8ull,0x140D45D80ull,0x140D45ED8ull}) {
        currentDti=dti;
        auto fields=Rollback_CharacterShotTailOffsets(dti);
        for (bool occupied : {false,true}) {
            std::memset(savedBody,0,sizeof(savedBody));
            for (size_t i=0;i<fields.size();++i) {
                *(void**)(savedBody+fields[i])=occupied ? &oldShots[i] : nullptr;
                *(void**)(owner+fields[i])=&newShots[5];
            }
            Rollback_CaptureCharacterShotReferences(slot);
            assert(slot.characterShotReferences.size()==fields.size()+1); // last-shot slot
            Rollback_RestoreCharacterShotReferences(slot);
            for (size_t i=0;i<fields.size();++i)
                assert(*(void**)(owner+fields[i])==(occupied ? &newShots[i] : nullptr));
        }
    }
    // Category-9 lookup is owner-qualified and saved null never infers a helper.
    std::memset(manager,0,sizeof(manager)); unitManager=(uintptr_t)manager;
    *(uint32_t*)(manager+SUNIT_CATEGORY_COUNT_OFFSET)=10;
    *(void**)(manager+SUNIT_CATEGORY_LISTS_OFFSET+9*SUNIT_CATEGORY_LIST_STRIDE)=helper;
    *(void**)(helper+0x1488)=owner;
    RollbackShotRecipe king;
    king.ownerChr=owner; king.concrete.dti=DTI_USHOT_VULCAN_KING_KOBUN;
    king.concrete.body.resize(0x2160);
    *(void**)(king.concrete.body.data()+0x2158)=oldHelper;
    RestoreKingKobunHelper(savedBody,king);
    assert(*(void**)(savedBody+0x2158)==helper);
    *(void**)(helper+0x1488)=nullptr;
    RestoreKingKobunHelper(savedBody,king);
    assert(*(void**)(savedBody+0x2158)==nullptr);
    *(void**)(helper+0x1488)=owner;
    *(void**)(king.concrete.body.data()+0x2158)=nullptr;
    RestoreKingKobunHelper(savedBody,king);
    assert(*(void**)(savedBody+0x2158)==nullptr);
}
'''
        with tempfile.TemporaryDirectory(prefix="character-tail-refs-") as directory:
            executable = str(Path(directory) / "test")
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            "-I", str(root), "-x", "c++", "-",
                            str(root / "rollback_character_restore.cpp"),
                            "-o", executable], input=harness, text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
