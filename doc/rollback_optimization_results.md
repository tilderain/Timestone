# Rollback Optimization Results

## Stack-Backed Region Dependency Cache

### Test Configuration

```text
Commit: 7eae058a7c2edbedefe5c87c35b303ee39a8aa0d (dirty profiling build)
Profiling enabled: yes
Roster/stage: unchanged saved dual-training configuration
CPU mode: both teams
Multi-resim: enabled
Target fighting frames: 1000
Baseline: rollback_profile_runs/20260823T070820.341980Z_candidate_direct_snapshot_regions_repeat
Candidate: rollback_profile_runs/20260823T072140.682190Z_candidate_dependency_stack_cache
```

### Hotspot Evidence

`RegionDependenciesValid()` created and grew a temporary `std::vector` on every
snapshot to cache fresh `VirtualQuery` results. The optimized baseline spent
74.6111 ms in `build_regions_ms` across 1,460 snapshot saves, or approximately
51.1 microseconds per save. Region construction represented about 12% of the
measured snapshot path.

The candidate replaces only that transient vector with a bounded stack array
and a last-hit index. It retains fresh memory queries, the same address ranges,
the same accessibility decisions, and the same dependency value comparisons.
It does not change captured state or native gameplay behavior.

### A/B Result

Every profile reported exactly 330 regions and 1540.157 KiB per snapshot.
Aggregate rollback time was not used to judge this candidate because native
action and projectile workloads differed substantially between runs.

| `build_regions_ms` per save | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host weighted mean | 50.989 us | 47.966 us | -5.93% |
| Client weighted mean | 51.213 us | 49.458 us | -3.43% |
| Host median | 48.782 us | 44.438 us | -8.91% |
| Client median | 48.400 us | 44.217 us | -8.64% |
| Host 10% trimmed mean | 49.503 us | 45.974 us | -7.13% |
| Client 10% trimmed mean | 49.154 us | 45.185 us | -8.07% |

A fixed-seed, 10,000-iteration rollback-window bootstrap produced these 95%
intervals for the normalized change:

| Statistic | Host | Client |
|---|---:|---:|
| Median | -14.63% to -1.58% | -14.77% to -2.04% |
| 10% trimmed mean | -12.99% to -1.34% | -13.15% to -2.04% |

The weighted-mean intervals included zero because a small number of timing
outliers dominated that statistic. Both robust measures agree across peers and
exclude no change.

### Correctness

- Both peers reached the 1,000-frame limit and exited normally.
- No crash artifacts were produced.
- No desync artifacts were produced.
- Snapshot region count and byte size were unchanged.

### Verdict

`optimization_verified` for the narrow region-dependency validation bucket.
The candidate is retained. No whole-run speedup is claimed because unrelated
native gameplay variance dominated aggregate rollback time.

## Command-Line Region Metadata

### Test Configuration

```text
Commit: 7eae058a7c2edbedefe5c87c35b303ee39a8aa0d (dirty profiling build)
Profiling enabled: yes
Roster/stage: unchanged saved dual-training configuration
CPU mode: both teams
Multi-resim: enabled
Target fighting frames: 1000
Baseline: rollback_profile_runs/20260823T072140.682190Z_candidate_dependency_stack_cache
Candidate: rollback_profile_runs/20260823T073954.381616Z_candidate_command_region_metadata
```

### Investigation

Restore setup previously rediscovered command-line regions by comparing every
owned snapshot region against both command offsets for every team character.
Snapshot construction already identifies these regions when it captures the
two `cChrCmd` line buffers per character.

The candidate stores that identity in an existing padding byte of
`RollbackRegion`. Restore then reads the live command-line count directly for
marked regions. Captured bytes, region size, owner validation, allocation-count
restoration, and command state are unchanged.

### A/B Result

| `restore.setup_ms` per window | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host median | 39.800 us | 32.100 us | -19.35% |
| Client median | 42.400 us | 32.350 us | -23.70% |
| Host 10% trimmed mean | 43.930 us | 29.764 us | -32.25% |
| Client 10% trimmed mean | 42.341 us | 31.939 us | -24.57% |

A fixed-seed, 10,000-iteration rollback-window bootstrap produced these 95%
intervals for the normalized change:

| Statistic | Host | Client |
|---|---:|---:|
| Median | -37.64% to -8.68% | -34.86% to -10.44% |
| 10% trimmed mean | -42.07% to -22.94% | -32.13% to -12.78% |

The client arithmetic mean was dominated by a small number of setup outliers;
its median and trimmed distribution agree with the host result.

### Correctness

- Both peers reached the 1,000-frame limit and exited normally.
- No crash artifacts were produced.
- No desync artifacts were produced.
- Snapshot region count and byte size were unchanged.
- Command-line ownership and live allocation-count restoration remain active.

### Verdict

`optimization_verified` for restore setup bookkeeping. The candidate is
retained. The combined rollback result improved 4.40%, but no whole-run claim is
made from one battle because native gameplay workload still varied.

## Collision Clone Child Validation

### Investigation

Snapshot profiling now records collision-wrapper, child, copy, creation, and
destruction counts. The measured roster captured exactly 28 external-animation
wrappers per save, approximately 127 children, and approximately 16 native deep
copies. Each ring slot created only six clone roots over the entire run and
destroyed none, confirming that root allocation and ownership reuse were
already effective.

The capture path nevertheless called `HasCallableDtiVirtual()` for every child
immediately before passing the same graph to native `copyFrom`. The restore path
repeated that check for every child immediately after native `copyFrom` created
the graph. Native copying already requires those children to be callable, so
the extra `VirtualQuery`-based passes provided no additional safety. The
retained change reads each child DTI directly while preserving native clone
creation, deep copying, destruction, scalar state, child order, and post-copy
DTI comparison.

### Capture Result

Baseline:
`rollback_profile_runs/20260823T081719.700858Z_baseline_clone_workload`

Candidate:
`rollback_profile_runs/20260823T081855.541526Z_candidate_direct_clone_child_dti`

| Collision-clone capture per save | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host median | 125.807 us | 54.817 us | -56.43% |
| Client median | 130.471 us | 55.486 us | -57.47% |
| Host 10% trimmed mean | 126.191 us | 55.225 us | -56.24% |
| Client 10% trimmed mean | 131.169 us | 56.794 us | -56.70% |

The 95% bootstrap intervals were `-59.31%` to `-53.40%` for the host median
and `-59.64%` to `-53.39%` for the client median. Wrapper, child, and deep-copy
workloads remained equivalent between runs.

### Restore Result

Baseline:
`rollback_profile_runs/20260823T081855.541526Z_candidate_direct_clone_child_dti`

Candidate:
`rollback_profile_runs/20260823T082033.654484Z_candidate_direct_restored_child_dti`

| Collision-clone restore per window | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host median | 99.700 us | 45.400 us | -54.46% |
| Client median | 95.400 us | 42.600 us | -55.35% |
| Host 10% trimmed mean | 102.210 us | 44.488 us | -56.47% |
| Client 10% trimmed mean | 102.448 us | 43.040 us | -57.99% |

Both median bootstrap intervals excluded no change. Both runs completed on both
peers without crashes or desync artifacts. The two changes are retained.

## Snapshot Copy Experiments

The snapshot contains exactly 330 regions and 1540.157 KiB per save. Two
byte-for-byte copy strategies were tested without changing coverage or region
metadata:

- Coalescing adjacent source regions produced `-0.48%` to `+0.97%` median
  changes across peers, with all robust confidence intervals including zero.
- SSE2 non-temporal stores for regions of at least 128 KiB produced median
  changes of `+0.97%` on both peers and did not improve trimmed means.

The optimized CRT `memcpy` path remains faster or equivalent for this workload,
so both snapshot-copy candidates were reverted.

## `MtObject::isAlive` Vtable Validation Cache

### Investigation

Temporary call-site profiling showed that the rollback safety hook for
`MtObject::isAlive` runs approximately 589-598 times per replayed frame. The
hook must still reject unmapped objects and the shorter base `MtObject` vtable,
because native callers treat a true result as permission for another virtual
dispatch. However, the vtable and `getDTI` executable target are immutable game
module metadata and were being revalidated on every call.

For callers that require a vtable proof, the retained change checks the object
allocation, rejects the base vtable, and uses a 256-entry thread-local
direct-mapped cache only for vtables that have already passed the complete
range and executable checks. Unknown or colliding vtables still take the
original validation path.

Baseline:
`rollback_profile_runs/20260823T085430.224674Z_baseline_lifetime_hook_validation`

Candidate:
`rollback_profile_runs/20260823T085909.589795Z_candidate_is_alive_vtable_cache`

| `isAlive` hook cost per call | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host median | 0.7189 us | 0.3873 us | -46.12% |
| Client median | 0.6883 us | 0.3864 us | -43.86% |
| Host 10% trimmed mean | 0.7307 us | 0.3895 us | -46.69% |
| Client 10% trimmed mean | 0.7018 us | 0.3880 us | -44.71% |

All robust bootstrap intervals excluded no change. At the baseline call rate,
the reduction is approximately 0.19-0.21 ms per replayed frame, or about
1.3-1.4 ms for a typical 6.6-frame rollback window. Both peers completed
without crash or desync artifacts.

### Active Character Caller Scope

Caller profiling found that return address `0x14005629E` in
`cChr__isMoveActive` accounts for approximately 57% of rollback-time
`MtObject::isAlive` calls. Ghidra confirms that this caller passes its current
`cChr` `this` pointer and immediately reads more fields from the same character.
The global allocation and vtable proof therefore cannot make this specific
caller safer. It now calls the original state predicate directly, while all
other callers retain the cached vtable validation above.

Baseline:
`rollback_profile_runs/20260823T085909.589795Z_candidate_is_alive_vtable_cache`

Candidate:
`rollback_profile_runs/20260823T091816.959237Z_candidate_trust_active_character_is_alive`

| `isAlive` hook cost per call | Baseline | Candidate | Change |
|---|---:|---:|---:|
| Host median | 0.3873 us | 0.3064 us | -20.90% |
| Client median | 0.3864 us | 0.3199 us | -17.21% |
| Host 10% trimmed mean | 0.3895 us | 0.3115 us | -20.04% |
| Client 10% trimmed mean | 0.3880 us | 0.3235 us | -16.61% |

All robust bootstrap intervals excluded no change. This saves approximately
0.04-0.05 ms per replayed frame, or 0.29-0.36 ms for a typical rollback
window. Both peers completed without crash or desync artifacts.
