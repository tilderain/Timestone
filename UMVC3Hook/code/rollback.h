#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Rollback Snapshot / Restore
//
// Provides frame-indexed save/restore of all MUST-snapshot gameplay state
// for deterministic rollback netcode.  See doc/rollback.md for the full
// singleton catalog and per-field analysis.
//
// Architecture:
//   A ring buffer holds N consecutive frame snapshots.  Each snapshot is a
//   flat memcpy dump of all critical singleton regions + per-character
//   uCharacter bulk data + physics world state.
//
//   Call Rollback_SaveSnapshot() at the FRAME BOUNDARY (before the game
//   tick runs).  When a late remote input arrives, call
//   Rollback_RestoreSnapshot(targetFrame) to rewind, then re-simulate
//   forward with corrected inputs.
//
//   Pointer resolution happens once at init (via getInstance / ptr storage
//   addresses).  Per-character pointers use the game's linked-list
//   traversal (sCharacter -> cChrTeam -> cChrNode -> uCharacter) so they
//   track whatever the game's own data structure says.
// ---------------------------------------------------------------------------

// Maximum rollback window in frames.  Keep in sync with NET_HISTORY_SIZE.
static constexpr int ROLLBACK_WINDOW = 16;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Hooks the physics trampoline to capture the physics world pointer.
// Must be called once during DLL init (from dllmain's hook setup).
void Rollback_InitHooks();

// Save a full snapshot of all MUST regions, keyed by |frame|.
// The snapshot overwrites the ring-buffer slot at |frame % ROLLBACK_WINDOW|.
void Rollback_SaveSnapshot(int32_t frame);

// Restore the snapshot previously saved for |frame|.
void Rollback_RestoreSnapshot(int32_t frame);

// Free all saved snapshot memory.  Call on netplay disconnect / match end.
void Rollback_FreeSnapshots();
