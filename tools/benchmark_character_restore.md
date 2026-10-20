# Frozen Legacy Versus Selective Character Restore

This benchmark measures a frozen legacy **common-character body restore kernel**
against a fully selective, precomputed range plan on controlled fixtures. It is
not a live-game measurement, a complete `Rollback_Restore` benchmark, or a
production-ready selective restore implementation.

**Production compatibility:** `--frozen-header PATH` is now required, including
with `--experiments`. The explicit production path in
`rollback_character_restore.cpp` no longer has the old scalar tables or restore
branch. Measuring it requires a separate harness with its actual layout, bounds,
and ownership policies. This tool does not measure it or claim equivalence to it.
Historical results below describe the legacy implementation only.

## Run

From the project root:

```sh
python3 tools/benchmark_character_restore.py --portable --cpu 2 \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h
```

The runner builds only a portable standalone executable with the host compiler.
It does not build, modify, load, or deploy the ASI. Generated C++, executable,
source hashes, compiler command and CSV are written to
`/tmp/opencode/umvc3_restore_bench/`. Existing production changes are left alone.

```sh
python3 tools/benchmark_character_restore.py --cpu 2 --samples 9 --ms 50 \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --output /tmp/opencode/umvc3_restore_repeat
python3 tools/benchmark_character_restore.py --sanitize --samples 1 --ms 1 \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --stream-mib 1 --output /tmp/opencode/umvc3_restore_sanitized
```

Choose a permitted CPU for pinning, or omit `--cpu`. GCC/Clang and a 64-bit Linux
host are expected. `--ms` is a minimum: each sample also traverses the complete
snapshot working set. Sanitized times are not performance results.

`--portable` omits `-march=native`, matching the plugin's portable ISA policy
(but not its Windows ABI, CRT, MinGW compiler, or LTO). Without this option the
optimized benchmark uses the host's native ISA. `--frozen-header PATH` reuses a
previous run's `current_character_restore.generated.h`, so concurrent production
edits cannot change the reference mid-experiment. Frozen runs record the header
path/hash and set production source hashes to null rather than claiming to have
measured today's source. Keep the original run's provenance alongside the header.

## Legacy Kernel

The runner uses a previously extracted header, not today's working-tree code.
The legacy extracted header retains:

- Common character bulk copy, live model blocks, the 512-entry pointer heuristic.
- Shared scalar range tables, constraints, ability, command and message copies.
- Motion, capsule, sphere, attack-target and embedded input policies.
- CPU-vtable search (the fixtures have no embedded CPU object).
- Late scalar, capsule, constraint and roster-reference repairs.
- The actual `IsLikelyPointer` predicate from `rollback_memory.cpp`.

The early and late phases stay separate in both implementations; plans are not
coalesced across the reconstruction boundary. Native reconstruction itself is
not executed between them in this standalone benchmark.

Excluded from both sides: concrete-character tail discovery and generated subtype
policies; DTI lookup; Iron Man-specific effect validation; external owned-buffer
restore and allocation-identity checks; collision-parent repair; shot/effect
construction and remapping; the region dispatcher; and worker/render barriers.
Game-dependent hooks are explicit stubs in the generated header. No performance
claim is made for those omitted components.

## Selective Kernel And Equivalence

The selective variants only execute coalesced saved-byte copies, leaving all
other bytes live. Its plans are **oracle-assisted fixture plans**, not discovered
UMVC3 schemas: 48 reference executions determine whether each byte can remain
live or must come from the snapshot, separately for early and late phases.

Before measurement, the harness checks full-byte output equality against the
extracted kernel using 32 independent scalar/input patterns, repeated restores,
multiple destination addresses, and every snapshot history transition in the
timed fixture. Training varies the full allowed address-bit range, rather than
assuming neighboring pointers have permanently equal high bytes. Preparation,
plan construction, validation and allocations are outside timing. Synthetic
pointer values, actual backing addresses, and null/roster transitions exercise
the pointer policies without dereferencing game objects.

This models what a complete, validated fixed-field policy could cost, **not** the
cost of reclassifying unknown fields each restore. A negative test deliberately
changes a scalar field to a pointer and proves that the inferred plan no longer
matches the reference. Passing the fixture checks therefore does not make the
plan safe for arbitrary game states. A production selective restore needs audited
field ownership and any required conditional pointer fixups.

All variants use exactly the same early/late spans, without merging across live
holes or across the phase boundary:

| CSV `kernel` | Copy implementation |
| --- | --- |
| `runtime_memcpy` | Original runtime-size `memcpy` per span |
| `size_switch` | Switch to constant-size `memcpy` for 1/2/4/8/16/24/32/48/64/96/128 bytes; runtime fallback |
| `precomputed` | Same size specializations selected as function pointers during plan preparation; indirect call per span |
| `inline_small` | Inline head/tail copies below 32 bytes; runtime `memcpy` otherwise |
| `inline64` | Inline head/tail copies below 64 bytes; runtime `memcpy` otherwise |
| `inline128` | Inline head/tail copies below 128 bytes; runtime `memcpy` otherwise |

Head/tail copies can write selected bytes twice but never touch excluded bytes.
The harness additionally checks lengths 0 through 257 at 16 offsets, including
untouched guard bytes, for every kernel. Every kernel also passes the full-body,
independent-pattern and history-transition checks described above. Each measured
loop is template-specialized, avoiding a per-operation kernel-selection call.

## Cases

Character bodies are 27,232 bytes (`0x6A60`, current nominal fallback) and 30,000
bytes (current capture size limit), not the earlier generic 64/256 KiB objects.
These are code-derived size choices, not sampled concrete-character distributions.

| Fixture | Deliberately injected state |
| --- | --- |
| `sparse32` | 32 pointer-shaped slots, plus shared ownership/reference fields |
| `pointers200` | 200 pointer-shaped slots, plus shared fields |
| `false_positive256` | 32 pointers and 256 packed float pairs that resemble pointers |
| `cap_stress600` | 600 pointer slots before shared-field setup; exercises preservation-cap pressure |

Injection sites use a fixed shuffled layout. Shared-field initialization can
replace an injection. Ordinary randomized scalar bytes are deliberately outside
the pointer predicate's range; the explicit float fixture tests false positives.
Pointer density is synthetic, not a claim about actual characters. The cap case
reproduces current behavior, including its limit, rather than declaring it safe.

- `hot`: one repeatedly restored character and two saved states.
- `match_ring`: six characters and twelve history slots.
- `streaming`: by default about 64 MiB live data, two histories, and one capture
  destination per object (about 256 MiB total); pages are prefaulted.

Streaming objects use synthetic six-member rosters that always include the
current character, so the reference follows the team-character path. This is a
cache-pressure test, not thousands of characters in one real match.

## Reading Results

Each case has one CSV row per selective `kernel`. `current_ns` is the same
reference measurement repeated on those rows, not six independent measurements.
The existing `current*` CSV columns and C++ names are retained for consumers of
saved results; they mean the **frozen legacy oracle**, never current production.
`current_ns` and `selective_ns` are medians of per-sample average nanoseconds per
character operation. Minimum/maximum columns show sample-average variability,
not individual operation percentiles. `speedup = current_ns / selective_ns`.

`restore` measures early + late body restore. `capture_restore` additionally
copies the complete live body into a separate preallocated capture buffer in
both variants; this does not overwrite the source history used for restoration.
`ranges` is the total number of selective spans across both phases.
`selected_bytes` counts bytes copied across both phases, including repeated fields.

First position rotates and direction reverses on alternate samples; compiler
barriers prevent dead-store removal.
No hardware cache flush or fence is added per operation. Timings use GCC `-O3`,
native ISA, and `-fno-strict-aliasing` for the native byte-overlay access patterns.
They are not identical to the Windows/MinGW/LTO plugin build. CPU boost, sibling
threads, OS scheduling and memory locality can affect results.

The frozen-header hash in `provenance.json` identifies the measured oracle. Its
bytes are copied unchanged to the output directory; production source hashes are
null, and Git HEAD identifies only the runner's repository context. Retain the
original extraction provenance to trace the header back to its historical source.
Snapshot histories remain separate from the capture destination, and reference
fixtures retain their original owner-address and roster setup.

## Measured Results: 2026-09-30

AMD Ryzen 7 5800XT, GCC 11.3.0, CPU 2 affinity. Two optimized runs used nine samples
per case, with 30 ms and 50 ms minimum durations respectively. The core was not
isolated from other system activity. All fixture equivalence checks passed in
both runs. A separate ASan/UBSan run with 1 MiB streaming data also passed.

Measured production source SHA-256:
`4d17eccfe4e2f1c7458515ea2aa58cb51b118ab18c13bd7f0d5d15044a057427`.

Raw first run: `/tmp/opencode/umvc3_restore_bench/results.csv`.
Raw confirmation run: `/tmp/opencode/umvc3_restore_repeat/results.csv`.
Each directory contains `provenance.json` and the extracted source header.

Confirmation-run median microseconds **per character restore**, six characters
with twelve snapshot slots:

| Body bytes | Fixture | Selective ranges (early + late) | Current us | Selective us | Speedup |
| ---: | --- | ---: | ---: | ---: | ---: |
| 27,232 | sparse32 | 289 | 21.440 | 3.378 | 6.35x |
| 27,232 | pointers200 | 451 | 21.432 | 4.789 | 4.48x |
| 27,232 | false_positive256 | 658 | 23.130 | 6.644 | 3.48x |
| 27,232 | cap_stress600 | 821 | 18.112 | 8.274 | 2.19x |
| 30,000 | sparse32 | 289 | 31.609 | 4.162 | 7.59x |
| 30,000 | pointers200 | 450 | 22.599 | 4.266 | 5.30x |
| 30,000 | false_positive256 | 645 | 22.447 | 6.142 | 3.65x |
| 30,000 | cap_stress600 | 827 | 18.928 | 7.901 | 2.40x |

For the 27,232-byte match-ring cases, the first run measured 5.62x, 5.27x,
3.65x and 2.30x respectively. Absolute times vary, but the direction and broad
size of the advantage repeat. In the confirmation run, including whole-body
capture reduces these four speedups to 4.47x, 4.14x, 2.77x and 2.17x. Streaming
restore speedups for that body size are 3.76x, 3.56x, 3.28x and 2.67x.

The cap-pressure reference can run faster than a less-dense fixture because the
original pointer scan terminates when its preservation array is full. This is
not evidence that exceeding the cap is safe or desirable.

Conclusion: **hundreds of precomputed selective copies can outperform the
current scan/preserve/bulk-copy/override sequence** in this extracted kernel.
These results support investigating an audited selective schema. They do not
establish a whole-rollback speedup or justify deploying the inferred fixture
plans, omitting native pointer validation, or dropping post-reconstruction repair.

## Copy Kernel Experiments: 2026-10-18

AMD Ryzen 7 5800XT, GCC 11.3.0, CPU 2 affinity, boost enabled, core not isolated.
The original two-variant harness was run first with five samples, 15 ms minimum
and 16 MiB streaming data, freezing its generated header before any experiments.
Subsequent comparisons all use that identical frozen header, nine samples,
30 ms minimum and 64 MiB streaming data. The portable confirmation uses seven
samples and 20 ms minimum. All 48 cases include both restore-only and
capture-plus-restore timings, both object sizes, all shapes and all working sets.

Baseline production SHA-256:
`cb4c21419cd06932d05c110ee3fcebdb7c57642c2a455610028ff21cd8f32c12`.

Frozen generated-header SHA-256:
`3b364775e464cd86d7f3dd4e93c903678639d6894551445c3b4f1696407afc93`.

Geometric mean of the 48 per-case ratios `runtime_memcpy / candidate`
(greater than 1 means faster than the existing selective kernel):

| Kernel | Native | Portable | Portable Repeat |
| --- | ---: | ---: | ---: |
| `size_switch` | 0.999x | 1.000x | 0.992x |
| `precomputed` | 1.007x | 1.034x | 1.021x |
| `inline_small` | 1.232x | 1.363x | 1.339x |
| `inline64` | 1.283x | **1.475x** | **1.448x** |
| `inline128` | **1.334x** | 1.260x | 1.270x |

`inline64` won 45/48 portable cases and 46/48 confirmation cases; `inline_small`
won the remainder. Native code favored `inline128` in 33/48 cases, `inline64`
in 14 and `inline_small` in one. The portable/native threshold difference is a
reason not to choose a production threshold using only `-march=native` results.

Portable match-ring median microseconds per character, restore only:

| Body bytes | Fixture | Extracted Reference | Runtime Copy | Inline64 |
| ---: | --- | ---: | ---: | ---: |
| 27,232 | sparse32 | 20.621 | 3.198 | 2.271 |
| 27,232 | pointers200 | 22.404 | 4.359 | 2.692 |
| 27,232 | false_positive256 | 21.833 | 6.623 | 3.643 |
| 27,232 | cap_stress600 | 15.493 | 7.217 | 3.617 |
| 30,000 | sparse32 | 21.911 | 3.661 | 2.508 |
| 30,000 | pointers200 | 24.111 | 4.719 | 3.153 |
| 30,000 | false_positive256 | 27.994 | 6.541 | 3.805 |
| 30,000 | cap_stress600 | 21.432 | 8.145 | 4.061 |

Recommendation: **test inline head/tail copies for spans below 64 bytes, with
runtime `memcpy` for larger spans**, in the portable production toolchain after
the real field policy is audited. Precomputed dispatch adds metadata and indirect
calls without a compelling consistent gain. These are the fastest tested
strategies for these builds, not a proof of an optimal universal copy kernel.
Absolute reference timings varied substantially between runs under system load;
use the interleaved within-run comparisons, not cross-run absolute differences.
Host glibc `memcpy` can itself select native instructions even with portable
compiler flags, so Windows CRT and MinGW measurements remain necessary.

Commands and raw outputs (each output directory includes CSV and provenance):

```sh
python3 tools/benchmark_character_restore.py --cpu 2 --samples 5 --ms 15 \
    --stream-mib 16 --output /tmp/opencode/restore_baseline
python3 tools/benchmark_character_restore.py \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --cpu 2 --samples 9 --ms 30 --stream-mib 64 \
    --output /tmp/opencode/restore_cutoffs_native
python3 tools/benchmark_character_restore.py \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --portable --cpu 2 --samples 9 --ms 30 --stream-mib 64 \
    --output /tmp/opencode/restore_cutoffs_portable
python3 tools/benchmark_character_restore.py \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --portable --cpu 2 --samples 7 --ms 20 --stream-mib 64 \
    --output /tmp/opencode/restore_cutoffs_portable_repeat
python3 tools/benchmark_character_restore.py \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --sanitize --cpu 2 --samples 1 --ms 1 --stream-mib 1 \
    --output /tmp/opencode/restore_cutoffs_sanitized
```

The baseline command above used the original harness; it cannot be rerun without
`--frozen-header` now. The initial four-candidate measurements before adding
the 64/128 cutoffs are in `/tmp/opencode/restore_kernels_native/` and
`/tmp/opencode/restore_kernels_portable/`. One initial portable-cutoff run hit
the command timeout; its output directory was replaced by the completed run.
All completed optimized runs passed equivalence. The final six-candidate
ASan/UBSan run passed, as did a fresh non-frozen extraction smoke run in
`/tmp/opencode/restore_live_extraction_smoke/`. No extraction-anchor conflict was
observed at that check. No plugin code was changed or built by this experiment.
After formatting-only cleanup, another sanitizer invocation emitted repeated
`AddressSanitizer:DEADLYSIGNAL` messages without an actionable stack trace and
exited before producing CSV rows. An identical rebuild/rerun passed all checks
in `/tmp/opencode/restore_final_sanitized/`. That intermittent sanitizer failure
was not diagnosed; it should not be interpreted as an unconditional clean bill
of health despite the other passing sanitizer runs.

## Layout Mechanism Experiments: 2026-10-18

The separate [production phase audit](../doc/character_restore_phase_audit.md)
records same-phase overlap and native reconstruction consumers. The runner also
emits `scalar_range_audit.json` from the extracted early/late scalar tables. This
is table-only byte accounting, not a complete character schema or permission to
remove overlapping writes across phases.

`--experiments` is a separate, opt-in matrix. The default benchmark and CSV remain
unchanged; running the default does not also run these experiments. This matrix
measures early + late restore only, not capture or the expensive extracted
reference in the timed loop. It preserves both phases and does **not** investigate
or remove redundant early/late writes. The separate production consumer audit is
outside this experiment's scope.

The runner first compiles a bootstrap executable and runs `--dump-plans`, saving
all eight fixture plans to `plans.json`. It then generates
`fixed_character_restore.generated.h` and recompiles with
`-DFIXED_LAYOUT_EXPERIMENTS`. Every selected span becomes an explicit
`std::memcpy(dst + OFFSET, src + OFFSET, SIZE)` with **both literal offsets and
literal sizes taken from that phase's fixture plan**. There is no runtime range
table or size-switch in those functions. Compiler lowering can still use vector
moves, `rep movs`, or CRT calls for individual constant-size copies; this is
unrolling of the span list, not a requirement to unroll every byte instruction.
An `objdump -d --disassemble=_ZL13fixed_0_earlyPhPKh` inspection confirmed literal
displacements and compiler-generated fixed-span moves/`rep movsq` in portable code.

These are still **fixture-only oracle policies**, not concrete-character schemas.
Generation does not make the inference safe for real game states. The frozen
header is the previous section's baseline, SHA-256
`3b364775e464cd86d7f3dd4e93c903678639d6894551445c3b4f1696407afc93`.
No production files were changed or re-extracted while the independent audit ran.
The original extraction's provenance remains in `/tmp/opencode/restore_baseline/`.
Each new output records the header, plans and generated-function hashes, compiler
commands, filters and harness hash. The current runner also records its own hash.

### Isolated Comparisons

| CSV kernel | Mechanism |
| --- | --- |
| `table_inline64` | Unchecked existing `inline64` span loop, separate phase calls |
| `fixed_cached` | Unchecked generated functions; function pair cached per object before timing |
| `fragmented_inline64` | Same phase masks split into chunks of at most 32 bytes, with duplicate 8-byte suffix overlaps where possible |
| `coalesced_inline64` | Sorted adjacent/overlapping union of those fragments; no gaps or phase boundaries merged |
| `bounds_range` | Each span checks both source and destination capacities before copying |
| `bounds_layout` | Each phase checks capacities against its prevalidated maximum span end once; short inputs fall back to the identical per-range policy |
| `fixed_lookup` | Exactly the same generated functions as `fixed_cached`, selected again once per character operation |

The original oracle already contains maximal contiguous spans. Coalescing the
deliberately fragmented input recovers exactly that original union and range
count; it cannot improve the oracle itself. Overlaps here mean overlapping range
descriptors with the same saved-byte provenance, **not overlapping source and
destination allocations**. All copies retain `memcpy`'s disjoint-buffer contract.
Coalescing is preparation work, not timed work.

Bounds comparisons use the same `inline64` copying policy on both sides. Maximum
ends are computed with wide arithmetic outside timing, after layout validation;
an unchecked/mutable/untrusted layout cannot use this shortcut. An invalid span
is skipped in both checked variants, rather than partially copied or causing
all valid spans to be rejected. The once-per-layout check is per phase, with both
capacities compared each time. Unchecked table/generated timings deliberately
isolate copy generation from bounds overhead. A separate size-aware generated
entry point is tested, including its short-buffer fallback, but is not timed.

Dispatch selection is a noinline, checked direct layout-ID array lookup returning
the early/late function pair, not an invented production hash-map or DTI cost.
A compiler memory barrier prevents lookup hoisting/CSE without adding a hardware
fence. Cached and repeated variants both make two indirect phase calls; the
repeated variant performs **one lookup per character**, not per span or phase.
The result therefore applies to cheap indexed selection; it does not establish
the cost of identifying a real character's concrete layout.

### Cases And Checks

All eight layouts (two body sizes, four shapes) are exercised in `hot`,
`match_ring` and `streaming`. `mixed_ring` additionally cycles the selected shapes
through six objects with twelve history slots, and `mixed_streaming` cycles shapes
through the streaming objects. Mixed cases run once per body size, for 28 cases
total. The six-object mixed order is shapes 0,1,2,3,0,1 when all shapes are enabled.
Streaming uses 64 MiB of live bodies and roughly 256 MiB allocated overall,
including both histories and the preallocated unused capture buffer.

`--shapes`, `--workloads` and `--sizes` accept comma-separated explicit filters
only with `--experiments`; `all` is the default. For example,
`--sizes 27232 --shapes sparse32,pointers200 --workloads hot,mixed_ring` is a
small matrix. Filters reduce execution, not the eight generated functions pairs,
so code-size reporting remains comparable. CSV `mean_ranges` and
`mean_fragmented_ranges` are per-object means, including mixed-layout weighting.
`table_speedup` is `table_inline64 / kernel`; use `summary.json` for the isolated
pairwise ratios. Ratios are not additive or multiplicative production estimates.

Validation is outside timing and includes:

- Exact per-phase byte-mask equality for original, fragmented, coalesced and generated plans; bounds validation and equal recovered range counts.
- Full-destination comparison against extracted production **after each phase**, using 32 independent fixture values and 64-byte outer guards, including excluded bytes.
- Every timed object/history transition, separately for early and late, including mixed-layout objects and changing owner addresses.
- Equivalent cached/repeated dispatch, including lookup execution on every history transition.
- Zero, very short, half-sized and full source/destination capacities independently; generated checked-entry fallback; invalid end-of-buffer and `UINT32_MAX`-offset descriptors with wide-end calculation. Complete backing-buffer comparisons ensure no writes beyond the declared short capacities.
- Existing copy-kernel checks for sizes 0 through 257 at offsets 1 through 16.

Final validation added generated checked-entry short-buffer coverage after the
optimized measurements. That validation-only addition is identified by a different
harness hash in `restore_mechanisms_final_sanitized/`; it does not change the
measured unchecked copy functions or measured loops. The unchanged default matrix
also passed a separate optimized smoke run.

### Measured Findings

Same Ryzen 7 5800XT, GCC 11.3.0 and CPU 2 as above; boost enabled, core not isolated.
Portable and native runs used nine alternating/rotating-order samples at 20 ms
minimum; a portable confirmation used nine at 30 ms. Each sample traverses at
least one complete history working set. Reported times are medians of sample
averages; CSV also retains min/max. These runs were sequential, not concurrent
with each other. System activity still produces visible variability.

Geometric means of the 28 **within-run** pairwise median ratios; greater than one
favors the first named mechanism:

| Comparison | Portable | Native | Portable Repeat |
| --- | ---: | ---: | ---: |
| Fixed offsets vs original inline64 table | 1.300x | 1.515x | 1.308x |
| Coalesced vs deliberately fragmented | 1.919x | 2.077x | 1.871x |
| Coalesced vs original maximal-span oracle | 0.983x | 0.999x | 0.995x |
| Once-per-layout vs per-range bounds | 1.154x | 1.027x | 1.127x |
| Cached vs repeated indexed function selection | 1.000x | 1.009x | 0.990x |

**Which mechanisms helped:** literal fixed copies were the strongest additional
improvement, winning 27/28 portable, 28/28 native and 28/28 confirmation cases.
In the confirmation they averaged 1.478x for hot, 1.402x for homogeneous six-object
rings, 1.334x for mixed rings, 1.118x for homogeneous streaming and 1.112x for
mixed streaming. Coalescing helps fragmented descriptors, not the already maximal
oracle; its near-1x oracle control confirms there is no new coalescing opportunity
being claimed. Bounds hoisting won all portable cases in both runs, but only 23/28
native cases with a much smaller average gain. Cached dispatch had no convincing
gain for this cheap lookup, winning only 11/28 and 12/28 portable cases. Do not
attribute the fixed-copy improvement to caching; `fixed_lookup` also uses the
same fixed-offset code and performs similarly.

Representative portable-confirmation medians, microseconds per character:

| Bytes | Layout/workload | Inline64 | Fixed Cached | Range Bounds | Layout Bounds |
| ---: | --- | ---: | ---: | ---: | ---: |
| 27,232 | sparse32 / match_ring | 2.226 | 1.989 | 2.413 | 2.104 |
| 27,232 | false_positive256 / match_ring | 3.401 | 1.897 | 4.435 | 3.688 |
| 27,232 | mixed / mixed_ring | 2.970 | 2.207 | 3.402 | 3.042 |
| 27,232 | mixed / mixed_streaming | 4.790 | 4.286 | 5.127 | 4.803 |
| 30,000 | mixed / mixed_ring | 3.266 | 2.471 | 3.917 | 3.223 |
| 30,000 | mixed / mixed_streaming | 5.314 | 4.804 | 5.561 | 5.429 |

### Code Size

Measured with `nm -S --size-sort -C` and `size -A`; `symbols.txt`, `sections.txt`
and `code_sizes.json` are saved alongside each CSV. Combined early + late
generated-function symbol sizes, **bytes**, excluding alignment/metadata/CRT:

| Bytes | Fixture | Portable Text | Native Text |
| ---: | --- | ---: | ---: |
| 27,232 | sparse32 | 13,125 | 10,016 |
| 27,232 | pointers200 | 26,278 | 18,266 |
| 27,232 | false_positive256 | 29,259 | 22,964 |
| 27,232 | cap_stress600 | 38,799 | 35,247 |
| 30,000 | sparse32 | 13,266 | 9,787 |
| 30,000 | pointers200 | 27,027 | 18,218 |
| 30,000 | false_positive256 | 30,304 | 22,674 |
| 30,000 | cap_stress600 | 41,667 | 35,890 |
| Total | Eight layout pairs | **219,725** | **173,062** |

The reusable `tablePhase` is only 354 bytes portable / 370 native, excluding its
range metadata and CRT. Whole executable `.text` is 296,551 / 250,647 bytes;
these include validation, reference and legacy kernels, not just the new copies.
Portable confirmation generated sizes were identical to the first portable run.
The per-core L1 instruction cache is 32 KiB. A dense generated layout can exceed
it by itself; four layouts at one size total about 105-110 KiB portable. Mixed
rings exercise switching among those functions, but there are no instruction-cache
miss counters here and no intervening real game code. Code size, instruction-cache
competition, additional production layouts, Windows CRT, MinGW/LTO and actual
consumer work can change the tradeoff substantially. Portable compiler flags do
not prevent host glibc's runtime `memcpy` dispatch from using native instructions.
The results justify further audited/toolchain-specific testing, not deployment of
the oracle policies or an assertion of whole-rollback speedup.

### Commands And Outputs

All output directories below contain their own CSV and provenance. Completed
experiment runs additionally contain generated plans/functions, code sizes and
`summary.json`. Exact optimized commands:

```sh
python3 tools/benchmark_character_restore.py --experiments \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --portable --cpu 2 --samples 9 --ms 20 --stream-mib 64 \
    --output /tmp/opencode/restore_mechanisms_portable
python3 tools/benchmark_character_restore.py --experiments \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --cpu 2 --samples 9 --ms 20 --stream-mib 64 \
    --output /tmp/opencode/restore_mechanisms_native
python3 tools/benchmark_character_restore.py --experiments \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --portable --cpu 2 --samples 9 --ms 30 --stream-mib 64 \
    --output /tmp/opencode/restore_mechanisms_portable_repeat
```

Exact final verification commands:

```sh
python3 tools/benchmark_character_restore.py --experiments \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --sanitize --no-pie --cpu 2 --samples 1 --ms 1 --stream-mib 1 \
    --workloads mixed_ring,mixed_streaming \
    --output /tmp/opencode/restore_mechanisms_final_sanitized
python3 tools/benchmark_character_restore.py \
    --frozen-header /tmp/opencode/restore_baseline/current_character_restore.generated.h \
    --portable --cpu 2 --samples 1 --ms 1 --stream-mib 1 \
    --output /tmp/opencode/restore_mechanisms_baseline_smoke
git diff --check -- tools/benchmark_character_restore.cpp tools/benchmark_character_restore.py tools/benchmark_character_restore.md
```

Both executable checks passed; sanitizer timings are not performance evidence.
An earlier otherwise identical sanitizer command without `--no-pie` or workload
filters, output `/tmp/opencode/restore_mechanisms_sanitized/`, failed with repeated
`AddressSanitizer:DEADLYSIGNAL` before emitting CSV. Retrying that executable with
`ASAN_OPTIONS=verbosity=1:log_path=/tmp/opencode/restore_mechanisms_sanitized/asan`
also failed; `asan.2` ends during ASan startup after signal-handler installation,
with no application stack trace. Root cause is not established. `--no-pie` adds
`-fno-pie -no-pie` only when explicitly requested and allowed testing to proceed.
The full non-PIE matrix in `/tmp/opencode/restore_mechanisms_sanitized_nopie/`
then exceeded 240 seconds after 17 cases; that partial CSV is not a completed run.
The explicit mixed-workload filter reduced the sanitizer matrix to four cases
while retaining plan/guard/short-buffer validation of **all eight layouts**.
Both `/tmp/opencode/restore_mechanisms_sanitized_filtered/` and the final run
above completed successfully. No optimized matrix was reduced or timed out.
