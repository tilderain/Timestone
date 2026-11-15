#pragma once

#include <cstdint>

// Rollback sound ledger: see doc/sound_rollback_plan.md.
void Rollback_InitSoundHooks();

// Bracket a resimulation. Commands issued in between are buffered, compared
// with the live ledger at End and only the corrected timeline's new sounds
// reach sSound. Begin before the snapshot restore, End after the last frame.
void Rollback_BeginSoundResim(int32_t startFrame);
void Rollback_EndSoundResim();

// sGSound queue state (delayed character voices, group queue, narration).
void Rollback_SaveSoundState(int32_t frame);
void Rollback_RestoreSoundState(int32_t frame);

// Headless replacement for sGSound::move's narration (announcer) tail.
void Rollback_SoundHeadlessTick();

// Commands issued inside are presentation: never tracked, never held.
struct RollbackSoundPresentationScope {
    RollbackSoundPresentationScope();
    ~RollbackSoundPresentationScope();
    bool previous;
};
