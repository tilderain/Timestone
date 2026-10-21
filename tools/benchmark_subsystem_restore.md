# Subsystem Restore Payload Benchmarks

Standalone experiment only. No production changes, plugin build, deployment,
native calls or claims of game-level determinism. Run with:

```sh
python3 -B tools/benchmark_subsystem_restore.py
python3 -B tools/benchmark_subsystem_restore.py --sanitize --iterations 100
```

The runner creates a unique directory under the existing `/tmp/opencode`, extracts
current kernels with unique anchors and expected schemas, and links the actual
`rollback_character_restore.cpp` in a separate translation unit without LTO.
Portable `-O3` is intentional: this is a host microbenchmark, not a plugin build.
Hashes, compiler command/version, CPU affinity, hardware, raw trial samples,
byte plans and correctness output are recorded there. No historical character
benchmark baseline is used.

## Scope

- Weapon: current scalar loop plus actual `Rollback_RestoreModelMotions` call.
  Fixture body is `0x15E0`; 12 weapons is an assumption, not a measured census.
  All motion bindings/cursors copied by current policy are retained in packing.
  Fixed candidate coalesces adjacent ranges, without copying across live gaps.
- Team: actual `CCHR_TEAM_RESTORE_RANGES` loop, including its duplicate 80-byte
  write. Candidate stores unique ranges plus the eight-byte active reference at
  `+0x48`. Active-reference copy/validation/list traversal is excluded on BOTH
  sides; packing retains the exact information needed for that existing repair.
  This is not a new stable-ID implementation. Two teams are assumed.
- Action: actual `SACTION_RESTORE_RANGES` loop against the current `0x300`
  compact region, not the disabled broad action capture. One action is assumed.
- Collision: actual `Rollback_RestoreExternalAnimBlock`, including forced zero
  at `+0x74`. Packing stores only the 81 source bytes used. 24 wrappers assumed.
  This measures ONE scalar image/pass, not both region and clone scalar images.
  Owner/DTI checks, native deep clone/copy/prune, child validation, vector/storage
  management and memory-query invalidation are excluded, not credited as savings.

Capture is the current raw region memcpy operation, with allocation and region
enumeration excluded equally. Fixed retains full capture; packed captures only
the packed representation. Restore-only inputs are prepared before timing.
Combined directly measures capture followed by restore, not a sum of independent
medians. Candidates never reconstruct or read a full saved body after packing.
Hot uses one snapshot slot; ring16 cycles 16 packed/full snapshot slots against
the same live objects. This is a payload working set, not the full game's cache
pressure. Nine trials alternate current/fixed/packed and packed/fixed/current;
warm-up, identical function-pointer dispatch, compiler fences and no hot-loop
allocations apply to all variants. Count times are measured batch time divided
by iterations; per-operation values divide that batch time by object count.

## Correctness

Full destination comparisons include untouched ownership bytes. Every source and
destination byte is independently mutated. Packed payload and retained team
reference roundtrip are checked; the original source is poisoned before unpack.
All shorter source lengths are tested. Fixed falls back to the current kernel
for partial bodies; weapon dispatch below `0x1300` is a no-op. Packed APIs accept
only exact record lengths and reject truncated capture/unpack without writing.
These APIs assume nonnull, nonoverlapping valid full-size destinations, as the
underlying kernels do. Sanitizers check actual short allocations as well.
Changing-byte saves reuse each of 16 slots four times and compare nonmonotonic
restores against full images. No native resource pointers are dereferenced by
these payload tests; borrowed motion references are compared as exact bytes.

## Not Measured

Projectile explicit restore: NOT MEASURED. `concrete_snapshot.cpp` currently
uses runtime DTI/size validation, a copied overlay, mapped-pointer filtering,
embedded-object scans of both timelines, class-specific ranges and native
collision accessors. Replacing those with a fixed schema requires auditing the
concrete layouts and native environment, not substituting a synthetic scan.
`rollback_restore.cpp` additionally pins resources, retires/destroys shots and
effects, sanitizes category lists, recreates/registers shots, repairs references,
restores RNG and evaluates poses. None of those costs disappear with packing.

Material controller/whole-job packing: NOT MEASURED. The reviewed eye-controller
subset in `rollback_character_restore.cpp` gates selectors/clocks on model,
material and per-controller binding equality. It does not define the full
`0xBB0`/`0xB90`/`0xB80` job schema. `rollback_snapshot.cpp` captures active job
tables coupled to seven pointer slots and packed counts. Native controller
transition/apply, color/UV jobs and callbacks mutate state and references;
observed unchanged table bytes do not establish immutability. No invented
whole-job scalar baseline or optimistic speedup is reported.

Full `sMain` and `sCollision` are already disabled. No savings are attributed to
them. External weapon animation/joint buffers and model-render-state repair also
remain outside this payload experiment. Integration would still require all
saved-body consumers to understand packed storage, correct resource lifetimes,
and matched gameplay/defeat/replacement and rollback-heavy two-peer tests.

## Results

Measured on AMD Ryzen 7 5800XT, Linux x86-64, GCC 11.3.0, pinned to logical CPU 0.
Portable flags: `-std=c++17 -Wall -Wextra -Wpedantic -fno-strict-aliasing -O3
-DNDEBUG`; no `-march=native`, LTO or plugin build. Nine alternating-order trials,
200,000 batches per trial. Frequency boost was enabled; CPU was not isolated.
This is one synthetic body size per subsystem, not a measured gameplay census.

Primary artifacts: `/tmp/opencode/subsystem-restore-cu9txgl1`.
ASan/UBSan artifacts: `/tmp/opencode/subsystem-restore-mp1sldwf`.
Both passed every correctness check. Sanitizer timings are **not** used below.
The initial 20,000-batch smoke run is in
`/tmp/opencode/subsystem-restore-2ybir_jm`; one longer attempt exceeded the shell's
120-second timeout and was discarded. The completed run used a longer timeout.
Artifact metadata hashes describe inputs at execution time; this results section
was written afterward. Production source and benchmark code were unchanged
between the completed performance and final sanitizer runs.

### Exact Timings

All entries are **milliseconds at the stated object count**, not whole-frame
time. Current = actual extracted current restore plus full capture; fixed =
coalesced literal copies plus full capture; packed = compact capture/unpack.
Capture and restore rows are independently measured. Combined is a separately
measured capture-then-restore batch, never their theoretical sum.
`results.csv` also contains per-operation nanoseconds/milliseconds and all nine
raw samples. Small differences should not be treated as statistically proven:
even the identical current/fixed full-capture kernel varies between trials and
buffer addresses.

| Subsystem (Count) | Working Set | Phase | Current ms | Fixed ms | Packed ms |
|---|---|---|---:|---:|---:|
| Weapon (12) | Hot | Capture | 0.003006663 | 0.002912626 | 0.004639942 |
| Weapon (12) | Hot | Restore | 0.002552277 | 0.004666927 | 0.004304144 |
| Weapon (12) | Hot | Combined | 0.005198901 | 0.007600489 | 0.009351576 |
| Weapon (12) | Ring16 | Capture | 0.003300833 | 0.003325719 | 0.004557117 |
| Weapon (12) | Ring16 | Restore | 0.003031220 | 0.004506520 | 0.004355796 |
| Weapon (12) | Ring16 | Combined | 0.005512375 | 0.007775272 | 0.008806632 |
| Team (2) | Hot | Capture | 0.000050197 | 0.000045503 | 0.000114170 |
| Team (2) | Hot | Restore | 0.000112507 | 0.000110217 | 0.000098160 |
| Team (2) | Hot | Combined | 0.000146013 | 0.000141657 | 0.000212932 |
| Team (2) | Ring16 | Capture | 0.000045781 | 0.000054945 | 0.000111782 |
| Team (2) | Ring16 | Restore | 0.000105346 | 0.000099033 | 0.000096690 |
| Team (2) | Ring16 | Combined | 0.000151956 | 0.000165679 | 0.000212331 |
| Action (1) | Hot | Capture | 0.000024175 | 0.000030496 | 0.000015337 |
| Action (1) | Hot | Restore | 0.000015823 | 0.000015583 | 0.000016338 |
| Action (1) | Hot | Combined | 0.000037920 | 0.000039331 | 0.000032481 |
| Action (1) | Ring16 | Capture | 0.000029834 | 0.000031460 | 0.000022184 |
| Action (1) | Ring16 | Restore | 0.000016288 | 0.000015498 | 0.000015131 |
| Action (1) | Ring16 | Combined | 0.000044112 | 0.000048308 | 0.000038557 |
| Wrapper (24) | Hot | Capture | 0.000253765 | 0.000265300 | 0.000173860 |
| Wrapper (24) | Hot | Restore | 0.000193847 | 0.000171933 | 0.000177937 |
| Wrapper (24) | Hot | Combined | 0.000408789 | 0.000482777 | 0.000459362 |
| Wrapper (24) | Ring16 | Capture | 0.000265991 | 0.000306973 | 0.000198550 |
| Wrapper (24) | Ring16 | Restore | 0.000188321 | 0.000180701 | 0.000186578 |
| Wrapper (24) | Ring16 | Combined | 0.000427265 | 0.000506157 | 0.000386892 |

Weapon fixed/coalesced and packed candidates regress despite storing fewer
bytes. Team packing also regresses combined time, although its restore median
improves. Action's small combined reduction is consistent across these two
working sets. Wrapper packing regresses hot combined time and improves ring
combined time. None establishes an end-to-end rollback speedup or justifies
production deployment. No aggregate timing is reported because the subsystems
were measured independently, not as one mixed workload.

### Copy Bytes

These are source-level memcpy payload bytes, not hardware counters or total
read-plus-write traffic. The team baseline includes its actual duplicate write.
Candidate capture includes the retained eight-byte team active reference even
though reference repair is excluded from restore. Metadata and excluded native
effects are not counted. Each wrapper additionally writes one forced-zero byte
on restore in **every** variant (24 stores at the assumed count).

| Subsystem (Count) | Capture Full / Packed Per Object | Restore Current / Candidate Per Object | Combined Current / Fixed / Packed at Count |
|---|---:|---:|---:|
| Weapon (12) | 5600 / 3544 | 3544 / 3544 | 109728 / 109728 / 85056 |
| Team (2) | 704 / 632 | 704 / 624 | 2816 / 2656 / 2512 |
| Action (1) | 768 / 253 | 253 / 253 | 1021 / 1021 / 506 |
| Wrapper (24) | 224 / 81 | 81 / 81 | 7320 / 7320 / 3888 |
| **Total** | **74752 / 45989 at counts** | **46133 / 45973 at counts** | **120885 / 120725 / 91962** |

Packing removes 28,763 captured bytes at these assumed counts, but the measured
copy kernels demonstrate why that is not a proportional elapsed-time saving.
The region/clone wrapper scalar duplication is not removed or credited here.

### Source Boundaries

- `rollback_restore.cpp:1485-1517`: weapon dispatch/scalar loop.
- `rollback_character_restore.cpp:305-319`: linked model motion implementation;
  header explicitly requires valid bodies, resources and compatible work storage.
- `rollback_restore.cpp:1425-1474`: action/team loops and excluded active repair.
- `rollback_restore.cpp:281-291`: wrapper payload and forced zero.
- `rollback_snapshot.cpp:1501-1510`: full region capture memcpy.
- `rollback_snapshot.cpp:1101-1130`: material-job table strides/counts/owners.
- `rollback_character_restore.cpp:78-94`: binding-gated eye-controller subset,
  insufficient to define whole material jobs.
- `concrete_snapshot.cpp:1013-1209`: projectile overlay/scans and native collision
  dependencies; `rollback_restore.cpp:491-1049` adds lifecycle reconstruction.
- `rollback_restore.cpp:309-474`: native clone capture/restore excluded here.

Historical prose in `compact_snapshot.md` describes an older hybrid character
path. This benchmark uses the current linked explicit implementation instead.
