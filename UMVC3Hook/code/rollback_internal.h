#pragma once

// Private rollback implementation boundaries.
//
// rollback.cpp owns snapshot capture/restore and dynamic object reconstruction.
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
//
// rollback_state.cpp owns rollback and re-simulation control globals.
//
// rollback_shot.cpp owns projectile reconstruction and sUnit shot cleanup.
//
// rollback_effect.cpp owns effect recipes, retirement, sanitation, and hooks.
