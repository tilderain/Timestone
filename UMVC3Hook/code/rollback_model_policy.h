#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

bool Rollback_ShouldRestoreShotModelPlayback(uint64_t runtimeDti);
void Rollback_PinShotModelResources(
    void* shot, std::vector<void*>& pinnedShotResources,
    std::vector<void*>& pinnedEffectResources);
int Rollback_RetireShotModelEffects(
    void* sUnit, const std::vector<void*>& currentShots);
void Rollback_PrepareShotModelAfterSpawn(
    void* shot, uint64_t runtimeDti,
    std::vector<uintptr_t>& waitedForModelDtis);
void Rollback_RestoreShotModelLinks(void* shot, void* ownerChr,
                                    uint64_t runtimeDti);
void Rollback_EvaluateRestoredShotModel(void* shot, uint64_t runtimeDti,
                                        const uint8_t* savedBody,
                                        size_t savedBodySize);
