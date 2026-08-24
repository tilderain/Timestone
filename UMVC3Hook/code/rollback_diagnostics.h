#pragma once

#include <cstddef>
#include <cstdint>

int Rollback_ResolveDllPath(char* destination, size_t destinationLength,
                            const char* relativePath);
bool Rollback_GetNetplayOutputDirectory(char* destination, size_t destinationLength);
void Rollback_ResetNetplayOutputDirectory();
uint64_t Rollback_Fnv1a64(const void* data, size_t length);

inline constexpr bool ROLLBACK_FREEZE_WATCHDOG_ENABLED = true;

void Rollback_FreezeWatchdogHeartbeatImpl(const char* phase, int32_t frame);
void Rollback_FreezeWatchdogUnitImpl(const char* phase, uint32_t category,
                                     uint64_t unit, uint64_t dti, int32_t frame);
void Rollback_FreezeWatchdogMaterialImpl(uint64_t model, uint32_t count,
                                         uint32_t invalid, int32_t frame);
void Rollback_FreezeWatchdogPollImpl();

inline void Rollback_FreezeWatchdogHeartbeat(const char* phase, int32_t frame)
{
    if constexpr (ROLLBACK_FREEZE_WATCHDOG_ENABLED)
        Rollback_FreezeWatchdogHeartbeatImpl(phase, frame);
}

inline void Rollback_FreezeWatchdogUnit(const char* phase, uint32_t category,
                                        uint64_t unit, uint64_t dti, int32_t frame)
{
    if constexpr (ROLLBACK_FREEZE_WATCHDOG_ENABLED)
        Rollback_FreezeWatchdogUnitImpl(phase, category, unit, dti, frame);
}

inline void Rollback_FreezeWatchdogMaterial(uint64_t model, uint32_t count,
                                            uint32_t invalid, int32_t frame)
{
    if constexpr (ROLLBACK_FREEZE_WATCHDOG_ENABLED)
        Rollback_FreezeWatchdogMaterialImpl(model, count, invalid, frame);
}

inline void Rollback_FreezeWatchdogPoll()
{
    if constexpr (ROLLBACK_FREEZE_WATCHDOG_ENABLED)
        Rollback_FreezeWatchdogPollImpl();
}
