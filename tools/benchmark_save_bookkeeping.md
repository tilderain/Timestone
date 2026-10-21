# Save Bookkeeping Experiments

## Scope

Investigation of the six proposed save optimizations, followed by implementation
of bounded sets and reusable shot/effect buffers. **These are not before/after
timings of the complete Rollback_SaveSnapshot.**
The observed live-game 0.25 ms remains unexplained without its stage breakdown.

## Production Follow-Up

Production now uses `RollbackCaptureSet` for the two capture-local sets, reuses
shot/effect recipe entries, and calls capacity-preserving `ResetForCapture`.
Recipe `AssignCaptureMetadata` methods copy metadata without touching nested
capture buffers. Filtering, traversal limits, shot sorting and removal of surplus
entries are unchanged. Camera refcounts, native collision clones, shared inventory
and region metadata caching were not changed.

The harness now directly uses those production types/methods and extracts the
production recipe-entry setup blocks. Its baseline reconstructs the former hash
set and destructive-reset policies; recipe cases include metadata copying and
entry reuse, unlike the original storage-only experiment below.

An initial implementation that moved the entire concrete state aside during
metadata assignment regressed callback-free shots (twelve shots: 31.43 to 42.92
us). It was replaced with metadata-only assignment before completing the change.

Updated optimized run: `/tmp/opencode/save-bookkeeping-6e6tzodh`.
Final ASan/UBSan run: `/tmp/opencode/save-bookkeeping-4uikm79z`.
`make -j8` and `git diff --check` passed after the production changes.

| Recipe Storage Case | Former Policy (us) | Production Policy (us) |
| --- | ---: | ---: |
| One shot, no callbacks | 2.430 | 1.351 |
| Twelve shots, no callbacks | 29.334 | 18.504 |
| One shot, thirteen callbacks | 3.225 | 1.707 |
| Twelve shots, thirteen callbacks | 49.679 | 22.228 |
| Twelve effects | 1.930 | 0.634 |

Hash-removal timings remain small and sensitive to the surrounding harness:
low-load 10.23 to 8.23 us in the preceding build, but 9.13 to 9.60 us in this run.
The allocation removal is real; a stable in-game speedup is not established.
Do not sum these microbenchmarks or apply their percentages to the full save.

Additional checks cover actual metadata assignment with nonempty source states,
sorting, shrink-to-zero, stable buffer addresses across invalid/valid captures,
and rejecting a set insertion beyond its traversal budget. Native geometry and
generator work still uses the fixture substitutes described below.

The remaining sections record the original exploratory measurements.

The runner extracts current collision-parent capture, render-state capture,
character-set construction, concrete shot/effect capture, and region dependency
validation from source. It records input hashes, generated code, compiler command,
hardware, raw per-trial CSV and check output under `/tmp/opencode`.

```sh
python3 -B tools/benchmark_save_bookkeeping.py --iterations 20000
python3 -B tools/benchmark_save_bookkeeping.py --iterations 100 --sanitize
```

Requires the existing `benchmark_subsystem_restore.py` extraction helpers, GCC,
and `taskset`. This does not build, copy or load the plugin.

## Candidates

| Proposal | Investigation / Measurement |
| --- | --- |
| Shared character/weapon inventory | Measured shared character discovery for collision parents, render capture and shot-reference membership. Weapon discovery remains in the original consumers; effects and full reference recipe creation are excluded. |
| Remove transient hash sets | Measured independently using fixed-capacity linear sets, retaining the original separate traversals. |
| Reuse recipe storage | Measured reusable concrete snapshot entries plus capacity-preserving reset. Native geometry/generator capture is substituted, so only storage-policy benefits can be inferred. |
| Repeated parent classification | Measured an additional last-parent/weapon-index cache per character. Unknown parents remain omitted and null/self parents retain their original handling. |
| Camera refcount churn | Inspected, not timed. Native add/release callbacks cannot be meaningfully benchmarked with no-op substitutes. |
| Cached region metadata | Measured cached total bytes while retaining current dependency validation. Offsets need not be cached for the existing sequential copy loop. |

Character discovery must preserve differing semantics:

- Collision parents: roster only, up to 16 nodes per team, deduplicated.
- Render capture: up to 8 nodes per team plus 64 unit-list entries; output models are deduplicated by the original code.
- Shot references: up to 16 nodes per team plus 64 unit-list entries; only sufficiently large regions reach the membership lookup.
- Effect capture has additional roster-specific behavior, not covered by the shared-inventory candidate.

The fixture uses six roster characters, two weapons each, and eight assumed
large noncharacter membership misses. Workload 0 has null collision parents;
workload 1 repeatedly references the last weapon. Workload 2 adds twelve helper
characters and uses sixteen weapons per character with mixed null, self, weapon
and unknown parents. These counts are assumptions, not observed game telemetry.
Small regions are excluded from membership queries, matching production's
size-check short circuit. An exploratory run that queried all small regions was
discarded because that unfairly penalized the linear candidate.

## Results

Linux GCC `-O3`, pinned to one permitted CPU, nine alternating-order trials,
20,000 saves per trial, 64 warmup saves per candidate. Snapshot storage rotates
through sixteen slots. Values below are **microseconds per isolated batch/save**,
not milliseconds and not the whole game save. Native ABI, allocator, Wine and
cache behavior can differ.

Final run: `/tmp/opencode/save-bookkeeping-2k_s24d4`.
Repeat with the same timing cases and earlier checks:
`/tmp/opencode/save-bookkeeping-ri898fkz`.

| Bookkeeping Workload | Baseline | Shared Inventory | Inventory + Parent Cache | Hash Removal Only |
| --- | ---: | ---: | ---: | ---: |
| Six characters, null parents | 9.970 | 8.457 | 8.419 | 8.546 |
| Six characters, repeated weapon parents | 10.368 | 9.684 | 9.607 | 9.778 |
| Helpers / many weapons / mixed parents | 62.941 | 62.534 | 65.252 | 61.168 |

The low-load shared-inventory delta was 1.51 us, versus 1.81 us on the repeat.
Hash removal alone saved 1.42 us, versus 1.76 us on the repeat. The distinction
between these candidates is not compelling enough to justify a larger shared
inventory integration. Busy-case ranges overlap substantially; the parent cache
is not a consistent win.

| Storage Workload | Baseline | Reuse | Saved |
| --- | ---: | ---: | ---: |
| One shot, no callbacks | 2.568 | 1.449 | 1.119 |
| Twelve shots, no callbacks | 25.534 | 17.188 | 8.346 |
| One shot, thirteen callbacks | 3.685 | 1.772 | 1.913 |
| Twelve shots, thirteen callbacks each | 44.099 | 21.963 | 22.136 |
| One effect | 0.148 | 0.055 | 0.093 |
| Twelve effects | 1.401 | 0.414 | 0.987 |

Shots use 0x3000-byte fixture bodies and four fixture collision primitives.
Callbacks/effects use four fixture generator-state records. Each batch uses the
same live source object for its entries, so this is a cache-friendly storage
experiment, not a representative native object graph. Actual concrete capture
functions are extracted, but DTI lookup, geometry discovery and generator capture
are fixture substitutes. Recipe metadata copying, sorting, reference lookup,
native effect eligibility checks and ownership callbacks are excluded. In
particular, this does not time the full `Shot_CaptureRecipes` or
`Effect_CaptureRecipes` functions.

The repeat saved 8.01 us for twelve callback-free shots and 24.13 us for twelve
fully populated callback shots. Zero-shot/effect cases are effectively empty
loops and showed no meaningful gain. Do not extrapolate active-shot savings to
an idle scene.

| Metadata Workload | Validate + Sum | Validate + Cached Total |
| --- | ---: | ---: |
| 64 dependencies / regions | 0.153 | 0.107 |
| 512 dependencies / regions | 1.339 | 0.719 |

Dependency addresses are contiguous fixture values, unlike a real scattered
ownership graph. Region-plan rebuild, metadata copying and full snapshot body
copies are excluded. Cached totals must be recomputed on plan rebuild and must
respect callers that request a truncated region list.

## Correctness Checks

Final ASan/UBSan run: `/tmp/opencode/save-bookkeeping-ztq3l8zo`.

- Compare ordered collision-parent and render-state records field by field.
- Compare membership outcomes for roster/helpers and noncharacters.
- Exercise empty and maximum roster traversal, duplicate entries, cyclic unit
  lists, null managers, invalid weapon counts, and unknown collision parents.
- Compare concrete snapshot body bytes, scalar fields, callback payloads and
  generator records through null, invalid-DTI and valid-object transitions.
- Exercise sixteen-slot reuse, growth, shrinkage, empty saves and source changes;
  compare all retained slots after each capture.
- Verify region dependency changes are rejected rather than bypassed by caching.

Fixture geometry generation is deliberately simple; checks do not establish
native capture equivalence or live-game determinism. Sanitizer timings are not
used for performance conclusions.

## Source Findings

`ConcreteShotSnapshot_Capture` begins with `out = {}` and
`ConcreteEffectSnapshot_Capture` begins with `state = {}`. Those resets discard
nested vector capacity. Simply keeping entries in `slot.shotRecipes` or
`slot.effectRecipes` is therefore insufficient. The isolated candidate retains
buffers while clearing every logical field, and reuses entries rather than
destroying/recreating them. A production implementation must also avoid undoing
this reuse when copying recipe metadata. Shrinking the vector still releases
removed entries; this experiment does not propose an unbounded object pool.

`CaptureActionSideCameraState` releases the old slot's resources before acquiring
the current ones. Avoiding unchanged-resource churn could be reasonable, but
resource transitions must still preserve one reference per occupied snapshot
slot. Parent/scalar state must refresh even when resource pointers are unchanged.
Native callback side effects and cross-field aliasing need validation before
altering this behavior. No speedup is claimed here.

Collision clone roots and child-DTI vectors already reuse storage. Native
`copyNode` still runs for populated wrappers, or to clear an existing clone when
the current wrapper becomes empty. Pointer/count equality cannot establish that
the geometry is unchanged. No native clone optimization was benchmarked.

## Recommendation

1. Prefer the smaller transient-hash-removal change over a shared inventory if
   pursuing the low-load bookkeeping gain. Recheck under the actual Windows
   build before deployment.
2. Prototype capacity-preserving concrete capture and reusable recipe entries
   for active-shot workloads. This is the strongest isolated saving, but requires
   real collision/generator and recipe-lifecycle validation.
3. Cached region totals are minor; last-parent caching is not worth integrating
   on these results.
4. Split the live save timer before attempting a large low-load reduction.
   Native collision cloning, full body copies, effect eligibility/capture,
   camera/resource management and snapdump remain outside these measurements.

Even if the measured 1.5 us bookkeeping reduction transferred directly, it would
be only about 0.6% of a 250 us save. These results do not justify predicting a
large reduction of the user's reported 0.25 ms.
