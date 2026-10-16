# Headless material work and savings bounds

Follow-up: `material_resim_skip_audit_20261016.md` identifies concrete job
completion consumers, material readback and snapshot-ownership constraints.

Research: 2026-10-15, Ghidra port 8192, `umvc3.exe.unpacked.exe`, image base
`0x140000000`. Static follow-up to `character_headless_render_audit_20261015.md`.
No runtime skip, profiler or benchmark was added. The existing
`nDraw__Material` type was applied to `0x1405E0530`; material-job research also
applied existing job types to previously untyped clock/completion helpers.

## Result

Both paths contain remaining CPU presentation work. Neither complete path is
safe to remove while retaining only a clock increment.

| Candidate | Evidence-based savings assessment | Principal restriction |
|---|---|---|
| Weapon/helper material animation | Workload-dependent; cheap when no material animation resource is bound. Historical Kobun whole-callback ceiling: 0.155-0.200 ms per seven replayed frames, not a predicted saving. | Keep four slot clocks, wrapping/clamping, cache flags and required material state. Audit concrete setters and overrides. |
| Character material jobs | Higher algorithmic upside with active wildcard color/UV jobs and many materials. No isolated timing or active-job distribution establishes an average millisecond saving or proves this is the largest remaining hotspot. | Keep validation, owner cleanup, clocks, controller frames, repeat transitions, completion/reset/retirement, and feedback through material contents. |

Zero or negative net benefit remains possible when these paths mostly take
their inactive fast exits and new hook/refresh bookkeeping costs more than the
work removed. No direct GPU submission was established in the inspected loops.

## Weapon and helper animation

Confirmed paths:

```text
uChrWeapon__moveAfter                 0x140037340
  active + owner-can-advance gates
  -> updateMaterialsAndCutinLight     0x140200CC0
       -> uToonModel__moveAfter       0x140451370
          shared material-loop body  0x14055FAC0
       -> virtual +0x230
          inspected cut-in lighting  0x140200B60

Kobun/Linlin moveAfterIfActive        0x1400A30A0
  activity predicate + virtual +0x248
  -> 0x140200CC0
```

The material loop walks `mMaterialNum` entries, calls each non-null material's
virtual `+0x38` with model delta time, then ORs `isCBuffer @ 0x1405DD640` into
the model's cache flags. The latter is a small pointer predicate, not animation
evaluation. `0x140564110` exposes an equivalent loop, but the existing repair
hook there does not intercept every entry above. The character-only post skip
at `0x1402021E0` also does not cover these callers.

For MaterialChar vtable `0x140B096D0`, `+0x38` is
`nDraw__Material__update @ 0x1405E0530`:

1. Null animation resource at material `+0x98`: immediate return.
2. Inspect four animation slots at `+0x70`, stride 8. A negative signed
   sequence index is inactive.
3. For each active slot, sample its current frame at `+0x74 + slot*8`.
   Traverse its tracks, search keys, interpolate and call virtual setters.
   Procedural tracks can use `fmodf`, `powf`, `sinf`, `cosf` or spline evaluation.
4. After evaluation, advance the slot frame by `deltaTime * mAnimationSpeed`,
   then wrap or clamp against the sequence duration. Negative frames are
   wrapped/clamped separately.

The expensive middle is a candidate for omission on non-rendered intermediate
updates; the clock tail is not. Preserve native per-step arithmetic if exact
clock equivalence matters. Multiplying delta time by replay depth can change
floating-point rounding, wrap behavior and behavior across sequence changes.

Evaluation precedes advancement. Evaluating once at the final *advanced* clock
would sample one step later than the native last update. A correct rebuild
must also account for sequence/slot changes during replay, retired objects,
partial material writes, cache flags, lighting and any material-state readers.
The no-op helper `+0x248` proof is specific to inspected Kobun/Linlin classes,
not a reason to bypass arbitrary overrides.

### Historical timing ceiling

`native_tick_optimization_20260911.md` records Kobun's entire `sync_save`
callback at 0.02218 ms/replayed frame client and 0.02860 host, or
0.15526-0.20020 ms per seven frames. This includes activity gates, the material
loop, clock work, cache checks and lighting. It is an upper bound on gross
removable work for that callback in that historical workload, not all weapons
or helpers, and not a current measurement. The referenced raw run is not
present in this checkout; these numbers are from the retained analysis.

For scale only, the following are assumptions about the *net removed fraction
of that entire callback*, including refresh and implementation overhead:

| Assumed net fraction removed | Saving per seven replayed frames |
|---:|---:|
| 25% | 0.039-0.050 ms |
| 50% | 0.078-0.100 ms |
| 75% | 0.116-0.150 ms |

These are sensitivity cases, not confidence intervals or expected gains.
Against that run's 1.61656-1.72046 ms total rollback cost per replayed frame,
the 50% case is about 0.69-0.83% less rollback time. Even deleting the entire
Kobun callback would account for only about 1.37-1.66% in that workload.
Other weapons/helpers need their own measurements; do not multiply Kobun's
cost by an assumed object count.

## Character material jobs

`uActorGameModel__updateMaterialVjobs @ 0x140200F50` interleaves seven tables
for ten indices. This is 70 table-pointer tests, with packed four-bit counts
at character `+0x1440`. It dispatches at most ten entries per non-null table,
through virtual `+0x40`; active-state filtering occurs inside the jobs.

| Table pointer offset | Job | Move address |
|---:|---|---|
| `+0x1408` | CngColor | `0x140140060` |
| `+0x1410` | CngUV | `0x1401409B0` |
| `+0x1418` | AddUV | `0x14013F760` |
| `+0x1420` | PatAnm | `0x140140EA0` |
| `+0x1428` | ReflectiveColor | `0x1401403B0` |
| `+0x1430` | SpecularColor | `0x1401406B0` |
| `+0x1438` | Animation | `0x14013FF40` |

### Controller application

Base job move `0x14013F6A0` calls controller trans while active with a target,
then performs owner/target cleanup. Trans `0x14054A430` first validates the
target and can clear it. That clearing affects subsequent cleanup and must
survive any presentation split.

After validation, trans optionally scans all materials and clears four
animation slots per animated material, then calls controller application
`0x14054A960` for 32 controller records. Empty controller names or missing
parent/target take cheap exits. Each nonempty controller scans all target
materials, including when it selects only one material ID.

For each qualifying animated material, application hashes the sequence name
and linearly searches sequences (`0x1405DD4E0`), assigns its slot
(`0x1405DDB60`), and on success writes a wrapped frame using `fmodf`.
The slot setter resets paired state even when given the same sequence index;
these calls are not proven redundant no-ops. Application reads controller
`mFrame` but does not advance the job clock.

For `K` nonempty controllers, `M` materials and sequence search length `S`,
application is approximately `K*M` material visits plus matching materials'
CRC/search/slot writes, potentially `O(K*M*S)`. The unconditional 32 calls
alone do not imply 32 expensive scans.

### Color and UV evaluation

CngColor first runs base move, checks active/target/owner-movement gates,
advances its clock through `0x1401420B0`, then evaluates and applies colors.
Completion `0x14013F600` follows evaluation. A repeat resets time and swaps
endpoints; otherwise it deactivates and may invoke reset `+0x88` and retirement
`+0x78`. The inspected retirement target is `0x140016F20`. These operations are
not optional presentation work. No separate user completion callback was
confirmed in this color path; the virtual reset/retirement calls are confirmed.

With a specific material ID, color evaluation performs one getter scan and
one complete setter scan. With wildcard ID, it performs `M` iterations,
temporarily setting controller 0's ID to `0..M-1`:

- Getter `0x140549F50` scans to the first matching packed material ID.
- Up to four interpolation calls evaluate enabled channels.
- Setter `0x14054A5C0` scans all `M` materials and writes every match.

Thus wildcard evaluation can be quadratic. For the illustrative case where
IDs `0..M-1` match unique materials in array order, getter plus setter material
visits are `M*(M+1)/2 + M*M`: 301 at `M=14`, 737 at `M=22`, per active color
job per update, excluding interpolation and each material's internal state
searches. These are examples, not runtime target counts. Missing IDs can make
getter scans longer; duplicate IDs change write counts.

CngUV has analogous traversal with three UV getter/setter groups and up to six
interpolations per iteration. Assembly at `0x140140D28..0x140140D81` confirms
all three setter groups execute; do not assume channel masks remove them.
Reflective/specular color jobs have related interpolation/application loops.

### Why clock-only preservation is insufficient

- AddUV reads current material UV, adds time-scaled deltas, wraps and writes it
  back. Material contents are recurrence state; skipping writes loses history.
- Partial-channel color/UV writes read existing material values. Later jobs can
  depend on earlier jobs' writes, so skipping intermediate applications is not
  generally equivalent even for absolute interpolation jobs.
- Animation jobs copy their time into named controllers before base trans;
  preserve controller frames, not only the job clock.
- PatAnm applies the current pattern before advancing/deriving its next one.
- Jobs can finish, reset, swap endpoints or retire before final refresh.
  Re-evaluating only surviving jobs cannot recover every completed job's final
  contribution.
- Owner movement gates and target validation differ. Active jobs can apply
  controllers even when owner movement gates prevent clock advancement.

Potential semantic split points are trans's application work after validation
(`0x14054A45E..0x14054A4FF`) and color's evaluation loop
(`0x1401400EF..0x1401402FB`), retaining completion from `0x140140300` onward.
These are research boundaries, not safe machine-code jump patches or proven
whole-program omission policies.

## Estimating savings without inventing timings

Current profiling has no dedicated material-job/application timing bucket.
The old exact-byte audit in `compact_snapshot.md` found unchanged material-job
tables in one 255-frame history. That does not prove inactivity in general or
zero application cost, but reinforces that allocation/table size is not an
active-job workload measurement.

Let `E` be measured eligible evaluation/application cost per replayed frame,
excluding clocks/lifecycle and excluding overlapping parent/child timers.
For `N` replayed frames and a final rebuild costing `R`, net savings are:

```text
saved_ms = sum(eligible historical work omitted) - R - new overhead
```

Only in a stable, validated workload where all `N` evaluations can be replaced
by one same-cost evaluation is this approximately `(N-1)*E - overhead`.
For seven frames that removes 85.7% of the eligible evaluation workload, NOT
85.7% of jobs, character movement or total rollback. Feedback, lifecycle and
variable workloads can require more catch-up work or make coalescing invalid.

| Hypothetically measured eligible E per replayed frame | Seven-frame saving with one same-cost rebuild, before added overhead |
|---:|---:|
| 0.010 ms | 0.060 ms |
| 0.025 ms | 0.150 ms |
| 0.050 ms | 0.300 ms |
| 0.100 ms | 0.600 ms |

This is a break-even/planning table, not a claim that the game spends any of
these amounts on materials. For a 0.1 ms net target per seven frames, eligible
per-frame work must exceed `(0.1 + overhead)/6` ms under these assumptions.
Inactive-job fast exits, null animation resources and empty controller names
can leave little worth removing.

Do not add controller application savings to inclusive trans savings, or trans
savings to inclusive job savings. They are nested. Weapon material evaluation
also consumes slot state written by controllers, so independent skip policies
need a shared state/refresh contract.

## Practical priority

1. Measure exclusive weapon/helper animation evaluation versus its clock tail,
   and jobs by active type, wildcard/specific ID, material count and nonempty
   controller count. Keep the historical Kobun ceiling separate from new runs.
2. For exact-output improvements, investigate reducing repeated material and
   sequence searches while preserving original per-job write ordering. This
   can avoid the difficult claim that material outputs are disposable.
3. Consider omission/coalescing only for audited concrete jobs/setters with
   proven material-reader and retirement behavior. Validate first-visible
   materials, clocks, pattern frames, owner links, liveness and checksums.

No runtime optimization was installed by this research, and no current
millisecond saving has been measured.

## Native timing log

Subsequent instrumentation adds a `MATCOST` summary per rollback batch, enabled
independently of the general rollback profiler. It does not skip or defer work.
Startup logs show hook status for both paths (`0=installed`).

Disabled at the user's request after the capture: `UMVC3_NATIVE_MATERIAL_TIMING`
defaults to `0` in `rollback.h`. This compiles out both timing hooks, counters
and log output; replay boundary calls become inline no-ops. Set it to `1` at
compile time to restore the diagnostic.

- `post_*`: inclusive `0x140200CC0`, material animation plus cut-in lighting
  for every caller reached during replay, not only weapons/helpers.
- `jobs_*`: inclusive `0x140200F50`, including dispatch, clocks, validation,
  application and lifecycle work, for every caller reached during replay.
- `*_calls`, `*_ms`, `*_max_ms`: call count, summed native wall time, maximum
  single call in the batch. Nested calls are inclusive; do not treat these as
  exclusive leaf costs or guaranteed savings.
- `*_per7_ms`: summed time multiplied by `7 / frames`, not necessarily a real
  seven-frame batch. Pool batches by total time and total replayed frames.

Timing covers only calls on the replay thread during the replay loop, excluding
snapshot restore and final repair. Live calls pass through without timers;
other threads are not recorded. Native timing includes timer/trampoline noise,
possible preemption and any instrumented descendants, not just useful CPU work.
Summaries use the existing buffered `HookLog`; exit normally to flush the log.
