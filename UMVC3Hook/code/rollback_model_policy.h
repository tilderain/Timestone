#pragma once

#include <cstddef>
#include <cstdint>

struct ShotModelPolicy {
    uintptr_t dtiAddress;
    const char* name;
    uint32_t resourceOffsets[2];
    uint32_t resourceCount;
    bool restorePlayback;
    bool retrySetupAfterSpawn;
};

const ShotModelPolicy* Rollback_FindShotModelPolicy(uint64_t runtimeDti);
bool Rollback_IsShotModelReady(void* shot);
bool Rollback_RetryShotModelSetup(void* shot,
                                  const ShotModelPolicy& policy,
                                  uint32_t attempts);
