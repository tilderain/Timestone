# Current Versus Selective Character Restore

This benchmark measures the current **common-character body restore kernel**
against a fully selective, precomputed range plan on controlled fixtures. It is
not a live-game measurement, a complete `Rollback_Restore` benchmark, or a
production-ready selective restore implementation.

## Run

From the project root:

```sh
python3 tools/benchmark_character_restore.py --cpu 2
```

The runner builds only a portable standalone executable with the host compiler.
It does not build, modify, load, or deploy the ASI. Generated C++, executable,
source hashes, compiler command and CSV are written to
`/tmp/opencode/umvc3_restore_bench/`. Existing production changes are left alone.

```sh
python3 tools/benchmark_character_restore.py --cpu 2 --samples 9 --ms 50 \
    --output /tmp/opencode/umvc3_restore_repeat
python3 tools/benchmark_character_restore.py --sanitize --samples 1 --ms 1 \
    --stream-mib 1 --output /tmp/opencode/umvc3_restore_sanitized
```

Choose a permitted CPU for pinning, or omit `--cpu`. GCC/Clang and a 64-bit Linux
host are expected. `--ms` is a minimum: each sample also traverses the complete
snapshot working set. Sanitized times are not performance results.

## Current Kernel

The runner extracts the working-tree code rather than reimplementing its copy
loops. The generated header retains:

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

The selective variant only executes coalesced saved-byte copies, leaving all
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

`current_ns` and `selective_ns` are medians of per-sample average nanoseconds per
character operation. Minimum/maximum columns show sample-average variability,
not individual operation percentiles. `speedup = current_ns / selective_ns`.

`restore` measures early + late body restore. `capture_restore` additionally
copies the complete live body into a separate preallocated capture buffer in
both variants; this does not overwrite the source history used for restoration.
`ranges` is the total number of selective spans across both phases.
`selected_bytes` counts bytes copied across both phases, including repeated fields.

Order alternates between variants; compiler barriers prevent dead-store removal.
No hardware cache flush or fence is added per operation. Timings use GCC `-O3`,
native ISA, and `-fno-strict-aliasing` for the native byte-overlay access patterns.
They are not identical to the Windows/MinGW/LTO plugin build. CPU boost, sibling
threads, OS scheduling and memory locality can affect results.

The source hash in `provenance.json`, not Git HEAD alone, identifies the measured
working-tree implementation. If extraction anchors change, update the harness
instead of treating old results as measurements of new source.

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
