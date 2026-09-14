# Native effect/projectile restart candidates

Ghidra investigation, 2026-09-13. Read alongside `shot.md`,
`effect_generator_control.md`, and `shot_effect_lifetime_dependency.md`.
Static mechanism verification only: no performance or gameplay-equivalence test.

## Best established candidate: restart an intact uGameEffect graph

Verified constructor `uGameEffect__ctor @ 0x140139D30` installs vtable
`0x140A8E180`. Reading the actual table establishes:

| Slot | Target | Meaning |
|---|---|---|
| `+0xB8` | `0x140132560` | Set/clear sleep bit at effect +0x100 |
| `+0xC8` | `0x140132540` | Set/clear restart-request bit |
| `+0xD0` | `0x1405E6C40` | Set EFL; releases existing graph |
| `+0xD8` | `0x1405E4D10` | Finish |
| `+0xE0` | `0x1405E4DB0` | Restart existing graph |
| `+0xE8` | `0x1405E4CD0` | Clear; releases graph |
| `+0xF0` | `0x1405E4D60` | Keep-hold-off |
| `+0xF8` | `0x1405E6AB0` | Reset outer restart/timing state |

The native property action `uEffect__callRestart @ 0x1405E0874` dispatches
virtual +0xE0. Prefer validated virtual dispatch over forcing a base-class
implementation onto a different concrete effect type.

`uEffect__doRestart @ 0x1405E4DB0`:

1. Resets control/countdown fields in existing 0x150-byte particle-manager records.
2. Resets active-manager count from total-manager count.
3. Calls generator virtual +0x48 on each existing generator/node.
4. Resets active-generator count from total-generator count.
5. Refreshes EFL flags and dispatches effect virtual +0xF8.

The verified uGameEffect +0xF8 target `uEffect__restart @ 0x1405E6AB0` calls
`uBaseEffect__doRestart @ 0x1405EFAF0`, sets a mode bit and reloads remaining
lifetime. The base helper resets outer timing/control fields. Calling +0xF8
alone skips manager/generator reset and is not a complete restart.

The outer +0xE0 implementation has no graph allocation/release. It does invoke
polymorphic generator callbacks, so this does not prove every subclass restart
is allocation-free or free of side effects.

## Ordinary generator behavior and cost

`cParticleGenerator__restart @ 0x14083F5A0` performs:

- `cParticleManager__restart @ 0x14080E2D0`: reset flags, phase and counters.
- `cParticleGenerator__initializeProgressiveTimingState`: reset timing/mode fields.
- `cParticleGenerator__recycleAllActiveParticles @ 0x140827720`: move each active
  particle to the existing free list without allocating pool entries.

The recycle path calls `ParticleRecord__requestChildRetirement` per active
particle. It can invoke a child unit's +0x78 kill callback and clear the child
slot. Therefore restart is proportional to managers + generators + active
particles, with additional child callback costs. It is not constant time, and
later emission may still recreate child units or consume RNG.

`cParticleNode__restart @ 0x140815F30` and
`cParticleNodeInfinite__restart @ 0x140815FA0` reset different class-specific
fields. Do not apply ordinary generator field assumptions to all EFL nodes.

## Preconditions and limits

- Effect body, resource, generator list, manager/pool storage and required child
  ownership must still be intact. Restart does not rebuild destroyed storage.
- Use the same compatible graph configuration; matching EFL alone is not proof
  when enabled descriptors, aggregate mode or other setup choices differ.
- Restart does not re-register sUnit lists/masks, rebind a new shot owner, repair
  shot callback slots, or restore a retired unit's lifecycle bits at +0x10.
- The uGameEffect-specific tail (owner/request, elapsed +0x29C, boundary state,
  etc.) is not generally reset by the inherited restart body. Fresh-spawn
  equivalence requires an explicit audit of these fields and spawn side effects.
- No direct RNG call appears in the inspected outer restart and ordinary reset
  bodies; arbitrary child callbacks and later emission are not proven RNG-free.
- Restart means beginning a new effect cycle. It destroys current particle
  history; it is not historical frame restoration.

## Cheaper operation with different semantics: resume

`uBaseEffect__setSleepMode @ 0x140132560` changes bit 1 at +0x100 only.
Clearing it resumes an intact sleeping effect; it does not reset timers, phases,
particles or unit registration. Useful for pause/resume, not replayed spawning.
`setRestartFlag` only schedules restart; the next normal moveAfter observes the
request and dispatches +0xE0. Avoid a direct restart plus a leftover request that
would schedule another restart.

## Operations that defeat pooling

- `uEffect__setEffectList @ 0x1405E6C40` first calls graph release +0x1A0,
  swaps the resource, and may rebuild generators. Even same-resource rebinding
  has no early equality fast path in the inspected function.
- `uEffect__doClear @ 0x1405E4CD0` also releases the graph.
- `uShotVulcanAxe__restartCallbackEffectForVariantAndFacing @ 0x1401581F0`
  kills callback slot 1, allocates/constructs a new 0x2C0 uGameEffect, binds its
  EFL and owner, and registers it. Despite its name, it is reconstruction.

## Model effects and projectiles

`uGameEffectModel__restart @ 0x14013C380` is a separate model/animation path:
checks its resource, calls virtual +0x158, and optionally binds the selected
actor animation and advances its command cursor at time zero. It is not the
uEffect generator restart API, and its callees need further cost/ownership audit.

No generic complete projectile restart API was established:

- `uShot__initFromShotData @ 0x1401CD9A0` initializes motion controller,
  transform and matrices, not all hit history, callbacks and concrete phase state.
- `uShot__setShotResource @ 0x1401CDAE0` swaps resource references and rebuilds
  attack/collision state; it is not a cheap complete reset.
- Changing shot phase/active bits does not reset its owned graph.

For projectiles the candidate remains same-identity retained-state restoration
with verified per-class restore ranges, collision state, callback state and
manager reconciliation. A genuinely new spawn needs a verified birth-state
reset, new identity, and equivalent native spawn side effects. Effect +0xE0 can
be a component of that reset, not a replacement for projectile initialization.

## Recommended experiment

Retain one intact, same-configuration uGameEffect with ordinary audited generator
types and compare virtual +0xE0 against factory/EFL graph recreation. Confirm
manager/generator/pool addresses stay stable, time the restart plus next emission,
count allocations including particle children, and compare completion frames,
owner/callback links and RNG against a fresh-spawn control. Then validate the
shot-lifetime dependency before expanding to projectile reuse. For a mid-flight
rollback use historical state restoration instead of a birth restart.
