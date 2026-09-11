# Compact Effect Generator Control Restore

This follow-up starts from `88c651f` (the pre-pool-snapshot baseline). It addresses
missing projectile bodies, premature callback completion, recurring standalone
bursts, and Doom missiles remaining in cleanup. It does not import the archived
particle-pool serializer or simulate effects from birth.

## Native evidence

- `cParticleGenerator__advanceStateAndSelectSpawnCount`, `0x140841a10`:
  phase `+0x77` consumes startup/emission/post-delay countdown **`+0x122`** and
  repeat counter **`+0x206`**. Restoring phase alone with a fresh countdown is
  incomplete (including decrementing zero into a long wait).
- `cParticleGenerator__advanceStateAndSelectSingleSpawnCount`, `0x1408426d0`:
  when `(generator+0x70 & 0xf00)==0x100`, phase 3 returns one spawn and invokes
  finish; phase 5 only advances elapsed time and returns zero. A single-spawn
  emitter can therefore be finished while its main visible particle remains.
- `cParticleGenerator__updateAndSpawnParticles`, `0x140834f30`: ordinary virtual
  `+0x88`, creates particles and then updates them. Completion also depends on
  generator status **`+0x20`**, including the finish bit. The old compact path
  restored phase 5 but left fresh non-retiring status.
- `cParticleGenerator__distributeSpawnCountAcrossSubframes`, `0x1408289d0`:
  uses the distribution bytes at **`+0x1f1/+0x1f2`**, also absent from the old
  compact record.
- Soul Fist setup `0x1401973a0` and Hadouken setup `0x1401a9220` construct their
  character EFL callbacks directly. Inspection of Ryu/Morrigan common and costume
  archives finds mixed single-spawn Model/PrimModel/Billboard and continuing
  trail generators. This supports the observed failure pattern: phase-5 primary
  emitters remain empty after reconstruction while continuing trails still emit.
  Exact live generator state still needs runtime confirmation.
- Doom missile finish handler `0x14016e700` finishes callbacks before setting
  shot step 2. Its vtable `0x140aaa5e0`, slot `+0x3c8`, resolves to
  `0x1401a0050`: tick vanish and complete only when callback slots are empty.
  Reconstructed callbacks must not restart that already-finished work.

## Changes

The ordinary-generator record saves phase, status, modes, seed/timing, elapsed,
`+0x122`, and the scalar block `+0x1f0..+0x207`. List, pool, allocator, resource,
and owner pointers stay fresh. Only the verified ordinary `+0x88` implementation
uses these offsets. Node/Infinite and other implementations remain represented
in list identity checks, but are not interpreted as ordinary particle storage.
History validation is limited to the audited track-capable DTIs.

For a **live shot callback only**, a single-spawn ordinary emitter saved in phase
5 with an existing particle, whose fresh pool has no active particle, is armed
for one normal phase-3 spawn. Its spawn/event serials are adjusted for that native
increment. There are no extra native update calls. This reconstructs the missing
visual, not its historical particle age/position; growth/trail history may reset.

Fresh effect generation tokens, child censuses and completion counts at
`+0x1f8..+0x207` are preserved. Retained character effects still restore those
fields over their existing graph. Full outer timeline restoration requires a
matching compact generator graph; otherwise only finite expiry/sleep is restored.

Independent completed non-sleeping bursts are not recreated. This targets the
reported Amaterasu TAC flower replay through the general effect-recipe lifecycle;
the exact flower EFL/action event has not yet been identified. Pending effects and
intentional sleeping effects are not treated as completed bursts. Shot callbacks
also tracked as general recipes reuse the shot's fresh callback, rather than
spawning a second effect.

Saved-absent/dead callbacks request native deferred retirement. Step 2/3 shots
using the verified callback-wait handler retire their replacements too. No finish
wrapper is replayed, since it may spawn another explosion. Native preMove clears
the retired slots; native shot cleanup remains responsible for removing missiles.
The historical shot-specific emission exclusions remain in place.

## Verification

`tools/test_effect_generator_state.cpp` exercises countdown/repeat/distribution
restoration, phase-3 repair versus standalone phase-5 completion, pending emission,
existing-particle protection, ownership-byte preservation, opaque-generator
storage, and completed versus sleeping independent effects. It does not emulate
native rendering or prove the exact Amaterasu flower event is covered. In-game
retests should cover Soul Fist, Hadouken, Amaterasu TAC exit, and hit/expired Doom
missiles through repeated rollbacks.

The standalone test passed under ASan/UBSan (72-byte generator record).
`make -j8` and `git diff --check` passed. Runtime confirmation of the four reported
symptoms is still pending; no in-game performance improvement has been measured.
