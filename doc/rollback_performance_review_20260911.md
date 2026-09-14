# Rollback performance review — 2026-09-11

## Result

Further investigation of the 0.1 ms/frame target is documented in
[`snapshot_bandwidth_investigation_20260911.md`](snapshot_bandwidth_investigation_20260911.md),
including exact-byte reuse audits and recovered material-job table identities.

The user-supplied baseline is `profile_ready`. Source and optimized Ghidra code
support two narrow candidates:

1. Avoid deep-copying a shot's old concrete snapshot immediately before replacing it.
2. Avoid mapped-pointer classification for identical live/saved qwords in the
   shot overlay's `PreserveLivePointers` loop.

Neither candidate has been implemented or benchmarked. There is no
`optimization_verified` result, and bucket costs below are ceilings, not promised
savings. This is a discovery report using the existing run, not an A/B experiment.

## Test configuration and provenance

```text
Run: rollback_profile_runs/20260911T095307.034337Z_addr_fixed_candidate_4000
Host profile: <run>/host_profile.json
Client profile: <run>/client_profile.json
Current source commit: 30491c3 (clean before this report)
Baseline build commit/compiler flags: not recorded in the supplied summary
Current release recipe: MinGW-w64, -O3, LTO, portable ISA
Profiling: present in baseline; current source default is disabled
Roster IDs: P1=35/17/33, P2=14/11/21
Observed character DTIs: Tron, Arthur, Felicia, Dr Doom, Iron Man, Hiryu
Stage: 105
CPU mode: both teams, mode=2
Multi-resim: enabled on both peers
Observed maximum rollback depth: 7
Configured input delay: not established from these artifacts
Target fighting frames: 4000 per peer
Ghidra instance: 8193, Timestone-unstripped.asi
```

The matching `Timestone_host.log` and `Timestone_client.log` under the game's
`rollback_logs/nplive_20260911_025312_{host,client}/` confirm both CPU-mode
messages, matching launch selections, and completion at 4000 fighting frames
(generation 1, native frame 4122). The runner reports no crash/desync artifacts.

Both raw JSON arrays were parsed and their numeric timing fields checked for
finite, nonnegative values. Every rollback entry has a positive integer frame
count. There are 894 host entries and 506 client entries, split equally between
rollback and post-resim records. Both peers have sufficient signal individually.
No build or new game run was performed. The temporary profiling-header change
was reverted after the user directed use of this baseline.

The loaded binary corroborates the candidate control flow, but its exact build
identity was not independently matched to the profiling executable. Establish
that identity before attributing a future A/B difference solely to a patch.

## Baseline summary

All per-replayed-frame costs use `sum(time) / sum(frame_count)`.

| Metric | Host | Client |
|---|---:|---:|
| Rollback windows | 447 | 253 |
| Replayed frames | 3043 | 1750 |
| Average depth | 6.808 | 6.917 |
| Median / maximum depth | 7 / 7 | 7 / 7 |
| Total rollback ms | 5235.364 | 2828.974 |
| ms/replayed frame | 1.72046 | 1.61656 |
| Average window ms | 11.712 | 11.182 |
| P50 window ms | 11.487 | 10.787 |
| P95 window ms | 15.082 | 14.166 |
| Maximum window ms | 20.548 | 24.353 |
| P95 maximum-frame ms per window | 2.473 | 2.329 |
| Maximum replay-frame ms | 3.560 | 3.516 |

Combined: **700 windows, 4793 replayed frames, 8064.338 ms**, or
**1.68252 ms/replayed frame**. Host/client workloads differ substantially in
projectile count; they must remain separate in candidate comparisons.

### Depth buckets

| Depth | Host windows / frames / ms per frame | Client windows / frames / ms per frame |
|---|---|---|
| 1–2 | No samples | No samples |
| 3–4 | 4 / 16 / 2.09036 | No samples |
| 5–8 | 443 / 3027 / 1.71851 | 253 / 1750 / 1.61656 |
| 9–16 | No samples | No samples |

The four shallow host windows are insufficient for a general shallow-rollback
claim. Most evidence describes approximately seven-frame rollback.

### Major phases

| Phase | Host total ms | Host ms/frame | Host share | Client total ms | Client ms/frame | Client share |
|---|---:|---:|---:|---:|---:|---:|
| Native tick | 3525.304 | 1.15850 | 67.34% | 1985.681 | 1.13467 | 70.19% |
| Frame snapshot | 956.011 | 0.31417 | 18.26% | 481.164 | 0.27495 | 17.01% |
| Restore | 727.683 | 0.23913 | 13.90% | 346.922 | 0.19824 | 12.26% |
| Frame input end | 3.749 | 0.00123 | 0.072% | 2.104 | 0.00120 | 0.074% |
| Frame input | 2.110 | 0.00069 | 0.040% | 1.282 | 0.00073 | 0.045% |
| Cut-in repair | 1.410 | 0.00046 | 0.027% | 0.780 | 0.00045 | 0.028% |
| Timing resync | 0.239 | 0.00008 | 0.005% | 0.148 | 0.00008 | 0.005% |
| Input-history restore | 0.198 | 0.00007 | 0.004% | 0.110 | 0.00006 | 0.004% |
| Input-history save | 0.158 | 0.00005 | 0.003% | 0.090 | 0.00005 | 0.003% |
| Patch enable | 0.093 | 0.00003 | 0.002% | 0.075 | 0.00004 | 0.003% |
| Patch disable | 0.093 | 0.00003 | 0.002% | 0.051 | 0.00003 | 0.002% |
| Unattributed residual | 18.315 | 0.00602 | 0.350% | 10.567 | 0.00604 | 0.374% |

Largest per-window native-tick/snapshot/restore values respectively:
host **14.3105 / 4.0779 / 4.3353 ms**;
client **19.1889 / 5.3503 / 5.7241 ms**. These maxima need not occur in the same
window. Input-availability and separate diagnostic phases are absent from this
schema; absence is not a measured zero. `native_tick_max` is a maximum, not an
additive phase.

### Headless tick

| Subsystem | Host total ms | Host ms/frame | Client total ms | Client ms/frame |
|---|---:|---:|---:|---:|
| Unit movement | 2172.376 | 0.71389 | 1284.414 | 0.73395 |
| Unit synchronization | 1177.552 | 0.38697 | 602.139 | 0.34408 |
| Collision hit | 82.294 | 0.02704 | 45.621 | 0.02607 |
| Early draw/collision | 27.023 | 0.00888 | 15.506 | 0.00886 |
| Render-begin/area | 21.471 | 0.00706 | 12.294 | 0.00702 |
| Simulation tail | 18.018 | 0.00592 | 10.974 | 0.00627 |
| Manager core | 17.119 | 0.00563 | 9.413 | 0.00538 |
| Action | 1.507 | 0.00050 | 0.812 | 0.00046 |
| Timing/input setup | 1.127 | 0.00037 | 0.638 | 0.00036 |

Other individual headless buckets (gameplay setup, viewport, post-frame,
Alt-F4, network services, render, input devices) are each below 0.00024 ms/frame.
Movement plus synchronization accounts for about 95% of headless tick time.
Subsystem timings overlap the top-level native tick and must not be added to it.

### Categories and concrete objects

Largest category: category 8 move, **1690.371 / 1041.551 ms** host/client,
or **0.55549 / 0.59517 ms/frame**. Next is category 21 sync-save,
**383.026 / 181.763 ms**, or **0.12587 / 0.10386 ms/frame**.
Each category has one timed call per replayed frame; its zero `visits` field
does not mean no objects were visited.

| Object / phase / category | DTI | Host total ms | Client total ms | Host/client us per call | Calls/frame |
|---|---|---:|---:|---:|---:|
| Tron / move / 8 | `0x140d45f20` | 528.487 | 277.553 | 173.67 / 158.60 | 1 |
| CPU / sync-save / 21 | `0x140d463b0` | 371.089 | 174.696 | 20.32 / 16.64 | 6 |
| Dr Doom / move / 8 | `0x140d45068` | 366.524 | 151.328 | 120.45 / 86.47 | 1 |
| Felicia / move / 8 | `0x140d45138` | 250.856 | 166.762 | 82.44 / 95.29 | 1 |
| Arthur / move / 8 | `0x140d44e48` | 206.915 | 183.259 | 68.00 / 104.72 | 1 |
| Iron Man / move / 8 | `0x140d45518` | 152.611 | 135.154 | 50.15 / 77.23 | 1 |
| Hiryu / move / 8 | `0x140d45450` | 134.780 | 98.369 | 44.29 / 56.21 | 1 |
| GameEffect / sync-save / 13 | `0x140d46e90` | 128.589 | 76.098 | 5.28 / 5.52 | 8.005 / 7.873 |

Character and CPU calls equal visits. Category-13 GameEffect has
24358/24577 host calls/visits and 13777/13885 client calls/visits.
Its maximum calls are **1195.3 / 1123.4 us**, versus Tron **875.1 / 845.0 us**
and CPU **777.5 / 662.1 us**. This is tail-investigation evidence, not proof that
effects or CPU execution can be bypassed. Reserve-character update and CPU VM
state are required until a narrower behavior is proven otherwise.

### Hooks

Largest hook costs in ms/frame:

| Hook | Host | Client | Host/client maximum us |
|---|---:|---:|---:|
| Collision move | 0.02556 | 0.02453 | 209.0 / 225.9 |
| Kirigakure effect update | 0.01375 | 0.01315 | 1165.9 / 200.4 |
| Unit remove | 0.00772 | 0.00401 | 272.5 / 60.3 |
| CPU class function | 0.00643 | 0.00432 | 33.7 / 50.8 |
| Shot hitbox update | 0.00582 | 0.00276 | 65.5 / 148.0 |
| Model bounds from joints | 0.00469 | 0.00479 | 56.4 / 102.2 |

Kirigakure runs about nine times/frame (27502 / 15561 calls); collision move
runs once/frame. Hook scopes include native callees and overlap unit timings.
The isolated host Kirigakure maximum alone does not establish a repeated spike.

### Snapshot and restore detail

Every timed snapshot reports **379 regions and 1837.698242 KiB**.
There are 3015 host saves and 1733 client saves.

| Snapshot bucket | Host total ms | Client total ms |
|---|---:|---:|
| Total instrumented save | 928.639 | 472.094 |
| Copy regions plus camera/post capture | 564.902 | 319.257 |
| Shot-recipe interval | 227.321 | 74.762 |
| Collision clones plus parent-reference capture | 115.425 | 66.279 |
| Region construction | 18.043 | 10.035 |
| Recycle/allocation | 2.177 | 1.330 |
| Metadata | 0.771 | 0.431 |
| Character-shot references, nested in recipe interval | 31.808 | 18.041 |

Mean instrumented save: **0.30801 / 0.27241 ms**. Copy interval accounts for
60.83% / 67.63% of instrumented save time. `shot_recipes_ms` also includes render
state collection, character-shot references, and effect capture; it is not an
exclusive timer for `Shot_CaptureRecipes`. Likewise, `copy_regions_ms` includes
`CaptureActionSideCameraState` and `CaptureCharacterPostState`.

| Restore bucket | Host total ms | Client total ms |
|---|---:|---:|
| Total | 727.683 | 346.922 |
| Shots/effects | 374.748 | 136.504 |
| Regions/character policies | 255.771 | 147.571 |
| Stale pointers | 55.410 | 38.287 |
| Collision clones | 14.248 | 8.490 |
| UI/final repairs | 13.788 | 7.609 |
| Setup | 12.505 | 7.764 |
| Owner pointers | 0.755 | 0.421 |
| Hit solver | 0.306 | 0.188 |
| Effect slots | 0.153 | 0.087 |

Mean restore cost: **1.6279 / 1.3712 ms/window**.
Shots and regions contribute 51.50%/35.15% on host and 39.35%/42.54% on client.

| Shot restore sub-bucket | Host total ms | Client total ms |
|---|---:|---:|
| Respawn, inclusive | 218.217 | 69.683 |
| Concrete overlay, nested | 142.704 | 41.755 |
| Pointer scan, nested in concrete overlay | 84.407 | 26.893 |
| Embedded-object preservation, nested | 42.250 | 10.544 |
| Retire effects | 51.738 | 26.117 |
| Destroy shots | 47.518 | 9.852 |
| Native factory, nested in respawn | 36.127 | 11.542 |
| Pre-sanitize | 21.005 | 11.593 |
| Pin resources | 18.532 | 9.697 |
| Reference cache | 18.289 | 10.590 |
| Register, nested in respawn | 17.252 | 4.654 |
| Post-sanitize | 9.233 | 4.915 |
| Finalize | 4.075 | 2.117 |
| Sanitize effects | 3.266 | 1.888 |
| Collect shots | 0.094 | 0.052 |

Mean current/saved shots per window: **3.774/3.743 host**, **1.225/1.225 client**.
Effect visits/window: **36.98 / 34.74**; sanitizer calls/window: **4.73 / 4.67**;
generator nodes/window: **33.06 / 31.39**. Do not compare raw host/client totals
as if the object workloads were equal.

Post-resim is separate: host **207.003 ms / 447 entries = 0.46309 ms/entry**;
client **96.774 ms / 253 = 0.38251 ms/entry**. Snapshot saves account for
206.725 / 96.595 ms, input resolution 0.278 / 0.179 ms, notification zero.

## Candidate 1 — omit immediately discarded concrete deep copies

### Hotspot evidence

Parent interval: `snapshot.shot_recipes_ms`, **227.321 / 74.762 ms**,
**0.07470 / 0.04272 ms/replayed frame**, or **24.48% / 15.84%** of instrumented
snapshot time. Both sides exceed the subsystem investigation threshold.
Recipe captures: **11355 / 2101**, or **3.732 / 1.201 per replayed frame**.
Maximum interval/window: **2.0174 / 3.4763 ms**.

### Investigation

- Source: `UMVC3Hook/code/rollback_shot.cpp:513-575`, especially line 526.
- Source reset: `UMVC3Hook/code/concrete_snapshot.cpp:999-1015`.
- Ghidra: `Shot_CaptureRecipes` at `0x3bd3723c0`;
  `ConcreteShotSnapshot_Capture` at `0x3bd37a8f0`.

`RollbackShotRecipe saved = recipe->second` deep-copies the body, collision
primitive vector, and each callback's generator-state vector. Capture immediately
executes `out = {}`, destroying those copied allocations, before rebuilding the
same destination from the current shot. Ghidra shows explicit `operator_new`,
`memmove`, vector element copies, followed by `operator_delete` in capture's reset.
LTO has not removed the waste.

The expensive case is a reconstructed shot: `Shot_TrackReconstructed`
(`rollback_shot.cpp:652`) retains populated concrete data in the live recipe.
That original data is still used by hitbox repair and must remain intact.

Proposed change: construct a capture recipe from metadata without copying the
old `concrete` member. Preserve all spawn arguments, references, sequence,
reconstructed flag, and pre-spawn RNG arrays. Keep flight capture, fresh concrete
capture, linked-list order, sort order, and all native calls unchanged. Avoid
temporarily moving data out of the live map, which would change observable state
under reentrancy. Prefer an explicit metadata-copy helper over raw partial-object
`memcpy` around a nontrivial C++ member.

This removes mod-owned temporary allocation/copy/free operations; it need not
change native lifetime, reference counts, RNG draws, locks, or captured payloads.
Allocator-address effects still warrant snapshot and desync validation.

Maximum attributable opportunity is **less than 0.07470 / 0.04272 ms/frame**:
the parent interval includes necessary capture and unrelated work. No precise
saving can be estimated from the existing fields.

### A/B result and verdict

**Not run; candidate remains unverified.** Per-window recipe-interval medians
are **68.257 / 38.167 us/save**, with 10%-trimmed means **70.076 / 39.286**.
Normalize by reconstructed-recipe count and copied concrete bytes if available;
recipe count alone misses the populated-versus-empty old snapshot distinction.
If existing buckets cannot resolve this, add only a focused timer/counter for
the discarded-copy path and establish a same-instrumentation baseline.

## Candidate 2 — skip equal qwords in shot pointer preservation

### Hotspot evidence

`shot_restore.concrete_pointer_scan_ms`: **84.4065 / 26.8928 ms**,
**0.02774 / 0.01537 ms/frame**. This is **59.15% / 64.41%** of concrete-overlay
time and **11.60% / 7.75%** of complete restore time; host exceeds the restore
subsystem threshold. Saved shots: **1673 / 310**. Maximum scan interval/window:
**0.9199 / 0.6738 ms**.

### Investigation

- Source: `UMVC3Hook/code/concrete_snapshot.cpp:475-487,1175-1208`.
- Ghidra: `ConcreteShotSnapshot_Restore` at `0x3bd37bc90`; helper is inlined.
- Query implementation: `rollback_memory.cpp:96-143` and `IsAccessibleRange`.

Every eight-byte lane starting at shot offset `0x50` tests the live and saved
values with `IsMappedPointer`, potentially entering the epoch-synchronized
memory-region cache and `VirtualQuery`, then copies the live qword if either
looks mapped. The optimized binary contains that loop without an equality exit.

Proposed local fast path:

```cpp
if (liveValue == savedValue) continue;
```

For equal qwords, either outcome of the original classification leaves the
overlay with exactly the same eight bytes. For unequal values, retain the
original predicate and write behavior. Retain the later embedded-object,
ownership, scalar-range, collision, and reference repairs.

No native object is skipped, dereferenced through a new path, retained, or freed
by this shortcut. Removed side effects are memory-query cache population and
epoch synchronization on those equal lanes. Check the remaining cache consumers
and final output in validation; do not infer unchanged timing solely from fewer
queries. The maximum possible saving is below the entire scan cost above, and
the equal-lane fraction is not recorded in this baseline.

**Do not generalize this shortcut to character-tail preservation.**
`RestoreDetectedCharacterTailScalars` at `concrete_snapshot.cpp:532` preserves
neighboring selector bytes when it finds a pointer, even when the pointer value
is unchanged. Omitting that classification can change gameplay state.
Likewise, the broad character pointer array has a 512-entry bound; removing
equal entries there could change which subsequent entries are preserved.

### A/B result and verdict

**Not run; candidate remains unverified.** Among windows with saved shots,
median scan cost/shot is **46.625 us host** (419 windows) and **80.700 us client**
(236 windows). Trimmed means are **48.982 / 91.208 us**; P95 **86.830 / 152.425 us**.
These differences demonstrate that shot count alone does not equalize workload.
Compare within each peer and stratify by shot class/body size and pointer mix.

## Other measured opportunities and limits

### Shot snapshot capacity reuse — follow-up after candidate 1

`slot.shotRecipes.clear()` destroys nested vector allocations at every save, and
`ConcreteShotSnapshot_Capture` resets its output with `out = {}`. Ghidra confirms
the destructor/free loop in `Shot_CaptureRecipes` and reset frees in capture.
Top-level vector capacity retention therefore does not retain shot body or
callback-vector storage. Reusing slot-owned payload storage could reduce capture
allocation churn, but requires a careful reset of every field and handling of
changing shot count/order. Benchmark it separately from eliminating the
discarded deep copy. It shares the same parent bucket and is not an additive
saving estimate.

### Region restore — measure the character-tail component next

`restore.regions_ms` consumes **0.08405 / 0.08433 ms/frame**, approximately
**0.5722 / 0.5833 ms/window**. Median per-window cost is **544.1 / 551.3 us**.
It includes generic copying and substantial character-specific policy work.

Ghidra `Rollback_RestoreSnapshot` at `0x3bd35d0d0` and
`ConcreteCharacterSnapshot_RestoreFields` at `0x3bd37d960` confirm a generic
pointer scan, model-state preservation, explicit gameplay overrides, a CPU-vtable
scan, and concrete-tail masking with temporary allocations. The recovered code
already recognizes snapshot/shot structure types at relevant interfaces.

A targeted timer around `ConcreteCharacterSnapshot_RestoreFields` would distinguish
tail-mask allocation/discovery from the broad region copy and fixed-field repair.
The existing region timer does not justify attributing all 403.342 ms to those
allocations. No concrete change is recommended here before isolating the cost.

### Broad snapshot copying

The copy interval costs **0.18564 / 0.18243 ms/frame**, with constant region and
byte counts. It is large, but the prior report
`doc/rollback_optimization_results.md` already records unsuccessful adjacent-copy
coalescing and SSE2 non-temporal-store experiments. Those historical results are
not a current A/B test, but generic memcpy replacement is a weaker first target
than the confirmed redundant shot work. Narrowing coverage requires independent
state-equivalence evidence.

## Verification plan

Implement and measure one candidate at a time, starting with candidate 1 or the
smaller candidate-2 equality gate. Match the supplied 4000-frame configuration,
roster/stage, delay, multi-resim, and build settings. Establish missing build
provenance before treating a comparison as controlled.

1. Build with consistent profiling and run `git diff --check`.
2. Compare target buckets per natural unit and per-window distributions for
   host/client separately; bootstrap windows with a fixed seed when useful.
3. Preserve region count/bytes, recipe counts, spawn order, full captured shot
   payloads, ownership links, and ground-truth checksums.
4. Check whole rollback and post-resim costs for displaced work. Snapshot changes
   affect post-resim and live capture too.
5. Repeat dual-CPU completion and focused projectile/assist/lifetime cases from
   `doc/ai_desync_testing_spec.md`, including the baseline roster's shots and
   reconstructed-shot hitbox repair. Use actual emitted numeric command IDs.
6. Retain a candidate only after attribution and determinism checks pass.

No A/B confidence interval or percentage improvement is claimed here. Ghidra
comments on the two candidate functions link to this report. Only this report
and those analysis comments were retained; application source is unchanged.

## Follow-up: direct inspection of the optimized Timestone binary

The user requested a deeper binary inspection after the initial report. This
pass started from Ghidra decompilation and disassembly, then checked the matching
source for bounds and semantics. It identifies additional candidates, rather
than treating source-level constructs as proof of runtime overhead.

### Priority 1: collapse repeated emulated-TLS resolution in memory validation

**Function:** `GetCachedMemoryRegion`, `0x3bd3493a0`.

The steady-state, last-region-hit path does the following *before* checking the
cached address interval:

| Instruction address | Operation |
|---|---|
| `0x3bd3493c1` | `CMPXCHG.LOCK` reads the global invalidation epoch |
| `0x3bd3493ce` | `__emutls_get_address`: thread's observed epoch |
| `0x3bd3493dd` | `__emutls_get_address`: last-region index |
| `0x3bd3493ec` | `__emutls_get_address`: TLS initialization guard |
| `0x3bd3493fb` | `__emutls_get_address`: region vector |
| `0x3bd349430` | Additional TLS resolution on epoch mismatch: replacement index |

`__emutls_get_address` at **`0x3bd464a30`** is not a single native TLS load.
Its initialized multithread path calls:

```text
0x3bd464a87  GetLastError
0x3bd464a8f  TlsGetValue
0x3bd464a9b  SetLastError
```

The binary's `_CRT_MT` storage at `0x3bd516d80` is initialized to **2**, selecting
that multithread path unless modified at runtime. Thus a normal memory-cache
lookup has **four emulated-TLS calls and twelve Windows API calls**, even when
`VirtualQuery` is completely avoided. This is a static instruction/call-path
count, not a measured estimate of elapsed time or cache-hit frequency.

**Proposed optimization:** put `regions`, `epochSeen`, `lastRegion`, and
`nextReplacement` in a single thread-local state object; obtain a local reference
once and use its members throughout the lookup. Keep the vector initially so
that changing storage policy remains a separate experiment. Inspect the rebuilt
assembly: a nontrivial TLS object may still have an extra guard lookup, so do not
assume the compiler automatically reduces four calls to one.

Keep the locked epoch check, invalidation events, per-thread isolation, cache
capacity/replacement behavior, accessibility/executable decisions, range checks,
and failed-query behavior intact. This candidate removes repeated access to
thread-local storage, not synchronization. The existing epoch read is expensive
but is not included in the proposed removal.

**Reach:** Ghidra callers include `IsAccessibleRange`, `HasCallableDtiVirtual`,
`hkMtObjectIsAlive`, effect destruction/retirement, shot sanitation, and concrete
character restore. The shot overlay reaches it through `IsAccessibleRange`.
This therefore affects mod-owned overhead both around native ticking and during
restore, rather than only one shot function.

**Measured scope:** the baseline shot pointer-scan interval contains
**84.4065 / 26.8928 ms host/client**; other consumers occupy several overlapping
phases. The profile does not isolate TLS cost or count cache lookups, so neither
the entire pointer-scan interval nor summed consumer times can be claimed as
recoverable TLS overhead. Prefer this structural change as the next broad-reach
candidate; normalize its first experiment by equivalent query workload.

**Verdict:** concrete binary-level candidate, performance unverified.

### Priority 2: replace small transient character hash sets

#### Character-shot reference capture

**Function:** `Rollback_CaptureCharacterShotReferences`, **`0x3bd355ab0`**.

The optimized binary retains a temporary `unordered_set`, node insertion,
bucket allocation/rehashing, hash division and pointer chasing for region
membership, and node/bucket destruction. Examples include a collision-chain
`DIV R11` at **`0x3bd355c66`** and the node-delete loop at
**`0x3bd355c90..0x3bd355ca3`**. These operations are not optimized away.

The set's input has a hard existing bound: **two team traversals of at most 16
nodes plus a category traversal of at most 64**, totaling at most **96 candidate
pointers**. A stack-backed pointer list can represent the same membership set
without node or bucket allocation. Keep all existing traversal caps and region
order; do not use a hardcoded six-character capacity. Benchmark linear membership
against the hash table rather than assuming comparisons are free.

The whole function has an existing timer:

| Metric | Host | Client |
|---|---:|---:|
| `character_shot_references_ms` | 31.8083 | 18.0414 |
| Snapshot saves | 3015 | 1733 |
| Entire function us/save | 10.550 | 10.411 |
| Entire function ms/replayed frame | 0.01045 | 0.01031 |

That interval also captures and resolves references, so the hash-table saving
must be smaller. It is a lower-priority constant-work candidate; the table cost
alone has not been shown to exceed the spec's quantitative threshold.

#### Collision-parent reference capture

**Function:** `CaptureCharacterCollisionParentReferences`, **`0x3bd375150`**.

This function independently builds another hash set, used only to deduplicate
the two capped team traversals. The binary contains `operator_new(0x10)`, prime
rehash policy calls, and hash divisions. Its maximum input is **32 candidate
pointers**, so a 32-entry stack deduplication list can preserve first-encounter
order without heap allocation.

Preserve every recorded null/self/weapon parent and every unclassified-parent
decision. These records drive restore; reducing their number is not the
optimization. The enclosing `collision_clones_ms` interval costs
**115.4246 / 66.2786 ms**, but also includes required native cloning. The set's
exclusive cost is unmeasured. Test this separately from the previous set change.
Do not combine character discovery across snapshot phases until intervening
native callbacks are shown not to change the traversed topology.

**Verdict:** two bounded-container candidates; savings unverified and likely
smaller than the broad TLS opportunity.

### Priority 3: remove cleanup-cache allocation churn without changing freshness

**Functions:** `Rollback_ClearStalePointers`, **`0x3bd33f9b0`**, and
`ClearModelMaterialCache`, **`0x3bd33b7a0`**.

These functions use a separate, per-call `MemoryRegionCache` from
`rollback_cleanup.cpp:40`. This is not the epoch-controlled TLS cache above.
Ghidra shows its `Get()` inlined repeatedly, including full vector-growth,
`operator_new`, copying, and deletion paths. Every hit searches from the
beginning of the vector. The vector is destroyed at the end of the cleanup pass.

The narrow allocation experiment is a small inline backing store with a dynamic
overflow fallback, retaining a freshly empty cache each pass and the original
insertion-order lookup. Do not introduce a fixed capacity that silently drops
regions, and do not change it into a cross-frame cache. A last-hit shortcut is a
separate lookup-policy experiment, not part of the allocation-only candidate.

Baseline parent `restore.stale_pointers_ms`:
**55.4096 / 38.2866 ms**, or **0.12396 / 0.15133 ms/window** and
**0.01821 / 0.02188 ms/replayed frame**. This is **7.61% / 11.04%** of total
restore. Only part of it is container overhead; preserve all memory checks,
material/weapon traversal, DTI dispatch, and writes clearing invalid references.

**Verdict:** plausible container optimization inside a measured restore hotspot;
exclusive allocation/lookup cost still needs isolation.

### Findings that do not justify an optimization patch

- `HeadlessUnitPass<false>` at `0x3bd36eae0` already delegates most allowed
  categories to native category functions; category 13 has special filtered
  lifecycle handling. Its indirect dispatch and cleanup are not evidence of
  redundant gameplay updates.
- `Rollback_HeadlessTick` at `0x3bd36f300` calls the `<false>` unit-pass
  specialization. This loaded path is profiling-disabled; supplied profiling
  timings are workload evidence, not exact timings of these instructions.
- Freeze breadcrumbs retain `GetCurrentThreadId` and `GetTickCount64` calls.
  Their production cost is not isolated. The optimization specification requires
  a dedicated control measurement before optimizing diagnostics.
- The region-cache hit path in `Rollback_BuildRegions` already consists of
  dependency comparisons and manifest copy. Old reports about multi-millisecond
  region rebuilding do not describe this compiled fast path.
- `Shot_PrepareReferenceResolution` already avoids building the full stable-unit
  set when no saved spawn reference needs it. An unconditional "skip this scan"
  proposal would overlook that existing gate.

### Ghidra analysis improvements and validation state

Created and applied **`RB_CachedMemoryRegion`**, a verified 24-byte structure:
start at `+0`, end at `+8`, accessible at `+16`, executable at `+17`, then padding.
Applied it as `GetCachedMemoryRegion`'s return type. Corrected
`IsAccessibleRange` to return `bool` and `__emutls_get_address` to return `void*`.
This removes misleading integer/pointer inference in the validation call chain.
Added function comments with instruction addresses, proposed invariants and
baseline scope.

No candidate code, build or runtime comparison was performed in this follow-up.
All new candidates remain unverified. The recommended first experiment is
**aggregating the memory-query TLS state**, followed by the previously identified
discarded shot deep copy or the smaller bounded-set changes.
