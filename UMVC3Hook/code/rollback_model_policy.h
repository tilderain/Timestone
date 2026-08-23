#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct RollbackShotRecipe;
struct RollbackEffectRecipe;

struct RollbackShotFactoryPolicyState {
    uint8_t ownerSlots[6] = {};
    uint8_t* ownerSlotTable = nullptr;
    uint32_t ownerUsrF = 0;
    uint32_t* ownerUsrFField = nullptr;
};

void Rollback_InitShotModelPolicyHooks();
void Rollback_ResetShotModelPolicy();
void Rollback_ForgetShotModelPolicy(void* shot);
uint32_t Rollback_AfterShotSpawn(void* shot, void* parent);
void Rollback_BeforeShotHitboxUpdate(
    void* shot, const RollbackShotRecipe* recipe);
bool Rollback_AfterShotHitboxUpdate(
    void* shot, const RollbackShotRecipe* recipe);
bool Rollback_ConsumePendulumCollisionReset(void* shot, uint64_t runtimeDti);
void Rollback_BeforeShotHitCollision(
    void* shot, const RollbackShotRecipe* recipe);
void Rollback_TraceShotPolicySpawn(
    void* shot, uint64_t spawnSequence, int32_t frame);
bool Rollback_ShouldReapplyShotCapsules(uint64_t runtimeDti);
bool Rollback_ShouldRestoreShotModelPlayback(uint64_t runtimeDti);
bool Rollback_ShouldFinalizeShotBeforeRetirement(uint64_t runtimeDti);
void Rollback_PinShotModelResources(
    void* shot, std::vector<void*>& pinnedShotResources,
    std::vector<void*>& pinnedEffectResources);
int Rollback_RetireShotModelEffects(
    void* sUnit, const std::vector<void*>& currentShots);
void Rollback_PrepareShotModelAfterSpawn(
    void* shot, uint64_t runtimeDti,
    std::vector<uintptr_t>& waitedForModelDtis);
void Rollback_EvaluateRestoredShotModel(void* shot, uint64_t runtimeDti,
                                         const uint8_t* savedBody,
                                         size_t savedBodySize);
void Rollback_EvaluateRestoredDogMissileModel(void* shot,
                                               uint64_t runtimeDti);
bool Rollback_RemapShotSpawnParent(
    const RollbackShotRecipe& saved,
    const std::vector<RollbackShotRecipe>& recipes,
    RollbackShotRecipe& remapped);
void Rollback_PrepareShotFactory(
    const RollbackShotRecipe& saved, RollbackShotFactoryPolicyState& state);
void Rollback_FinishShotFactory(RollbackShotFactoryPolicyState& state);
void Rollback_RebuildShotStateBeforeRegistration(
    void* shot, const RollbackShotRecipe& saved);
void Rollback_RestoreShotClassState(
    void* shot, const RollbackShotRecipe& saved, void* spawnParent);
void Rollback_RemapCharacterShotLinks(
    void* character, uint64_t expectedVtable, const uint8_t* savedCharacter,
    size_t savedCharacterSize, const std::vector<RollbackShotRecipe>& recipes);
void Rollback_RemapCharacterEffectLinks(
    void* character, uint64_t expectedVtable, const uint8_t* savedCharacter,
    size_t savedCharacterSize, const std::vector<RollbackEffectRecipe>& recipes);
uint32_t Rollback_RepairNemesisExplosionAttack(
    void* shot, void* parent, uint32_t restoredId = UINT32_MAX);
