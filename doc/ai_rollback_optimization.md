# AI Rollback Optimization Specification

**Status:** Active implementation specification  
**Scope:** Profile-guided performance optimization of UMVC3Rollback re-simulation  
**Primary consumer:** An AI coding agent with repository and Ghidra access

## 1. Core Objective

Reduce the cost and worst-case latency of rollback re-simulation without
changing gameplay state, RNG consumption, input behavior, object lifetime, or
netplay synchronization.

The optimization loop is:

```text
Enable profiling -> build -> run a dual-CPU battle -> inspect JSON ->
identify a measured hotspot -> investigate it -> make one small change ->
repeat the same profile -> validate performance and determinism
```

Profiling is evidence, not permission to remove work. A costly native function
may still be required simulation. Every optimization must preserve the state
produced by the original rollback path.

## 2. Outcomes

| Outcome | Meaning |
|---|---|
| `profile_ready` | The run completed and produced enough rollback samples for analysis. |
| `optimization_verified` | A candidate produced a repeatable improvement and passed correctness validation. |
| `no_actionable_hotspot` | The profile is valid, but no sufficiently large safe candidate was found. |
| `insufficient_signal` | The battle ran, but too few rollback windows or replayed frames were recorded. |
| `run_incomplete` | The battle did not reach the target fighting frame or artifacts are invalid. |
| `performance_regression` | The candidate is slower than baseline outside expected run variance. |
| `correctness_regression` | The candidate changes deterministic gameplay state, crashes, freezes, or desyncs. |

`insufficient_signal`, `run_incomplete`, and malformed profile output must never
be reported as successful optimization runs.

## 3. Non-Negotiable Rules

1. Enable rollback profiling at compile time before building. The operator may
   set the profiling switch manually; this specification does not prescribe a
   compiler flag.
2. Perform a clean release build after changing the profiling setting. Do not
   compare objects compiled with different profiling settings.
3. Do not deploy, copy, or inspect the installed ASI location. The project is
   already deployed through its existing setup.
4. Use a release build. Debug timings are not representative.
5. Keep NDJSON logging disabled during performance measurement. Every analyzed
   rollback entry must report `"ndjson_enabled": false`.
6. Do not change rollback depth, delay, multi-resim mode, roster, stage, CPU
   mode, or other test conditions between baseline and candidate runs.
7. Optimize one independently measurable candidate at a time.
8. Never accept a speedup that changes deterministic state or merely avoids
   rollback work by reducing the number or depth of rollback windows.
9. Do not optimize logging or profiling instrumentation unless the profile and
   a dedicated control measurement prove it is the production bottleneck.
10. Prefer optimizations in the mod's snapshot, restore, re-simulation,
    bookkeeping, traversal, allocation, and policy code. Native gameplay code is
    too risky to alter merely because it is expensive.
11. Do not disable or bypass a native gameplay function unless there is clear
    evidence that it performs exclusively render or presentation work during
    re-simulation. That evidence must include Ghidra analysis of its writes and
    callees plus live-versus-resimulation state validation.

## 4. Baseline Protocol

### 4.1 Build

Manually enable rollback profiling (if it is not already enabled), then rebuild from clean objects:

```bash
make clean
make -j8
```

The build must complete successfully. Record:

- Git commit and dirty state.
- Whether profiling is enabled.
- Build mode and compiler.
- Any local optimization patch already present.

Do not begin a baseline if unrelated experimental performance changes are
enabled and cannot be identified.

### 4.2 Fixed Battle Configuration

Launch dual training with both teams under native CPU control through Wine from
the game directory:

```bash
wine umvc3.exe -training-cpus -training-frame-limit=2000
```

The `-training-cpus` option is the command-line equivalent of selecting
**Launch Dual Training with CPUs**. It must bypass the launch dialog, start the
dual-training host, spawn the connected second instance, and configure both
teams for CPU control. The AI must use this command-line path because it cannot
operate the launch dialog.

`-training-frame-limit=<frames>` is forwarded to the spawned client. Each
process counts distinct native phase-5 fighting frames across battle
generations and closes normally after reaching the limit. Use `1000` for a
short discovery run or `2000` for the preferred baseline. The option accepts an
integer from 1 through 10,000,000.

Confirm the command-line CPU mode in both process logs before collecting data:

```text
UMVC3Hook: Dual training CPU control enabled for both teams
TrainingBoot: CPU control mode=2
```

If either process does not report two-CPU control, classify the run as
`run_incomplete` and relaunch it. Do not use a human-controlled or one-CPU run
as the performance baseline.

Choose a fixed roster and stage before the first run. Preserve them for all
baseline and candidate runs. Prefer a roster that exercises varied systems:

- Projectiles and persistent effects.
- Assists and reserve-character updates.
- Throws or cinematic actions.
- Character-specific resources or transformations.

The first baseline may use any representative roster. Later investigations
should add focused matchups only when a hotspot is state-dependent. Do not
average unrelated rosters into one comparison.

### 4.3 Run Length

Set `-training-frame-limit` to at least **1,000 fighting frames**. A
**2,000-frame** run is preferred and should be used when runtime permits. The
plugin counts fighting frames only, excluding loading, intro, startup-barrier,
post-match, restart, and paused frames.

Wait for both processes to close automatically, then inspect the artifacts. The
logs must contain `FrameStepper: training frame limit reached` on both sides.
Do not read the profile while either process is still writing it. Use an
external wall-clock timeout only as a deadlock fallback; if it fires, classify
the run as `run_incomplete` and do not benchmark its partial profile.

Long runtime alone does not make a valid profile. The run must also meet the
sample requirements in section 5.

### 4.4 Repetition

For exploratory ranking, one valid 1,000-2,000-frame run is sufficient. Begin
verification with one valid baseline and one valid candidate run under the same
conditions. Before launching more games, inspect the raw profiles as described
in section 6.6 to determine whether the targeted change can be separated from
run-to-run battle variance.

Do not require a fixed number of repetitions solely because the aggregate
improvement is below a percentage threshold. A small change may still be
measurable when its natural workload is constant and its targeted bucket shifts
consistently across rollback windows and both peers. Conversely, a large total
time change is not convincing when it comes from different native actions,
shots, effects, or rollback depths rather than the modified code.

Launch additional baseline or candidate games only when the existing profiles
cannot resolve the question, including when:

- The targeted bucket lacks per-window or per-operation timing.
- Baseline and candidate workloads differ materially after normalization.
- Host and client results disagree without an understood state-dependent cause.
- The per-window confidence interval includes no change by a meaningful margin.
- A few outliers dominate the result.
- The claim concerns whole-run latency but unrelated gameplay work masks the
  targeted saving.

When more runs are necessary, add the minimum number needed to resolve the
specific ambiguity. Record why the existing profiles were insufficient rather
than applying an automatic three-baseline/three-candidate rule.

### 4.5 Automated Runner

Use `tools/rollback_profile_ai.py` to launch, capture, qualify, and compare
profiling runs. The runner supplies `-training-cpus` and
`-training-frame-limit`, waits for both completion logs, and merges profile
segments produced across rematches.

Capture an explicit baseline:

```bash
python3 tools/rollback_profile_ai.py run --label baseline --frames 2000
```

After implementing one candidate, capture it and compare it to the baseline run
directory printed by the first command:

```bash
python3 tools/rollback_profile_ai.py run --label candidate --frames 2000 \
  --compare-to rollback_profile_runs/<BASELINE_RUN>
```

An existing pair can be compared again without launching the game:

```bash
python3 tools/rollback_profile_ai.py compare \
  rollback_profile_runs/<BASELINE_RUN> \
  rollback_profile_runs/<CANDIDATE_RUN> \
  --output rollback_profile_runs/comparison.json
```

The runner discovers only artifacts changed after its launch timestamp and
copies them into a labeled run directory. It deliberately requires an explicit
baseline for A/B comparison instead of assuming that arbitrary pre-existing
profiles form a valid pair.

## 5. Artifact Discovery and Qualification

### 5.1 Profile Location

Each netplay process writes its own session directory beside the loaded plugin:

```text
nplive_<TIMESTAMP>_host/rollback_profile.json
nplive_<TIMESTAMP>_client/rollback_profile.json
```

A reset or rematch may create a suffixed directory such as `_r1`. Use only
directories created after the test launch timestamp. Match host and client
sessions by timestamp and logs; never analyze a stale profile from an earlier
run.

When profiling is active and NDJSON is disabled, the log reports:

```text
NetplayLive: profile session -> <directory>
```

Use that path as the authoritative artifact location. Preserve the raw host and
client JSON files before beginning another run because a later session may
replace or split artifacts.

### 5.2 JSON Structure

`rollback_profile.json` is a JSON array containing two entry types:

- `type: "rollback"`: one complete rollback window from `start_frame` through
  `end_frame`.
- `type: "post_resim"`: work performed after re-simulation, including notify,
  input resolution, and the final snapshot save.

Do not combine these entry types as if they had the same schema. Analyze
rollback-window cost first, then report post-resim overhead separately.

### 5.3 Minimum Useful Signal

A profile is `profile_ready` only when all of the following are true:

1. Both files parse as valid JSON arrays.
2. At least one side contains `rollback` entries.
3. All analyzed entries have `frame_count > 0` and finite non-negative timing
   values.
4. All analyzed entries have `ndjson_enabled == false`.
5. Baseline and candidate use the same `multi_resim_enabled` value.
6. The combined evidence contains at least 30 rollback windows and 200 replayed
   frames.

The 30-window and 200-frame thresholds are minimums, not targets. Prefer 50 or
more windows and 300 or more replayed frames. If the profile lacks signal, run
the CPU battle longer. Do not fabricate rollback, edit the JSON, or interpret
zero samples as zero cost.

Host and client may have different rollback workloads. Qualify and summarize
them separately before presenting an optional combined total.

## 6. Required Profile Analysis

### 6.1 Top-Level Rollback Metrics

For `rollback` entries, compute separately for host and client:

- Rollback-window count.
- Total replayed frames: `sum(frame_count)`.
- Average and median rollback depth.
- Maximum rollback depth.
- Aggregate rollback time: `sum(total_ms)`.
- Cost per replayed frame: `sum(total_ms) / sum(frame_count)`.
- Average rollback-window cost.
- P50, P95, and maximum `total_ms`.
- P50, P95, and maximum `max_frame_ms`.

Use weighted aggregation. Do not average per-window `avg_frame_ms` values because
a two-frame rollback and a sixteen-frame rollback must not have equal weight.

Also bucket windows by depth when sample count permits:

```text
1-2 frames
3-4 frames
5-8 frames
9-16 frames
```

This distinguishes a genuine faster replay tick from a run that happened to
contain shallower rollbacks.

### 6.2 Major Phase Ranking

Sum each field in `phases_ms` across all rollback entries and report:

- Aggregate milliseconds.
- Milliseconds per replayed frame.
- Percentage of aggregate `total_ms`.
- Largest per-window value when available.

At minimum rank:

- `restore`
- `frame_snapshot`
- `native_tick`
- `input_availability`
- `frame_input`
- `frame_input_end`
- `input_history_save`
- `input_history_restore`
- `timing_resync`
- `cutin_repair`
- Patch enable/disable and diagnostic phases

The phase totals may not account for every instruction in `total_ms`; report an
`unattributed_ms` residual rather than forcing percentages to sum to 100%.

### 6.3 Headless Tick Ranking

Aggregate the `headless_tick` object by total and maximum cost. Rank these
subsystems by milliseconds per replayed frame and aggregate share:

- Unit movement.
- Collision hit processing.
- Unit synchronization.
- Action processing.
- Input devices.
- Gameplay setup.
- Manager core and simulation tail.
- Render and post-frame paths.
- Timing, viewport, and render-begin setup.

Use the corresponding `*_max_ms` values to identify tail spikes. A subsystem
with modest aggregate cost but repeated large maxima may be more harmful than a
smooth subsystem with slightly greater total cost.

### 6.4 Unit Categories and Concrete Objects

Aggregate `unit_categories` by `(phase, category, name)` and `unit_objects` by
`(phase, category, dti, name)`.

For each aggregate compute:

- Calls and visits.
- Total milliseconds.
- Average microseconds per call.
- Maximum microseconds.
- Calls per replayed frame.
- Share of headless unit time.

Rank first by total cost, then separately by maximum call cost. Preserve the DTI
address in the report because names can be missing, misleading, or shared.

High call count and high per-call cost are different opportunities:

- High call count suggests redundant traversal, avoidable dispatch, repeated
  lookup, or incorrectly active objects.
- High per-call cost suggests an expensive state-dependent implementation.
- A high `visits` count with few timed `calls` may indicate registration or
  dispatch behavior worth inspecting before changing code.

### 6.5 Hook, Snapshot, and Restore Detail

Aggregate and rank `hooks` by total and maximum cost. Use call counts to avoid
optimizing a large one-time call that has negligible total impact.

For `snapshot`, report:

- Total and average save time.
- Average KiB and region count.
- Region construction, allocation/recycling, copying, metadata, collision-clone,
  and shot-recipe shares.

For `restore` and `shot_restore`, report:

- Total restore cost and cost per rollback window.
- Region, collision, owner-pointer, hit-solver, stale-pointer, effect-slot,
  shot, and UI shares.
- Shot collection, destruction, sanitization, respawn, concrete overlay, and
  finalization shares.
- Counts such as current/saved shots, effect visits, sanitizer calls, and
  generator nodes normalized per rollback window.

For `post_resim` entries, separately aggregate `notify_ms`, `resolve_ms`,
`save_snapshot_ms`, and `total_ms`.

### 6.6 Distinguishing a Change from Run Variance

Inspect the existing raw baseline and candidate profiles before requesting more
game runs. Aggregate rollback totals are highly sensitive to CPU battle
trajectory and must not be the sole variance test.

For the targeted bucket:

1. Choose its natural unit of work, such as snapshot saves, replayed frames,
   restore windows, calls, visits, copied bytes, regions, shots, effects, or
   generator nodes.
2. Confirm workload invariants. Compare region count and bytes for snapshots,
   call and visit counts for hooks, object and recipe counts for restore work,
   and rollback depth for per-frame phases.
3. Normalize each rollback entry by that unit before comparing baseline and
   candidate. Prefer ratios such as `copy_regions_ms / snapshot.count` over
   whole-run totals when the change affects one snapshot operation.
4. Compare host and client independently. Agreement in direction and magnitude
   is evidence that a targeted shift is not merely one peer's battle path.
5. Inspect the per-window distribution, including median, trimmed mean,
   percentiles, and outliers. Stratify by rollback depth or object workload when
   those variables affect cost.
6. When sample count permits, bootstrap rollback windows separately by side to
   estimate a confidence interval for the normalized difference. Use a fixed
   seed and report the method. A window bootstrap measures within-run timing
   variance; it does not eliminate a run-wide configuration or workload bias.
7. Compare matching frame ranges or equivalent workload buckets when available.
   Do not force frame pairing when CPU trajectories have diverged.
8. Verify attribution: the measured reduction must appear in the bucket changed
   by the patch and must not be explained by fewer calls, smaller snapshots,
   shallower rollback, or less gameplay activity unless that workload reduction
   is the intended optimization.

The existing profiles are sufficient when the target workload is comparable,
both peers show a consistent normalized shift, the per-window analysis excludes
no change by a useful margin, and correctness artifacts are clean. In that case,
do not run more games merely to satisfy a repetition count. If these conditions
are not met, classify the performance result as inconclusive and collect only
the additional evidence needed to address the identified uncertainty.

## 7. Selecting an Optimization Candidate

### 7.1 Evidence Threshold

A candidate is actionable when it satisfies at least one condition:

- At least 5% of aggregate rollback time.
- At least 10% of native-tick, restore, or snapshot time within its subsystem.
- At least 0.05 ms per replayed frame on the profiling machine.
- A repeated tail spike that materially threatens the frame budget.
- Excess call count that can be proven redundant from control flow or object
  state.

These are investigation thresholds, not guaranteed savings. Prefer candidates
with both meaningful measured cost and a narrow, testable implementation.

### 7.2 Investigation Order

Investigate in this order:

1. Confirm the timing aggregation and identify the exact phase, category, DTI,
   or hook.
2. Locate the corresponding source path and existing rollback policy.
3. Determine whether the work is gameplay, bookkeeping, synchronization,
   presentation, diagnostics, or profiler overhead.
4. If native behavior is unclear, use Ghidra to decompile the function, inspect
   callers and callees, and identify every write and side effect.
5. Determine whether cost scales with rollback depth, active object count,
   shots/effects, assists, or a specific action state.
6. Estimate an upper-bound saving from the profile before editing code.

Do not begin by guessing from function names. Terms such as `draw`, `model`,
`effect`, `sound`, `sync`, or `post` do not prove that a function is safe to
skip. Model joints, collision attachments, effect owners, sound-driven RNG, and
post-move callbacks may be gameplay-critical.

The default conclusion for an expensive native gameplay path is that it must
remain intact. First look for mod-owned overhead around that path, including
duplicate snapshots, broad copies, repeated object discovery, policy lookup,
pointer sanitation, reconstruction, temporary allocation, and redundant
bookkeeping. Only consider native-function suppression when the profile shows
the call materially matters and the investigation clearly proves that its
outputs are render-only.

### 7.3 Candidate Classes

Common safe optimization directions include:

- Eliminate duplicate scans over the same stable collection within one restore.
- Cache immutable DTI, vtable, or policy resolution outside inner loops.
- Reuse allocations already owned by rollback storage.
- Narrow copying only when excluded bytes are proven irrelevant and pointer
  ownership remains correct.
- Avoid repeated sanitization when a stronger invariant proves an object was
  already sanitized.
- Reduce mod-owned diagnostics, bookkeeping, conversion, or policy work that is
  not required in production re-simulation.
- Suppress proven render-only native work during `g_isResimulating` only as an
  exception, while preserving surrounding state updates and cleanup.

Unsafe default assumptions include:

- Changing native character, action, shot, effect, collision, AI, model-motion,
  or team-state behavior to make a profile faster.
- Preventing reserve or inactive objects from running their native update path
  based only on apparent role or call cost.
- Skipping collision, command matching, action dispatch, motion evaluation, or
  RNG calls.
- Deleting restore logic because it is expensive.
- Retaining prediction-timeline pointers to avoid reconstruction.
- Replacing native object lifetime with stale object reuse without proof.
- Removing synchronization or waits to improve measured latency.
- Reducing snapshot coverage without deterministic state comparison.

## 8. Implementation Rules

1. Make the smallest change that tests the hypothesis.
2. Keep optimization gates limited to re-simulation when live behavior must not
   change.
3. Preserve call order, locks, reference-count operations, cleanup, RNG draws,
   timers, and scalar state unless analysis proves they are unnecessary.
4. Keep native gameplay execution unchanged unless a separately documented
   render-only exception satisfies the evidence requirements above.
5. Add new profiling fields only when the current buckets cannot isolate the
   candidate. Do not add broad instrumentation speculatively.
6. If instrumentation is added, measure its empty-scope overhead and avoid
   interpreting sub-microsecond differences below timer noise.
7. Build and run `git diff --check` after each implementation.
8. Do not combine unrelated optimizations into one benchmark result.

## 9. Candidate Verification

### 9.1 Performance A/B Test

Rebuild with profiling enabled and repeat the same battle configuration for the
same target duration. Compare baseline and candidate using:

- Cost per replayed frame.
- Depth-bucket cost per replayed frame.
- P50 and P95 rollback-window cost.
- Maximum replay-frame and rollback-window cost.
- The targeted phase/category/object/hook total and call count.
- Snapshot size and object-count changes.
- Host and client results separately.

An optimization is not verified merely because aggregate `total_ms` decreased.
Reject or rerun the comparison if rollback depth, sample count, action mix, or
object workload differs too much to explain the targeted result.

The measured saving should approximately agree with the reduction in the
targeted bucket. If total time improves but the targeted bucket does not, treat
the result as run variance until reproduced.

If total time regresses or fluctuates because unrelated gameplay work differs,
but section 6.6 isolates a repeatable reduction in the targeted bucket under a
constant normalized workload, report the narrow targeted improvement rather
than misclassifying the patch from aggregate time alone. Do not extrapolate that
narrow result into an unsupported whole-run speedup.

### 9.2 Correctness Validation

Every candidate must pass:

1. The same dual-CPU run without a crash, freeze, startup-barrier stall, restart
   failure, or desync.
2. Relevant focused cases from `doc/ai_desync_testing_spec.md` when the changed
   code affects characters, shots, effects, collision, input, RNG, or object
   lifetime.
3. Live-versus-resimulation state comparison for any skipped native work.
4. Existing build and repository validation.

For a presentation-only skip, compare at minimum:

- Character action, position, velocity, flags, and timers.
- Input and command state.
- Gameplay RNG state and draw count.
- Collision and hit results.
- Shot/effect topology and owner links.
- Snapshot region identities and sizes.
- Final ground-truth checksums.

Visual differences during hidden re-simulation may be acceptable only when no
visual state leaks into the subsequent live frame and no gameplay state differs.

Any `.usdp` artifact or desync report must be investigated according to
`doc/ai_desync_testing_spec.md`. A performance patch that exposes a desync is a
`correctness_regression`, not an optimization.

### 9.3 Acceptance Criteria

Accept a candidate only when:

- The targeted bucket decreases as predicted.
- The improvement exceeds observed run variance.
- Cost is not merely shifted into another phase or `post_resim` work.
- Rollback depth and sample quality remain comparable.
- Snapshot/object counts change only when intended and understood.
- Correctness validation passes.
- The code remains simpler than, or proportionate to, the measured gain.

Revert the candidate when it is unmeasurable, state-dependent without a valid
gate, too complex for the gain, or correctness cannot be proven.

## 10. Required Report

Write a concise Markdown report for each investigated candidate with these
sections.

### Test Configuration

```text
Commit:
Profiling enabled:
Roster:
Stage:
CPU mode:
Rollback/delay/multi-resim settings:
Target fighting frames:
Host profile:
Client profile:
```

### Baseline Summary

| Metric | Host | Client |
|---|---:|---:|
| Rollback windows | | |
| Replayed frames | | |
| Average depth | | |
| Total rollback ms | | |
| ms/replayed frame | | |
| P95 window ms | | |
| Maximum window ms | | |

### Hotspot Evidence

```text
Phase/category/DTI/hook:
Aggregate cost:
Cost per replayed frame:
Share of parent phase:
Calls per replayed frame:
Maximum call/window:
State or matchup correlation:
```

### Investigation

Document:

- Source locations and functions.
- Ghidra addresses and recovered behavior when used.
- Reads, writes, RNG, ownership, locking, and cleanup effects.
- Why the proposed change should preserve deterministic state.
- Estimated maximum saving.

### A/B Result

| Metric | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Target bucket ms/replayed frame | | | |
| Total ms/replayed frame | | | |
| P95 rollback-window ms | | | |
| Maximum rollback-window ms | | | |
| Post-resim ms/window | | | |

### Verdict

Use one of:

- `optimization_verified`
- `performance_regression`
- `correctness_regression`
- `no_actionable_hotspot`
- `insufficient_signal`
- `run_incomplete`

State whether the change was retained or reverted and identify the next measured
candidate.

## 11. Optimization Loop and Stop Conditions

Repeat the profile-investigate-change-verify cycle while each iteration has an
actionable measured target.

Stop when any condition is met:

- No remaining candidate exceeds the evidence threshold.
- Remaining hotspots are required gameplay work with no safe narrower path.
- Expected savings are below timer noise or run variance.
- The next change would require disproportionate complexity or correctness
  risk.
- The agreed rollback frame budget is met across representative profiles.

After the final accepted optimization:

1. Run the relevant desync suite with profiling still enabled once.
2. Disable rollback profiling for production.
3. Perform a clean release build.
4. Run a final smoke test of battle startup, rollback, match end, and restart.
5. Keep raw profiles and the report as evidence; do not commit generated profile
   JSON unless explicitly requested.

## 12. Current Profiler Reference

The profiler implementation is primarily defined in:

- `UMVC3Hook/code/rollback_profile.h`
- `UMVC3Hook/code/rollback_profile.cpp`
- `UMVC3Hook/code/rollback_resim.cpp`
- `UMVC3Hook/code/rollback_tick.cpp`
- `UMVC3Hook/code/rollback_snapshot.cpp`
- `UMVC3Hook/code/rollback_restore.cpp`

`doc/retick.md` is an example of turning a profile into a focused native-code
investigation. Its specific measurements are historical evidence, not a
baseline for a different machine or current build.
