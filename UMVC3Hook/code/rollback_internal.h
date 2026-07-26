#pragma once

// Private rollback implementation boundaries.
//
// rollback.cpp owns snapshot storage, dynamic object reconstruction, diagnostics,
// re-simulation orchestration, and hook installation. These areas currently
// share SnapshotSlot and recipe lifetime state and must remain in one translation
// unit until those invariants have explicit interfaces.
//
// rollback_tick.cpp owns frame-pacing/render patch state and the optional
// headless tick entry point.
//
// rollback_runtime.cpp owns independent native determinism policy.
