#pragma once

// Private rollback implementation boundaries.
//
// rollback.cpp owns snapshot storage, dynamic object reconstruction, and
// re-simulation. These areas share SnapshotSlot and recipe lifetime state.
//
// rollback_tick.cpp owns frame-pacing/render patch state and the optional
// headless tick entry point.
//
// rollback_runtime.cpp owns independent native determinism policy.
//
// rollback_hooks.cpp owns main-loop orchestration and presentation/timing guards.
//
// rollback_resim.cpp owns corrected-frame replay and deferred rollback state.
//
// rollback_memory.cpp owns game-memory validation and DTI hierarchy queries.
//
// rollback_restore.cpp owns specialized collision restores and deep-clone lifetime.
//
// rollback_rng.cpp owns optional RNG and Hsien-Ko Anki diagnostic hooks.
