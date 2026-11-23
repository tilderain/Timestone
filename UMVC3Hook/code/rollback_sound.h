#pragma once

#include <cstdint>

// Rollback sound ledger: see doc/sound_rollback_plan.md.
void Rollback_InitSoundHooks();

// Bracket a resimulation. Commands issued in between are buffered, compared
// with the live ledger at End and only the corrected timeline's new sounds
// reach sSound. Begin before the snapshot restore, End after the last frame.
void Rollback_BeginSoundResim(int32_t startFrame);
void Rollback_EndSoundResim();

// A resim runs without sSound::move, which is where the game notices that the
// unit a voice follows has died. Both do nothing outside a resim.
// After the restore and after every resim tick: drop targets no longer alive.
void Rollback_DropDeadSoundTargets();
// Before a unit is freed: drop every reference to it.
void Rollback_ForgetSoundTarget(const void* object);

// sGSound queue state: delayed character voices and the group queue are
// rewound; narration entries only where gameplay queued them unconsumed.
void Rollback_SaveSoundState(int32_t frame);
void Rollback_RestoreSoundState(int32_t frame);

// Commands issued inside are presentation: never tracked, never held.
struct RollbackSoundPresentationScope {
    RollbackSoundPresentationScope();
    ~RollbackSoundPresentationScope();
    bool previous;
};
