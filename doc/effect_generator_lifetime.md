# Effect And Generator Lifetime

**Status:** Native behavior and current restore-policy reference, deeply rechecked
against the updated Ghidra database and current source on 2026-09-10.
**Program:** `ichneumon_sub/umvc3.exe.unpacked.exe`, image base `0x140000000`.

This connects effect completion, generator emission, particle pools, and teardown.
Addresses are preferred over historical Ghidra names: several old names assigned
shared helpers to unrelated classes. Field meanings below describe observed
behavior, not necessarily original source identifiers. Unproven points are explicit.

## Review Findings — 2026-09-10

The earlier separation of emission, completion, and destruction remains valid, but
several important generalizations were wrong or incomplete:

1. **The effect list is heterogeneous.** Descriptor kind `0x19` creates
   `cParticleNode` / `cParticleNodeInfinite`, siblings of `cParticleGenerator`
   under the polymorphic `cParticleManager` base. Node `+0xe0` is matrix storage,
   not an active-list pointer; node `+0x1f4/+0x1f8` are also matrix components,
   not spawn serials. The pre-fix snapshot code applied those particle-generator
   interpretations without a family check.
2. **Effect `+0x210` is not the final contiguous allocation size.** Allocation
   initially stores that size, but initialization adds separately allocated
   manager sub-buffers and generator auxiliary storage. It must not bound a raw
   copy of effect `+0x220`.
3. **Effect `+0x1fc` is a scheduler-phase child census.** Aggregate roots clear it
   in phase 2; valid linked children increment it. It is not an ownership
   reference count balanced by destruction.
4. **Configured lifetime expiry does not necessarily terminate the effect.**
   `mEndType` selects finish-and-drain, immediate completion handling, or
   keep-hold-off. The sleep branch and aggregate zero-substep behavior also matter.
5. **Particle child cleanup requests retirement.** The helper at `0x1409727e0`
   calls child virtual `+0x78`, not its deleting destructor. Its previous Ghidra
   comment incorrectly described a type check and destruction.

These are static native/source findings. The historical endpoint corruption has
not been reproduced or attributed to a particular writer by this review.

## Shot Callback Lifetime Fix — 2026-09-10

### Follow-up: Lingering Effects After Hit / Expiry

Runtime feedback confirmed the particle restore stopped disappearance, but Doom
missile hit effects, a Trish circle, and Thor's Mighty Spark still lingered or
reactivated. The first implementation's **fresh fallback discarded terminal
state**, while independent recipes still restored phase without its matching
retirement bits.

Native evidence for the missing event:

- Doom missile hit/expiry handlers `0x14016e700` / `0x14016e7a0` call
  `uShot__finishGameEffectCallbacks(..., UINT_MAX)`, then enter shot step 2.
- Thor's `uShotLaserBigThunder` setup (`0x1401b5780`) references the Thor EFL
  directory and creates two callbacks. Its expiry virtual uses shared finish
  handler `0x14017bd90` and the same cleanup-wait handler as Doom missiles.
- `uShotMissileTrishBullet` hit handler `0x1401b6990` also finishes callbacks and
  enters step 2. This confirms a Trish-family path; the exact reported circle's
  runtime DTI has not been captured in this follow-up.
- `uShot__preMove` (`0x1401ccbe0`) promotes step 2 to 3 and dispatches virtual
  `+0x3c8`. For Doom and Big Thunder this is `0x1401a0050`, which waits for empty
  callback slots. It does **not** repeat the earlier callback finish call.
- `uShot__finishGameEffectCallbacks` (`0x1401c9910`) dispatches effect `+0xd8`.
  Replaying the complete shot hit handler is incorrect: those handlers also
  perform weapon/attack/follow-up-effect work.

The follow-up adds a compact lifetime record independent of particle support or
byte budgets:

1. Capture unit state, sleep state, and whether all remaining emitters have
   finished. The bounded list scan reads only the common `+0x18/+0x20` prefix,
   so it also covers Node/Infinite/EffectUnit without interpreting their pools.
   A partial/cyclic chain does not establish completion.
2. For a successfully restored CPU pool, keep the particle tail. If a confirmed
   cleanup-waiting shot nevertheless has running emitters, dispatch only their
   native virtual `+0x50` with `immediate=true`, allowing those particles to drain.
3. For an unsupported/rejected fresh graph whose saved emission had finished,
   request native retirement. Also retire fresh callbacks for step-2/3 shots
   whose actual `+0x3c8` virtual is one of the two verified callback-wait handlers
   (`0x1401a0050`, `0x14015a8d0`). A step value alone does not select this policy.
4. Preserve finite outer remaining/configured lifetime and end mode when the
   graph falls back, instead of resetting the lifetime at each reconstruction.
   Preserve native sleep mode separately, so an intentionally sleeping effect
   does not wake solely because its graph is unsupported.
5. Model callbacks now capture terminal unit state and the model finish phase
   (`uGameEffectModel+0x1cf8` bits 18..24 == 1). They no longer evade retirement
   merely because they are not derived from `uEffect`.
6. Reconstructed independent recipes with finished emission retire as well;
   retained character effects are explicitly distinguished. The legacy scalar
   emitter path now restores `+0x20 & 0x80000007` with its saved phase, preserving
   fresh sound/ownership bits outside that mask.

Retirement uses virtual `+0x78`, with normal deferred scheduler cleanup. The fix
does not repeat effect `doFinish` wrappers, which could spawn a second follow-up
effect. For unsupported finished graphs, the remaining visual tail is discarded
rather than restarted indefinitely; supported graph tails remain restored.

Additional work is at most 64 common-prefix reads per captured effect, compact
flags per callback, and one cleanup-handler check per restored shot. No new
update hooks, frame-age replay, particle simulation pass, or temporary graph
allocation is added. `tools/test_callback_effect_lifetime.cpp` covers terminal
fallbacks, preserved tails, missed finish on a restored pool, incomplete/mixed
emission chains, sleeping reuse, and finite countdown progress across restores.
The follow-up passed `make -j8`, both lifetime and particle snapshot test suites
with AddressSanitizer/UndefinedBehaviorSanitizer, and `git diff --check`.
In-game confirmation of the three reported cases remains the next validation.

The first implementation is in `callback_particle_snapshot.h`, called by
`ConcreteShotSnapshot_Capture` / `ConcreteShotSnapshot_RestoreCallbackEffects`.
It replaces partial emitter rewinding for shot callbacks with **bounded,
all-or-nothing CPU particle graph restoration**. It does not replay an effect
from birth or invoke another native update to calculate completion.

### What Changes

- Existing particles and their history/track bytes are restored together with
  the emitter countdown, cycle count, progressive timing, finish/status bits,
  and visitation guard. A saved phase-5 effect therefore retains its draining
  particles instead of disappearing over an empty factory pool; its retirement
  bit also survives instead of leaving it active forever.
- Plain-manager transforms, scalar timing, and optional scalar sub-buffer
  contents are restored into their fresh allocations. Ownership pointers stay
  fresh. Effect clocks/completion fields are written **only after** the complete
  graph has passed validation and been restored.
- A callback absent from the saved shot, or already in a non-alive unit state,
  requests native deferred retirement on its fresh replacement. The slot is
  left for native `preMove` cleanup; reconstruction does not immediately delete
  a possibly queued effect.
- General effect recipes that refer to an already-restored shot callback reuse
  that callback instead of creating a second independent effect. Callback
  identity lookup is sorted once per restore and binary-searched per recipe.
- The legacy standalone-effect emission capture now excludes Node/Infinite and
  effect-unit update layouts. Standalone/retained character-effect restoration
  otherwise retains its existing policy; this change targets shot callbacks.

### Supported Layout And Ownership Boundary

Exact DTIs are CPU Billboard (`0x140e1e260`), LiteBillboard (`0x140e1e1b0`), and
mode-0 LitePolyline (`0x140e1e2d8`, nonzero history length). Further restrictions
require no GPU handle, no nonzero generator-owned auxiliary byte count, no child
emission flag or live particle child references, and audited scalar motion modes
0..2 / track modes 0..8. Aggregate roots and linked child effects are excluded.
The **whole callback pool** stays fresh if any generator is outside this boundary
or its layout/resource identity differs. The follow-up above separately preserves
terminal/sleep/finite-clock semantics even when particle restoration is rejected.

The snapshot copies each supported generator's **borrowed slice size `+0x34`**,
not effect `+0x210`. It preserves the fresh generator/pool/manager ownership and
allocator pointers. Only validated particle forward links and generator list
endpoints are relocated; free previous links are reconstructed because native
does not maintain them symmetrically. Manager sub-buffers are copied separately
by their verified `+0x128` size. Resource-backed runtime track pointers must agree
with the fresh graph. The serializer does not guess pointer-shaped values or
relocate arbitrary qwords.

This is deliberately narrower than copying the entire combined allocation or
supporting every generator DTI. Unsupported callbacks still restart natively;
exact visual age for those families, including repeated-restoration behavior,
remains a follow-up. The previous seven shot-specific exclusions are replaced by
these graph-level checks rather than partially restoring their outer clocks.

### Performance And Verification

- One contiguous snapshot blob per supported callback; maximum **64 KiB/effect**
  and **256 KiB/shot**, including graph metadata, with a **2 MiB total/frame**
  budget in `Shot_CaptureRecipes`. Exhaustion rejects further graph captures
  before generator/pool traversal.
- Blobs are immutable and shared across recipe/tracking copies, so copying a
  `RollbackShotRecipe` does not duplicate the particle payload.
- At most 64 generators, 64 managers, and 1,024 entries per pool. The byte budget
  normally limits large pools first.
- Work is linear in bounded pool bytes/entries, **independent of effect age**.
  Validation uses fixed-size stack arrays; restoration allocates no temporary
  vectors and adds no simulation/RNG/native-update calls or per-frame hooks.
- `tools/test_callback_particle_snapshot.cpp` exercises draining particles,
  repeated restores, last-pass completion guards, ownership preservation,
  relocation, unknown-family rejection, malformed lists, and byte budgets.
  Its optional argument runs synthetic capture/restore microbenchmarks; these
  exclude game DTI lookup and memory-query overhead and are not live-game timings.

Verification for this implementation: `make -j8` succeeded; the standalone C++
tests passed normally and with AddressSanitizer/UndefinedBehaviorSanitizer
(`-fsanitize=address,undefined -no-pie`). An optimized 20,000-iteration benchmark,
including fresh payload allocation per capture, measured about **1.67 us** for
16 entries / 4,412 bytes and **20.36 us** for 256 entries / 54,332 bytes per
capture-plus-restore pair. These measurements exercise the snapshot core only.

Runtime validation should cover a burst restored during its particle tail,
continuous fire with repeated rollback, a callback already absent at capture,
and a laser/LitePolyline with existing history. Confirm that restore profiling
does not grow with shot/effect age. The build and synthetic tests do not establish
that every shipped effect resource meets the supported-layout assumptions.

## The Three Lifetimes

An effect object, its generators, and its particles do not finish together.

```text
uEffect: clocks, generator list, manager/pool allocation, resource reference
  |
  +-- separately allocated generator/node objects
  |     |
  |     +-- ordinary particle generator
  |     |     phase machine -> spawn count -> pool activation
  |     |     -> concrete particle initialization -> concrete update/recycle
  |     |
  |     +-- cEffectUnitGenerator (aggregate root)
  |     |     phase machine -> child uEffect creation/registration
  |     |     completion also depends on owner phase-2 child census
  |     |
  |     +-- cParticleNode / cParticleNodeInfinite
  |           separate phase machine -> single track + GPU particle handle
  |           matrices and node-specific completion count, not pool lists
  |
  +-- combined allocation at effect+0x220
        plain manager records -> borrowed generator pool slices -> optional tail
        each manager can additionally own a separate sub-buffer

outer lifetime expiry -> end-mode handling -> continue or completion helper
generator/node virtual +0x88 returns active/inactive
  -> effect recomputes active generator count
  -> zero active generators -> completion helper
completion helper -> sleep OR kill
  -> kill requests retirement; it does not free anything
  -> scheduler unlinking and deleting destructor happen later
```

**A spawn count is not a completion boolean. A completion boolean is not a free.**
The ordinary generator's emission helper returns a count; its virtual `+0x88`
returns whether it still contributes to effect completion.

## Function Map

| Address | Operation | Why it matters |
|---|---|---|
| `0x1405e2860` | `uEffect__buildGeneratorObjectList` | Creates concrete generator objects and records successful count. |
| `0x1405e27c0` | `uEffect__createGenerator` | Orchestrates graph construction; failure releases the partial graph. |
| `0x1405e0950` | `uEffect__allocateParticleManagerAndGeneratorBuffers` | Allocates manager records, particle-pool slices, and optional auxiliary tail. |
| `0x1405e54c0` | `uEffect__initializeParticleManagers` | Initializes plain records at stride `0x150`, including optional sub-buffers. |
| `0x1405e57a0` | `uEffect__moveAfter` | Runs substeps and makes outer completion decisions. |
| `0x1405e67c0` | `uEffect__updateGeneratorsAndCountActive` | **Advances generators** through virtual `+0x88`; formerly `countActiveGenerators`. |
| `0x1405e8690` | `uEffect__sync` | Phase-2 aggregate-root census reset and linked-child census increment. |
| `0x1405eeea0` / `0x1405ef2b0` | End-mode handling / sleep-or-kill | Distinguishes emission finish, sleep, and deferred retirement. |
| `0x140834f30` | `cParticleGenerator__updateAndSpawnParticles` | Spawn, concrete initialization/update, then completion test. |
| `0x140842e20` | `cEffectUnitGenerator` virtual `+0x88` | Child-effect creation path rather than ordinary particle-pool spawning. |
| `0x140841a10` | `advanceStateAndSelectSpawnCount` | Shared emission phase machine. |
| `0x1408426d0` | `advanceStateAndSelectSingleSpawnCount` | Alternative phase path; returns a zero-or-one spawn count. Formerly `advanceStateAndCheckCompletion`. |
| `0x140815920` / `0x140815a60` | Node / Infinite virtual `+0x88` | Separate single-track phase machines; do not use the ordinary pool layout. |
| `0x140815bf0` | `cParticleNode__stepSingleTrack` | Advances borrowed single-track storage; updates node `+0x22a` completion count. |
| `0x14083fac0` | `selectEmissionInterval` | Selects countdown and updates progressive interval state. |
| `0x14083f8b0` | `selectPostEmissionDelay` | Selects post-emission delay and updates progressive delay state. |
| `0x14083f9a0` | `initializeParticlePool` | Establishes pool links/indices, not complete concrete particle payload. |
| `0x14083f430` | `activateParticleFromFreeList` | Moves a free entry to active before concrete initialization. |
| `0x140839820` | `dispatchMode` | Dispatches motion and resets the transient completion guard. |
| `0x140839200` | `shiftHistoryBuffer` | Proven unsigned-underflow path for zero history length. |
| `0x140827680` / `0x140827720` | Recycle one / all active particles | Requests accepted children's retirement before recycling entries. |
| `0x1409727e0` | `ParticleRecord__requestChildRetirement` | State-1/2 child gets virtual `+0x78`; not a deleting destructor. |
| `0x14080d440` / `0x14080d3f0` | Generator / plain-manager seed selection | Initial RNG use is separate from subsequent table-cursor advancement. |
| `0x1405e4db0` | `uEffect__doRestart` | Resets existing managers/generators and completion counts. |
| `0x1405e6ab0` | `uEffect__restart` | Resets outer timing and remaining lifetime; not the complete graph restart. |
| `0x140016f20` | `uEffect__kill` | Requests deferred retirement only. |
| `0x1405e6890` | `uEffect__releaseGenerator` | Destroys generators, frees sub-buffers, then frees combined allocation. |
| `0x14080c120` | `cParticleManager__releaseSubBuffer` | Releases only the manager's separate optional buffer. |

Two especially misleading names were corrected: `0x1405e87b0` is now
`refreshParticleManagerTransformsWithoutSubstep`, and `0x1405e88d0` is
`updateGeneratorsWithoutSubstep`. Neither releases the objects its old name claimed.

## Effect Completion

Offsets in this section are relative to **the effect**, not the generator.

| Offset | Width | Observed role |
|---|---:|---|
| `+0x10` | 4 | Packed unit state/category; low three bits govern lifetime. |
| `+0x108` | 8 | Held EFL resource reference. |
| `+0x114` | 4 | Computed floating-point frame increment. |
| `+0x11c` | 4 | Fractional frame accumulator used to derive substeps. |
| `+0x124` | 4 | Cumulative advanced-frame counter. |
| `+0x128` | 2 | Current substep count. |
| `+0x1d4` / `+0x1d6` | 2 each | Remaining / configured lifetime. |
| `+0x1f0` | 8 | Linked parent-effect reference. |
| `+0x1f8` | 4 | Generation token used to validate linked effects. |
| `+0x1fc` | 2 | Phase-2 census of valid linked children for an aggregate root; not an owning refcount. |
| `+0x1fe` | 2 | Child loop/timing input used in child catch-up calculation. |
| `+0x204` | 2 | Total successfully created generators (`mGeneratorNum`). |
| `+0x206` | 2 | Active generators contributing to completion (`mGeneratorMoveNum`). |
| `+0x208` | 2 | Total plain manager records (`mJointNum` in native properties). |
| `+0x20a` | 2 | Active/retained manager bookkeeping (`mJointMoveNum`). |
| `+0x210` | 4 | `mGeneratorBuffSize`: aggregate buffer accounting after initialization; initially combined allocation size. |
| `+0x218` | 8 | Owned generator-list head; node `+0x18` is next. |
| `+0x220` | 8 | Owned combined manager/pool allocation. |
| `+0x228` | 8 | Optional interior pointer into that allocation. |
| `+0x240` | 2 | Current substep index, not a lifetime count. |

On a normal substep, eligible generator virtual `+0x88` results rebuild `+0x206`.
On a zero-substep update in the **non-aggregate** branch, special update callbacks
run, but this count is not recomputed. Aggregate mode (effect `+0x100 & 0x80000`)
instead directly updates the single list head through virtual `+0x88` on each
substep and stores zero or one; it has no corresponding zero-substep callback
branch. Both can reach the outer zero-count test with an unchanged historical
count when no substeps run. Sleeping effects skip this advancement/completion block.

### Lifetime Expiry Is End-Mode Dependent

In `moveAfter`, configured lifetime `+0x1d6 == 0` disables the outer lifetime
countdown. Otherwise `+0x128` is subtracted from remaining lifetime `+0x1d4`,
saturating at zero. Expiry calls `0x1405eeea0`:

| Effect `+0x130 & 0xf` (`mEndType`) | Action after expiry |
|---:|---|
| 0 | Virtual `+0xd8` (`uEffect__doFinish`); **continue** generator updates so particles/children can drain. |
| 1 | Return true; caller immediately takes sleep-or-kill handling and exits this update. |
| 2 | Virtual `+0xf0` (`doKeepHoldOff`); continue updating. |
| Other | No action in this helper; continue updating. |

`uEffect__doFinish` (`0x1405e4d10`) dispatches generator/node virtual `+0x50`
with `immediate=true`. It can also create a resource-configured follow-up effect
through `0x1405e7aa0` when effect `+0x238 & 0xf000 == 0x1000`. Despite its current
name, `advanceLifetimeByFrames` is a **follow-up child-spawn routine**, not a pure
clock adjustment; its argument becomes the child's catch-up frame input.

There is no one-shot expiry latch in the remaining-life comparison itself:
configured lifetime stays nonzero, so an already-zero remainder can revisit
end-mode handling on a later non-sleep update. Follow-up spawning clears its
own `+0x238` mode nibble.

The shared outer completion helper has a sleep branch; completion does not always
kill. The usual non-sleep retirement path is:

| Unit state | Native action |
|---:|---|
| 1 | Pending setup; category traversal promotes to 2 and calls setup. |
| 2 | Active. |
| 3 | Retirement requested by kill; no allocations released yet. |
| 3 -> 4 | Subsequent category traversal removes secondary/multi-mask links. |
| 4 -> 0 / destruction | Following traversal removes the primary link and invokes deleting destructor. |

Evidence: `sUnit__moveCategory` at `0x14051b4c0`, kill at `0x140016f20`.
FixFlag can prevent kill from changing state. These are traversal transitions,
not a guarantee of two wall-clock frames.

Specifically, `sleepOrKillOnCompletion` clears effect mode flag `0x20`; mode flag
`0x4` selects virtual `+0xb8` with true (sleep), otherwise it dispatches virtual
`+0x78` (kill). Its true result means the kill branch was dispatched, not that
FixFlag allowed retirement or that storage was freed.

### Linked Children Are Counted By Scheduler Phase

`uEffect` vtable `0x140bae1d0` maps `+0x50` to `sync` and `+0x48` to
`moveAfter`. In the native phase-2 sync path (`0x1405e8690`):

- Aggregate root: clears its own `+0x1fc` (`0x1405e86ca`).
- Non-aggregate linked child: accepts parent unit state 1 or 2, equal generation
  tokens at `+0x1f8`, and parent `+0x204 != 0`; increments parent `+0x1fc`
  (`0x1405e8709`). An invalid link dispatches child kill.
- Child creation in `0x140842e20` registers the new effect on **parent category
  + 1**, using `0x14051a690`, which forwards to `sUnit__addUnit`.

This ordering lets the root reset precede its children's increments, before
phase-3 completion consumes the census. Child construction stores the linked
parent through `0x1405e7a00`; `buildGeneratorObjectList` inherits its token from
that parent. Root tokens come from `sEffect+0x94` when aggregate mode is selected.
The child path does not increment the census at spawn time, and neither examined
effect destructor decrements it. It counts eligible sync visits, not allocations
currently owned by the root. Skipping, repeating, or reordering sync is therefore
not equivalent to recomputing an ordinary reference count.

`canSpawnChildEffect` (`0x1405e56d0`) compares effect depth byte `+0x1db` with
`sEffect+0x284`; it does **not** test pool capacity or `+0x1fc`. If a nonzero
spawn count is denied by this depth gate, the effect-unit update still returns
true that tick. Its completion test runs only on the zero-spawn-count branch.

`uShot__clearDeadCallbackEffectSlots` (`0x1401c9ab0`) stops retaining the callback
once it is no longer alive, before the effect allocation is necessarily freed.
`sGameEffect__flushPendingArray` (`0x140130d00`) dispatches kill, not graph
finalization. Flushing after reconstruction can kill freshly created callbacks.

## Generator Phase Machine

Offsets below apply to **cParticleGenerator and its compatible subclasses**,
including `cEffectUnitGenerator`, not every object in the effect list.
Phase is byte `+0x77`.
Labels are descriptive, not recovered enum names.

| Phase | Shared emission behavior | Next transition |
|---:|---|---|
| 0 | Reset elapsed/spawn counters; select startup countdown. | Enter 1, fall through. |
| 1 | Wait for active status and countdown expiry. | Select cycle count, enter 2. |
| 2 | Select emission interval and optional spawn-count seed. | Enter 3, fall through. |
| 3 | Produce this tick's spawn count and advance interval countdown. | Delay phase 4, another cycle at 2, or virtual finish. |
| 4 | Wait through post-emission delay. | Another cycle at 2 or virtual finish. |
| 5 | No new emission; elapsed counter can advance. | Completion still depends on status and concrete update/child state. |
| 6 | Select restart delay. | Return to 1. |

The alternative path at `0x1408426d0` enters phase 3 directly after startup wait
and invokes virtual finish while producing a zero-or-one count. Do not substitute
the shared phase-2/3 emission interpretation for every generator mode.

Ordinary generators select the alternative path when
`(generator+0x70 & 0xf00) == 0x100`; effect-unit generators select the shared multi-emission path only for
`== 0x200`, otherwise the alternative. Its phase-3 entry dispatches finish and
returns one even when phase 3 was restored directly; it does not repeat the
phase-1 status-bit test.

Shared phase 3 decrements a **16-bit** countdown before checking zero; restoring
phase 3 with countdown zero wraps it to `0xffff`, rather than immediately ending
the interval. Remaining cycles `+0x206 == 0` means unbounded cycling in this
helper; positive values are decremented at cycle completion. Progressive interval
and delay fields `+0x1fc/+0x200` are floats, and only advance on the in-range
selection branch (out-of-range selections clamp without advancing them).

Base finish (`0x140829700`) and effect-unit finish (`0x140842cf0`) select phase
5 or 6 according to repeat/deferred-finish flags, but their status writes differ.
Ordinary virtual `+0x88` uses particle update completion; effect-unit virtual
`+0x88` also uses the owning effect's child/link count. Identical prefix layout
does not establish identical lifetime behavior.

The ordinary completion predicate, instruction-checked at
`0x140835189..0x1408351a6`, is:

```text
guard(+0x112) == 0 && (status & 0x80000000) && (status & 0x4)
```

On success it stops vibration/sound and clears status low three bits before
returning false. The effect-unit zero-spawn branch substitutes owner `+0x1fc == 0`
for the guard and stops vibration/sound, but does **not** clear those low bits.
Base finish sets `0x80000004`; effect-unit finish sets only `0x80000000`, relying
on the existing status bit 2. Both use generator **cached flags at `+0xbc`** for
repeat/deferred-finish decisions; this is not a resource pointer dereference.

The following copy/omission column describes the **legacy scalar-only emitter
snapshot**, not the new supported shot-callback graph snapshot above.

| Generator offset | Width | Role | Legacy emission snapshot |
|---|---:|---|---|
| `+0x20` | 4 | Activity/retirement status. | Follow-up copies lifecycle mask `0x80000007`; retains live ownership/sound bits. |
| `+0x77` | 1 | Phase. | Copied. |
| `+0x78` / `+0x7c` | 4 each | Timeline and per-particle table cursors. | Copied. |
| `+0x80` | 2 | Selected seed used during restart. | Copied within 12-byte range. |
| `+0x82..83` | 2 | Packed configuration/flags; high nibble of `+0x83` includes owner particle-volume selection used during layout sizing. | Also copied; not homogeneous timing data. |
| `+0xb8` | 4 | Elapsed/work-tick counter. | Copied. |
| `+0x112` | 2 | Transient concrete-pass visitation/completion guard. | Not copied; recomputed natively. |
| `+0x122` | 2 | Current phase countdown. | **Omitted.** |
| `+0x1f4` / `+0x1f8` | 4 each | Successful-spawn and emission-batch serials. | Copied. |
| `+0x1fc` / `+0x200` | 4 each | Floating-point progressive interval and post-delay accumulators. | **Omitted.** |
| `+0x204` | 2 | Spawn-count track seed. | **Omitted.** |
| `+0x206` | 2 | Remaining emission cycles. | **Omitted.** |

The omitted fields affect transitions, but this table is **not a recommendation
to copy them blindly**. Advancing the historical phase machine over a fresh pool
can still be unsafe even with a complete scalar snapshot.

Emission batch serial `+0x1f8` increments after a nonzero requested batch even
when every ordinary pool activation/initializer fails; successful-spawn serial
`+0x1f4` increments only after successful concrete initialization. Effect-unit
serials follow its child-allocation path and depth gate instead. Neither serial
is a particle-list length.

### The Completion Guard

Generator `+0x112` is not final active-list length. `dispatchMode` resets it;
concrete post-dispatch update passes increment it for visited entries, including
entries recycled during that pass. Billboard writer: `0x14084ec15`; corresponding
patterns were rechecked in LitePolyline (`0x14085ee70`) and Model
(`0x140897c10`) implementations, including their vtable dispatch.

The ordinary completion test occurs after concrete virtual `+0xb8`. Thus the
last particle can be recycled while the visitation guard still holds completion
open for that update. Do not replace the guard with a list-length calculation or
invoke another full update merely to recompute a count. The effect-unit path uses
owner child state instead. Manager-record `+0x112` is an unrelated index.

### The Missing Third Family: cParticleNode / Infinite

`buildGeneratorObjectList` handles descriptor kind `0x19` by allocating `0x280`
bytes and constructing Node (`0x1408142b0`) or Infinite (`0x1408147b0`). The
constructor calls `cParticleManager__ctor`, not `cParticleGenerator__ctor`.
Node vtable `0x140bc48d0 + 0x88` points to `0x140815920`; Infinite uses
`0x140bc4990 + 0x88 -> 0x140815a60`. This distinction is independently supported by allocation,
constructor, vtable, update, and destructor behavior.

| Node-relative offset | Observed role; ordinary pool interpretation is invalid |
|---|---|
| `+0x38` | Borrowed single-track buffer, with track state after its `0x80`-byte header. |
| `+0x48` | GPU-particle handle obtained through `sGpuParticle`; released by the polymorphic base destructor. |
| `+0x77` | Node phase; phases 3 **and 5** run the single-track update. |
| `+0xe0..+0x1df` | Four `0x40`-byte transform matrices, initialized/copied as floats. Not active/free endpoints or capacity/guard fields. |
| `+0x1e0..+0x21f` | Manager-derived matrix written by `0x140816300`; includes would-be spawn counters `+0x1f4/+0x1f8` and would-be owned pointer `+0x210`. |
| `+0x228` / `+0x22a` | Configured/current count; final completion tests current count zero. |
| `+0x256` | Startup/restart countdown; not ordinary generator `+0x122`. |

Node startup wait comes from manager `+0x124`; phase 2 initializes the single
track (`0x140815110`) before phase 3 advances it (`0x140815bf0`). Node finish
(`0x140815050`) also updates node-specific state at `+0x24c/+0x252/+0x254`.
Visibility handling can select phase 7 and clear `+0x22a`. Node destruction
(`0x140814860`) calls the polymorphic base cleanup and frees the node, without
ordinary `cParticleGenerator__dtorBody` pool/auxiliary cleanup.

**Pre-fix restore mismatch:** `CaptureGeneratorEmissionState` checked only that
each list object has a DTI, then labels `+0x1f4..+0x1fb` as spawn counters.
`RestoreGeneratorEmissionState` matched that DTI/resource but did not require
the `cParticleGenerator` family. If a Node phase differs, it follows matrix bytes
at `+0xe0` as a pointer before reading supposed particle history fields. Even if
that scan is bypassed, its serial copy overwrites two components of the current
manager-derived matrix. This is a proven **layout-contract error conditional on
such a node entering restoration**, not proof that a recorded crash involved Node.

The matrix interpretation is instruction-checked: `0x140816358` stores
manager `+0x14` into node `+0x1f4`, and `0x140816361` stores manager `+0x18`
into node `+0x1f8`; subsequent instructions scale the matrix rows. Captured DTI
equality cannot make these bytes ordinary-generator serials.

### Restart And RNG Are More Than Phase Restoration

`uEffect__restart` resets outer clocks/remaining lifetime. Full `doRestart`
additionally resets plain-manager state and calls each node's virtual `+0x48`.
Ordinary generator restart (`0x14083f5a0`) resets base status/phase/table cursors,
initializes progressive timing (`0x14083f4e0`), and **recycles all active
particles**, including child-retirement side effects. Node has its own restart
at `0x140815f30`. Neither operation is a pure count reset.

Initial generator seed selection (`0x14080d440`) consumes zero, one, or two RNG
draws: one if descriptor `+0xf` offers multiple seed entries, and another if the
chosen signed seed is negative. The low 16 bits go to `+0x80` and seed both
`+0x78/+0x7c`. Plain-manager selection (`0x14080d3f0`) independently consumes a
draw when manager resource `+0x60` is negative, storing seed/cursor at
`+0x11a/+0x11c`.

Both call `sEffect__getNextRandom`: owner mode flag `0x8` chooses the private
`sEffect+0x3c` RNG; otherwise it advances global `g_MtXorShift__globalState`.
Subsequent emission choices use cursor-indexed static tables (`cursor & 0xfff`),
not another global RNG draw for each selection. Restoring cursors after factory
creation does not undo the factory's initial RNG consumption. Other constructor
and concrete-initializer draws still require per-path accounting.

## Allocation And Pool Ownership

| Storage | Owner / rule |
|---|---|
| Generator objects | Separately allocated; effect destroys them first. |
| Combined allocation at effect `+0x220` | Effect-owned: manager records, generator pool slices, optional tail. |
| Plain manager record | `0x150` bytes inside combined allocation; **not an MtObject**. |
| Manager `+0x130` | Separate optional sub-buffer; `+0x138/+0x140` are interior views. |
| Generator `+0x38` | Borrowed pool slice; generator must not free the containing allocation. |
| `cParticleGenerator` `+0x210` local allocation | Generator-owned auxiliary storage, separate from the borrowed pool; size at `+0xc4`, interior views at `+0x218/+0x220`. Not a universal node field. |
| Particle `+0x60` child reference | Native cleanup must run before recycling where applicable. |

**Ghidra type separation:** the existing `cParticleManager` is a polymorphic
`0xe0` base used by generator/node constructors. This review added a distinct
`/RE/effect/EffectParticleManagerRecord` (`0x150`) with the observed owner,
descriptor, state/seed, and sub-buffer fields, and applied it to plain-record
allocation, release, initialization, RNG, and lifetime/history helpers. The
resulting `uEffect__initializeParticleManagers` decompilation now identifies a
record pointer rather than a fictitious `uEffect`. Some remaining historical
signatures still use the base type; check the receiver's actual layout.

### Buffer Accounting And Partial Construction

Successful native construction (`0x1405e27c0`) is ordered:

```text
build generator/node list
  -> allocate combined block and assign borrowed slices
  -> initialize managers and allocate their optional sub-buffers
  -> initialize generators/nodes and their auxiliary storage
  -> update disable flags -> set graph-ready effect+0x130 bit24
```

Failure after entering this pipeline dispatches virtual `+0x1a0`
(`releaseGenerator`). A failed object's initialization may leave it linked before
`+0x204` is incremented; teardown correctly follows the list rather than using
the successful-object count as its destruction bound.

In normal mode, the initial combined block size is:

```text
0x150 * distinct manager records
  + sum(list object +0x34 requested buffer bytes)
  + optional linked-child tail
```

The optional tail is added for a **linked child** (`effect+0x1f0 != 0`):
`0x80 + getAuxBufferSizeForMode((EFL+0x98 >> 4) & 0xf)`. The aggregate-root branch
instead allocates one `0x150` manager record. Generator slices can contain more
than entry records: `computeBufferLayout` (`0x140827930`) includes additional
per-entry auxiliary/command/output regions.

After storing the combined size in `+0x210`, native initialization adds:

- Manager `+0x128` at instruction **`0x1405e5519`**: size of separate owned
  `+0x130` storage (`0x20`, `0x30`, or `0x50` when required).
- Generator/node `+0xc4` at instruction **`0x1405e5640`**: separately allocated
  auxiliary bytes; ordinary allocation is established by `0x140830c60`.

Thus final `mGeneratorBuffSize` is accounting, not an allocation boundary.
`releaseGenerator` also leaves `+0x210` unchanged while clearing graph ownership
and counts, so it can remain stale after release. A nonzero size is not proof of
a live graph, and `[effect+0x220, effect+0x220 + effect+0x210)` is not a valid
generic snapshot/relocation interval.

### Ordinary Pool Topology

Ordinary generator list endpoints are `+0xe0/+0xe8` (active head/tail) and `+0xf0/+0xf8`
(free head/tail). Capacity is `+0x110`, entry stride `+0x116`, borrowed base `+0x38`.
At stable boundaries each pool entry belongs to one forward list, endpoint pairs
agree on emptiness, traversal stays inside the correct pool slice, and lists are
acyclic/disjoint. Check these at function boundaries, not during unlink/relink.

Native activation at `0x14083f45e..0x14083f465` does not maintain a conventional
free-list reverse chain. Validate forward links and endpoints; do not reject an
otherwise native list solely because reverse links are not symmetric.

Capacity constraints are only partly established. The ordinary strip-layout
constructor (`0x1408277f0`) loads capacity from emission descriptor `+0xc` and
can reduce it to `floor(capacity*5/16)` or `floor(capacity*9/16)` according to
resource/volume settings, with no minimum clamp in that helper.
`initializeParticlePool(nonnull)` uses unsigned capacity-minus-one/minus-two.
For capacity one the head/tail alias and forward next becomes null, but the
reverse pointer calculation still wraps; activation repairs the entry's active
previous pointer. For capacity zero with a nonnull buffer, tail writes would be
out of range. A zero requested slice is skipped by the normal allocator wiring,
so the arithmetic alone does not prove native resource reachability of that case.
The minimum-one clamp at `0x1408149e0` belongs to **Node's separate count**, not
the ordinary pool; it cannot establish the latter's capacity guarantee.

### History Underflow

For track-capable layouts, particle `+0x9c` selects history mode and `+0x9d` is an
unsigned history-slot count. These offsets are **not universal particle fields**.

In `shiftHistoryBuffer`, the first-substep path (effect `+0x240 == 0`) executes:

```text
0x140839228: MOVZX R9D, byte ptr [RDX + 0x9d]
0x140839264: DEC   R9D
0x140839267: JZ    skip_copy_loop
...
0x140839283: MOV   EAX, dword ptr [R10 + RDX*8]
```

Zero underflows to `0xffffffff` before the zero test and produces an out-of-range
history read. Count one correctly skips the copy loop. This is instruction-level
evidence, independently checked during consolidation.

Pool initialization and activation do not establish the concrete history bytes.
LitePolyline initialization (`0x14085ddf0`) copies resource `+0x170/+0x171` into
mode/count without a nonzero clamp. Spawning and concrete update can happen in
one generator call. An empty active list before restore therefore does not prove
the first post-restore update is safe.

The concrete LitePolyline chain is now traced through
`0x14085ee70 -> 0x14085ec10 -> 0x1408394e0 -> 0x140839200` (subject to the
earlier motion/texture/lifetime gates). `0x1408394e0` dispatches particle
`+0x9c == 0` directly to history shifting. Its layout setup
`0x14084f970 -> 0x140829830` also accepts history count zero: the latter returns
zero extra history bytes, not an error. Resource loading/validation may impose
additional constraints; this review has not proven that a valid shipped resource
can reach the failing combination.

For zero count, the first attempted history source is approximately **64 GiB**
beyond the computed history base (`16 * 0xfffffffe`); it is not a nearby list
field. The following instruction is also an out-of-range write if the read
succeeds. This does not identify the source of a small in-object overwrite.

The exact writer of the reported `freeTail = 0xc0000000` corruption remains
unknown. The underflow above is proven; its responsibility for that separate
topology overwrite is not. Do not equate all particle crashes with one cause.

## Teardown Order

```text
unlink retiring effect from scheduling/owners as required
  -> destroy generator objects (may traverse/recycle particles)
  -> release each manager's optional +0x130 sub-buffer
  -> free combined effect+0x220 allocation
  -> release effect's EFL reference
  -> free effect object storage
```

Clearing manager counts before teardown skips sub-buffer frees. Detaching valid
generator chains skips generator destruction. Freeing pool storage first invalidates
generator cleanup. Native kill is not a substitute for any of these steps.

Ordinary `cParticleGenerator__dtorBody` (`0x140816670`) recycles active particles
only when the active head is nonnull and generator `+0x12c & 0x400` is set; it
then frees its own `+0x210` allocation via allocator `0x140e1e120`. The
polymorphic base cleanup also handles vibration/sound and the optional GPU
handle at `+0x48`. The borrowed pool at `+0x38` is not freed there. Node uses its
own destructor and does not interpret matrix `+0x210` as an owned allocation.

Particle cleanup (`0x1409727e0`) first clears particle `+0x14` bit `0x04000000`.
Only a nonnull child at `+0x60` with unit state 1 or 2 receives virtual `+0x78`,
after which the slot is nulled. If the child is already in another state, the
slot is left unchanged; subsequent activation zeros it. There is no DTI/type-tag
validation, generation-token check, immediate free, or refcount decrement here.

Rollback deliberately performs immediate abandoned-timeline unlink/destruction
rather than waiting for native retirement passes. Its sanitizer preserves valid
ownership and detaches shot-valued reverse references before freeing allocations
(`rollback_effect.cpp`, `SanitizeEffectForDestruction`, `Effect_RetireOne`,
`Effect_RetireCurrent`). These are accessibility/callability checks, not proof
that a readable pointer is the correct allocator-owned base. The sanitizer does
not reconstruct or fully validate each concrete particle pool.

## Pre-fix Restore Dependencies (Audit Baseline)

The table below records the code reviewed before the shot-callback fix above.
It remains relevant to the legacy standalone scalar path, but no longer describes
the new supported shot-callback particle graph restore.

Relevant source: `ConcreteEffectSnapshot_Restore`,
`ConcreteShotSnapshot_RestoreCallbackEffects`, and
`RestoreGeneratorEmissionState` in `UMVC3Hook/code/concrete_snapshot.cpp`.

| Current behavior | Dependency / unresolved policy |
|---|---|
| Every list object with a DTI is captured as a particle generator. | **Node/Infinite layout mismatch:** `+0xe0` scan treats matrix bytes as pointers; `+0x1f4/+0x1f8` copy overwrites matrix components. Require a proven family-specific contract. |
| Outer ranges include effect `+0x204/+0x206`. | Historical graph/cardinality and completion state can replace fresh counts. |
| `state1F8[0x10]` also copies the generation token and phase-2 census `+0x1fc`. | Fresh parent/child graph must agree with those values; restoring a census does not recreate its counted children. |
| `flags100[8]` restores aggregate-mode and other lifecycle flags before graph validation. | The next update can choose the single-generator branch using historical flags over a fresh graph. |
| Generator restore checks list length/order, DTI, resource. | Establishes configuration identity, not particle/child readiness. |
| Generator restore can fail after outer ranges were copied; callers ignore its result. | Produces a mixed historical-effect/fresh-generator timeline. Requires an explicit policy. |
| Mode-0 safety scan runs only when phases differ. | Equal phase does not imply equal countdown, cycles, readiness, or spawn serials. |
| Safety scan examines currently active entries. | An immediate new spawn/update can cross the unsafe boundary afterward. |
| Safety scan reads particle `+0x9c/+0x9d` for all scanned families. | Those offsets are not universal particle fields; this is an additional layout assumption even after excluding Node. |
| Lasers skip generator rewind but still receive outer scalar copies. | A fresh emission graph can still receive historical outer expiry/completion state. |
| Seven exact shot DTIs return before any callback rewind. | Attack Wire, Fire Grenade Explode, Jam Session, Inferno, Shocking Pink Bomb, Ground Oil, Dark Phoenix Fire; empirical exceptions, not generic safety proof. |
| A null saved callback currently preserves the fresh callback. | Older `shot.md` history describes retiring it; current code differs. |

Do not recompute effect counts by calling `updateGeneratorsAndCountActive` or manager
advancement as if they were pure queries: those functions can spawn, consume RNG,
advance countdowns, recycle particles, and change completion.

The audit's source locations were `concrete_snapshot.h` (`CallbackEffectState`
range sizes), `concrete_snapshot.cpp:848..934` (generator capture/restore),
`:1009..1026` (standalone effect restore), and `:1029..1109` (shot callbacks).
The exact outer byte intervals are `[0x100,0x108)`, `[0x110,0x138)`,
`[0x1d0,0x1d8)`, `[0x1e0,0x1f0)`, `[0x1f8,0x208)`, and `[0x230,0x242)`.
They preserve ownership pointers `+0x1f0/+0x218/+0x220`, manager counts
`+0x208/+0x20a`, and buffer accounting `+0x210`, but are not solely clocks.

Generator validation and safety checks finish before its scalar-write loop, so
a rejected generator restore does not partially write that loop. **Outer writes
have already happened**, and both callers ignore the rejection;
`ConcreteEffectSnapshot_Restore` can return true after it. A coherent policy must
cover graph identity, family-specific state, outer completion, and child census
together; adding the omitted ordinary-generator scalars alone cannot fix this.

## Debugging Map

Capture one stable effect/shot identity and its concrete generator DTIs across:

1. Graph creation: concrete family/DTI, actual combined allocation base/extent,
   separate auxiliary sizes, manager count; pool base/capacity/stride only for
   ordinary pools. Do not use final effect `+0x210` as the combined extent.
2. Before restore: historical and fresh phase, countdown, cycles, serials, outer counts.
3. After outer copy and after generator restore/rejection: include rejection reason.
4. First post-restore spawn: selected descriptor, activated entry, initializer result.
5. First concrete update: mode/history fields only for verified concrete layouts.
6. Completion: family-specific guard/count, generator return, effect active count,
   end mode, unit state; for linked children also phase-2 census and generation token.
7. Teardown: generator destruction, each sub-buffer free, combined allocation free.

Useful boundaries are `0x1405e0950`, `0x14083f430`, concrete-init return
`0x1408350ef`, `0x140839200`, `0x140834f30`, and `0x1405e6890`.

## Remaining Research

- Exact writer of corrupted active/free endpoints in the historical crashes.
- Per-DTI/resource prerequisites for concrete initialization and history modes.
- Shipped-resource reachability of zero history count and small ordinary pool
  capacity, including capacity reduction by particle-volume settings.
- Family-specific Node/Infinite reconstruction policy and whether affected live
  callback/resource combinations currently reach generic emission restoration.
- Coherent child-effect reconstruction policy for `cEffectUnitGenerator`.
- Complete per-concrete-initializer RNG accounting beyond the now-traced initial
  manager/generator seed selection and emission table cursors.
- Propagate the new plain-manager datatype through remaining mis-typed helpers;
  recover precise concrete particle and Node layouts without conflating families.
- Formal meaning of remaining packed configuration bits; offset widths alone do
  not establish whether a field may be restored independently.

## Verification And Ghidra Changes

This review re-decompiled the construction, update, phase, restart, child-spawn,
pool, and teardown paths; checked the critical instructions for buffer accounting,
child census, ordinary completion, and history underflow; and read concrete
vtables to distinguish Node and Model dispatch. Existing comments were treated
as hypotheses, including several that still described old rollback experiments.

Ghidra changes include the separate `EffectParticleManagerRecord` datatype and
supported receiver signatures, corrected Node receiver signatures/names at
`0x1408149e0` and `0x140815bf0`, Infinite constructor at `0x1408147b0`,
the single-spawn helper name at `0x1408426d0`,
and the particle child-retirement helper at `0x1409727e0`. The incorrectly
`uModel`-typed sound/vibration helpers at `0x14080e9b0/0x14080ec10` now use the
polymorphic `cParticleManager` receiver. Lifecycle comments record the confirmed
findings. Some multi-typed signatures were rejected by the MCP parser; the
corresponding offsets and intended receiver distinctions are retained here.

The initial research changed documentation/Ghidra only; the subsequent code
attempt is documented in the shot-callback fix section above. Native verification
here is static, not a runtime reproduction. The earlier review referred to
temporary research notes; the durable evidence and corrections from this
re-review are recorded in this document.
