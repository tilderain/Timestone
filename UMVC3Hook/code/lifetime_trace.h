#pragma once

#include <cstdint>

// Development diagnostics. Native lifetime changes are controlled separately below.
inline constexpr bool LIFETIME_TRACE_ENABLED = true;
// First retention experiment: detached exact uGameEffect with an empty graph only.
// Requires tracing; active effect simulation is not restored.
inline constexpr bool EMPTY_EFFECT_QUARANTINE_ENABLED = true;
// Restores only already-retiring (state 3/4), empty, ungrouped exact uGameEffect.
inline constexpr bool RETIRING_EMPTY_EFFECT_RESTORE_ENABLED = true;
// Extends retirement retention/restoration to audited CPU billboard graphs.
// Does not permit restoring active simulation or GPU/child-owning generators.
inline constexpr bool RETIRING_CPU_BILLBOARD_ENABLED = true;

void LifetimeTrace_Install();
void LifetimeTrace_SetFrame(int32_t frame, bool resimulating);
// Event names must have static storage (the bounded queue retains the pointer).
void LifetimeTrace_Marker(const char* event, int32_t frame);
void LifetimeTrace_Flush();
// Request final drain at main-tick entry; poll until retained objects and logs drain.
bool LifetimeTrace_FinishCapture();
// Snapshot census, optional holding, and guarded retiring-only state restoration.
void LifetimeTrace_DropSnapshot(int32_t frame);
void LifetimeTrace_SaveSnapshot(int32_t frame);
void LifetimeTrace_CheckSnapshot(int32_t frame);
void LifetimeTrace_ClearSnapshots();
void LifetimeTrace_RestoreRetiringEffects(int32_t frame);
