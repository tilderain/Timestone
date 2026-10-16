# Material-post skip scope

Follow-up to `material_resim_skip_audit_20261016.md`, researched on Ghidra
8192, `umvc3.exe.unpacked.exe`, base `0x140000000`, 2026-10-16.
No runtime skip or profiling was enabled. This replaces the earlier blanket
"unknown override" concern with a concrete scope, while retaining unresolved
material-state equivalence requirements.

## Refined result

An exact-class material-loop exclusion for `uChrWeapon`, `uKobun` and `uLinlin`
is a credible experimental candidate. Their inspected post callbacks contain
no additional combat/movement/lifetime work. Their lighting callback is known
and can simply remain native by intercepting the inner material loop instead
of the entire shared post function.

This is stronger than an unaudited generic-model proposal, but it is not a
proof that freezing material evaluation preserves every material reader,
animation frame and first-visible binding. Keep job progression native and
exclude shots/stages/unknown model classes. Do not replace this with a
global early return at `0x140200CC0`.

## Concrete class coverage

Constructor-written vtables and inspected slots:

| Exact class | Constructor | Vtable | Post `+0x48` | `+0x230` | Helper `+0x248` |
|---|---:|---:|---:|---:|---|
| `uChrWeapon` | `0x140036B00` | `0x140A6E150` | `0x140037340` | `0x140200B60` | Not a slot in this table |
| `uKobun` | `0x14009D780` | `0x140A79480` | `0x1400A30A0` | `0x140200B60` | `0x1407E1A10`, bare RET |
| `uLinlin` | `0x1400A2550` | `0x140A79F20` | `0x1400A30A0` | `0x140200B60` | `0x1407E1A10`, bare RET |

The weapon vtable ends at `+0x238`; reading `+0x248` would read the following
class-name string, not a callback. Do not apply helper slot assumptions to it.

Generic weapon factory `0x140038250` allocates a fixed `0x15E0` bytes, calls
the exact weapon constructor, registers on unit line 9, and assigns its owner
at `+0x1540`. It contains no subtype selection or vtable replacement. It has
67 direct call sites across 34 character setup routines, including Dante,
Vergil, Zero, Arthur, Doctor Strange, Rocket Raccoon, Tron and Hsien-Ko.
The other weapon-constructor caller is its DTI factory `0x140037380`.

This gives the exact-weapon allowlist broad ordinary-weapon coverage; no
constructed subclass was found in the inspected constructor/DTI references.
It is not a proof against every indirect or unrecognized construction path.

Tron creates Kobun through the constructor call at `0x1400E585E`; Hsien-Ko
creates Linlin at `0x1400A1F3C`. Each also has a DTI factory. The helper post
wrapper's recovered vtable references identify these two tables.

### Important non-members

`uShotVulcanThrowWeapon` and `uShotVulcanThrowWeaponB` are not weapon subclasses:

| Class | Vtable | Outer post |
|---|---:|---:|
| ThrowWeapon | `0x140AB1460` | `0x1401530F0` |
| ThrowWeaponB | `0x140AB18B0` | `0x1401530F0` |

Their constructors `0x1401782F0` / `0x140179130` call the shot-vulcan
constructor. Outer post updates position, attack matrices, contact/hantei,
durability, lifetime and phase dispatch. Their `+0x248` owner-watch callback
also changes gameplay flags/lifetime/hantei. They share the ordinary
`+0x230` light target, demonstrating that matching this single slot does not
justify skipping an outer post callback.

## Narrow interception point

Verified assembly:

```text
0x140200CC9  CALL 0x140451370
0x140200CD9  JMP  [vtable + 0x230]
0x140451370 JMP  0x14055FAC0
```

`0x14055FAC0` is the canonical material-loop body. An exact-model-vtable
policy there would preserve the outer weapon/helper activity gates, the
helpers' extra callback dispatch, and the lighting tail. Unknown classes
would continue native execution. The separate material-cache repair hook at
`0x140564110` is not this entry point and should not be repurposed as though
it intercepted every post call.

For the three classes, the light target copies only embedded toon parameters
at model `+0x12C0`: rim-light heights, color, power and cut-in factor.
`uToonModel__renderToonShading` consumes them in draw parameter construction.
Leaving this small copy native avoids an unnecessary light-refresh policy.

## What material-loop skipping actually freezes

For `MaterialChar` vtable `0x140B096D0` and `MaterialCharAlpha` vtable
`0x140B09790`, update `+0x38` is the same `0x1405E0530`, with the same inspected
animation-setter slots. It samples up to four current sequence frames, writes
constants/resource bindings, then advances and wraps/clamps those frames.

The enclosing loop also ORs `isCBuffer` into model cache flags. Omission skips
that bookkeeping, not just evaluation. A future reduced/rebuild path must
retain or regenerate it.

Do not allow arbitrary materials just because the model class matches.
`MaterialSSS::update @ 0x14095AF60` calls the common evaluator and then reads
constants, compares map size/layer count, and can destroy/recreate its backing
resources. At minimum, an experiment should restrict materials to audited
implementations or fall back native for an unsupported material.

### Resource-default versus controller-driven clocks

Material animation binding `0x1405DDC30` calls slot reset `0x1405DD9D0`.
The latter clears four slots and automatically selects the first four sequences
whose sequence flag bit 1 (`& 2`) is set, starting their frames at zero.
Those default slots can advance autonomously through material post.

In contrast, `uMaterialControl::trans @ 0x14054A430` can clear all slots and
applies its 32 controllers. Each named controller uses `0x14054A960` to select
a sequence, reset its slot, and write `fmod(controller.mFrame, duration)`.
For such a slot, controller/job state can reassert the sampled frame on each
update; native material-clock advancement is not necessarily its authoritative
timeline. This does not cover unnamed/uncontrolled default slots.

Thus "advance every material clock historically" and "freeze all live clocks"
are not interchangeable policies. A narrow live-presentation freeze is plausible,
especially for autonomous slots that were not rewound, but cannot claim exact
native-resim clock equivalence.

### Material feedback scope

Kobun and Linlin derive from `uActorGameModel`, not the full common character
object. Their inspected moves process commands/jobs/model motion and local
helper state; they do not call `uCharacter__move_base`.

The previously identified common-character material-color readback therefore
does not by itself prove that either helper's own move reads its material
post output. Its color-transition setup passes the character itself as target;
it is not evidence that every weapon material is read by that branch either.

However, getters use controller `mpParent->mpTarget`, so target identity must
be established rather than inferred from the owning job's class. AddUV and
partial color/UV jobs can read/write the same material state that animation
post writes. Skipping post can change a later job's visual recurrence even if
the job's active/time/completion progression remains native. Cross-target
command/property configurations were not exhaustively ruled out.

## The next live tick is not a guaranteed rebuild

Current hook order in `rollback_hooks.cpp:254` performs resimulation before
calling the original live main tick at line 297. This gives ordinary moving
objects a natural live post opportunity before normal rendering.

But weapon post `0x140037340` also requires owner
`canAdvanceSimulation @ 0x140038BE0`. That function can reject an owner with
near-zero time scale while action time has not expired. Weapon drawing at
`0x140036F00` / `0x140036CA0` does not use this same time-advance condition.
Therefore a visible weapon can lack the assumed live material-post refresh.
The helper predicates also have separate parent/movement gates.

This is a concrete reason not to justify skipping with "the next frame will
update it anyway." It is not proof that every pause produces a visible glitch.

For a live-clock-freeze experiment, a possible presentation repair is one
validated material evaluation before the first corrected draw, using zero
delta rather than an extra full native post. Zero delta avoids an ordinary
positive-time advance for finite, normalized clocks, but is not a universal
side-effect-free refresh: wrap/clamp normalization, setter reference changes,
partial-write ordering, unsupported material overrides and cache flags still
need handling. A global full-delta post flush would add a clock step and can
then be followed by another ordinary live post.

An evaluated-only final refresh also cannot generally reconstruct visual
recurrences lost on intermediate frames or completed jobs' overwritten values.
Use an audited workload/state policy, not just a surviving-object pointer list.

## Recommendation

The investigation now supports a concrete experimental boundary rather than
a global post skip:

1. Exact model vtables: weapon, Kobun and Linlin above; reject shot/stage/unknown
   classes even if named "weapon" or sharing the light callback.
2. Intercept only the material loop; keep lighting, movement, commands, jobs,
   constraints and lifecycle native.
3. Restrict material implementations and explicitly choose live-clock freezing
   versus clock-preserving splitting. Do not describe the former as exact
   historical replay.
4. Prove or conservatively exclude feedback-dependent target configurations,
   and handle corrected bindings/cache flags before a visible draw even when
   owner movement is stopped.
5. Validate first-visible output, pause/hitstop/hypers, tags, weapon visibility,
   material sequence changes, helper activation, retirement and match restarts.

The earlier 0.176-0.246 ms per-seven-frame post measurements cover all callers,
not just this allowlist or its removable evaluation. No new performance gain
or visual-equivalence result was measured by this audit.

Existing material/slot/controller structs were applied in Ghidra to clarify
the reset, binding and getter functions. Exact-class dispatch annotations and
this follow-up were saved; material profiling remains disabled.

## Experimental trial

Subsequently enabled at the user's request: hook canonical material-loop body
`0x14055FAC0` and return during resim only for the three exact model vtables
above. Other classes and all live calls retain native behavior. Shared post
still invokes its lighting callback; material-job processing is untouched.
Startup reports `experimental material-post skip status=0` on success.

This first trial freezes the entire material loop, including clocks and cache
flag bookkeeping, for those model classes regardless of material implementation.
There is no deferred refresh or material-type fallback in this trial. It tests
the live-presentation policy, not historical clock equivalence or the fully
restricted proposal above. Paused-visible materials, sequence changes, feedback
and resource overrides remain visual/resource correctness risks to test in-game.
The hook retains no model/material pointers across calls. Material profiling
remains compiled out; no performance or visual-equivalence result is claimed.
