# Determinism-preserving character movement optimization investigation

## Result

Investigated shared character movement using the supplied
`20260911T095307.034337Z_addr_fixed_candidate_4000` profiles, current source,
Ghidra's native executable on port 8192, and the six roster characters' installed
command resources. No game/plugin code was changed or game launched.

**Best demonstrated small candidate:** eliminate the command resource's repeated
linear index searches. Actual resources and a standalone native-instruction
microbenchmark support a saving on the order of **6–10 us/replayed frame**, before
integration overhead and dependent on the active character mix. This is not an
in-game A/B result.

**Most interesting deeper candidate:** memoize the exact native output of selected
pure compressed-quaternion decoders. Preserve the surrounding motion state machine
and all cursor updates. Actual decoder costs, format frequencies and cache hit
rates are missing, so no numerical saving is established.

Additional opportunities are a bounded redundant action-getter loop and caching
resource bind-pose quaternion conversion. These do not justify a broad native
movement skip or a claimed large percentage improvement.

The existing profile is `profile_ready`; none of these candidates is
`optimization_verified`. CPU optimization is outside scope.

## Baseline context

| Metric | Host | Client |
|---|---:|---:|
| Native tick ms/replayed frame | 1.15850 | 1.13467 |
| Category-8 movement ms/replayed frame | 0.55549 | 0.59517 |
| Full frame-input calls/replayed frame | 2.46139 | 2.71886 |
| Main fighter movement calls/replayed frame | 6 | 6 |

The input call count is an activity proxy, not per-character attribution of
every inner movement path. Cheap reserve updates are already selected by native
state. Do not replace the native gate with a point-character-only check.

Addresses are preferred-image VAs at base `0x140000000`. Source inspection began
with the two existing untracked research reports present; they were preserved.

## 1. Command resource lookup: verified redundant search

### Native call chain

```text
0x14004BD30 uCharacter__move_base
  -> 0x140028D10 cChrCmd__updateMatcherAndSuccessDelay, twice
     -> for each mutable 0x20-byte line, virtual +0x28
        -> 0x140028240 cChrCmd__Line__evaluateCommandSteps
           -> 0x14005B630 rCommandList__getCommandByLineIndex
```

The last helper runs **before** the line's nonempty/predicate checks. Rejected
commands still pay the lookup cost. Ghidra receiver types were corrected to
`cChrCmd__Line *`, `cChrCmd *` and `rChrCommand *` where established.

`0x14005B630`:

1. Reads blob pointer from resource `+0x70`; returns zero if absent.
2. Rejects a negative signed line ID or ID outside the uint32 count at blob `+8`.
3. Calls `0x14005B600` with index zero, obtaining blob `+0x10`.
4. Scans `count` index entries, each `{uint32 id, uint32 blobOffset}`.
5. Returns `blob + offset` for the **first** matching ID, otherwise zero.

The search loop is six instructions on each nonmatching entry. It has no
gameplay writes, RNG, ownership operations, or callbacks; its only callee is
pointer arithmetic. The misleading generic name of the `+0x10` helper does not
imply 0x40-byte index records: its caller advances the search pointer by **8**.

For dense IDs, all N line lookups cost **N(N+1)/2 comparisons per matcher pass**.

### Actual command resources

Read only these archives under the game's `nativePCx64/chr/archive`, decompressing
matching command entries in memory. Names inside each ARC identify the character;
archive IDs should not be inferred from list position or a UI's roster ordering.

| Character | Archive | Normal spatk / combo lines | Native index comparisons/full update | Easy spatk / combo lines |
|---|---|---:|---:|---:|
| Tron | `0014_param.arc` | 39 / 47 | 1,908 | 38 / 38 |
| Doctor Doom | `0035_param.arc` | 58 / 46 | 2,792 | 56 / 38 |
| Felicia | `0017_param.arc` | 46 / 52 | 2,459 | 35 / 43 |
| Arthur | `0011_param.arc` | 57 / 53 | 3,084 | 45 / 50 |
| Iron Man | `0033_param.arc` | 54 / 42 | 2,388 | 45 / 37 |
| Strider / Hiryu | `0021_param.arc` | 47 / 47 | 2,256 | 32 / 38 |

All 24 main-character normal/easy index tables have unique IDs exactly
`0,1,...,count-1`. All their command records have nonzero step counts. FeliciaF
and FeliciaC helper resources found in Felicia's archive have 4/1 lines and
the same dense property; they offer negligible search savings.

These are current installed-resource facts, not proof that the historical run
used unchanged resource bytes or which normal/easy variant was bound.

### Safe optimization shape

For a resource proved dense, the result is exactly:

```cpp
blob + indexTable[lineId].blobOffset
```

Validate the dense property before using that fast path. An inexpensive initial
experiment can validate once per matcher invocation and restrict the optimization
to that invocation, with nested-call-safe context and native fallback. Preserve
all native line iteration, predicates, timers, success handling, and ordering.

Resource lifecycle matters:

- `rChrCommand__convert`, **0x14005AD90**, frees the old blob, allocates and
  copies the new resource after format/version checks. It does not sort the table.
- Destructor **0x14005AC10** frees and clears the blob.
- Matcher binding **0x1400280C0** sets resource `+0x168`, count `+0x170`, and
  allocates/reset lines at `+0x178`.
- A persistent resource cache needs conversion/destruction invalidation; pointer
  plus count is insufficient against allocation-address reuse.
- A scoped optimization still needs a stable table during the matcher invocation
  and must fall back if resource/blob/count changes. In-place index mutation
  requires invalidation too; asset density is not blanket permission to assume
  every modded or future resource is dense.
- Preserve native first-match semantics for duplicate IDs. Checking only that
  `table[lineId].id == lineId` does not rule out an earlier duplicate.

An exact-search SIMD implementation is another way to improve this accessor
without a persistent index cache, but its smaller gain has not been measured.
The trivial step accessor `0x14005AD40` is different: it already performs indexed
arithmetic and one virtual header-size query, not another whole-table scan.

### Isolated microbenchmark

Temporary research source: `/tmp/opencode/bench_command_lookup.cpp`.
Command-resource inspection: `/tmp/opencode/inspect_character_commands.py`.

```text
g++ -O2 -fno-tree-vectorize -std=c++17 /tmp/opencode/bench_command_lookup.cpp \
    -o /tmp/opencode/bench_command_lookup
/tmp/opencode/bench_command_lookup
```

This builds a standalone Linux research executable, not the Windows plugin.
The benchmark transplants the native lookup and its pointer helper's instruction
sequences with Windows x64 calling convention retained and local labels replacing
game addresses. Tables use the inspected normal resource counts and dense ID order;
record offsets are synthetic, since no record bodies are read by the accessor.
The candidate is a noinline direct lookup. A second candidate includes dense-table
validation on every matcher pass. Output pointers were checked equal for every
in-range ID, two out-of-range IDs, and `UINT32_MAX` in each test table.

Nine rounds rotate method order. Each sample performs 20,000 complete two-matcher
sweeps; values below are medians in microseconds per full character update.

| Character | Native lookup sweeps | Direct lookup | Validation + direct lookup | Saving with validation |
|---|---:|---:|---:|---:|
| Tron | 2.7089 | 0.2593 | 0.3520 | 2.3569 |
| Doctor Doom | 3.7087 | 0.3231 | 0.4517 | 3.2570 |
| Felicia | 3.4178 | 0.3103 | 0.4042 | 3.0136 |
| Arthur | 4.1003 | 0.3473 | 0.4551 | 3.6452 |
| Iron Man | 3.3481 | 0.4585 | 0.5527 | 2.7954 |
| Strider | 3.2230 | 0.2893 | 0.3990 | 2.8240 |

Scaling the observed 2.46–2.72 full-input calls/frame by these microbenchmark
savings gives roughly **0.0058–0.0099 ms/frame**, or **0.041–0.069 ms per
seven replayed frames**. This is an order-of-magnitude estimate, not a matched
character-weighted or in-game measurement. Warm tight-loop caches, loop overhead,
branch prediction, integration guards, normal/easy resources and active mix
differ from the game. The microbenchmark does not measure the command interpreter.

The improvement is large for the lookup itself but only about **1–2% of the
category-8 budget** at that scale. It is not a route to a 20% tick reduction.

## 2. Hoist repeated action getter within a single command predicate

At **0x14002AC40**, `cChrCmdCombo__matchesAllowedActionList` compares eight
allowed action IDs at command `+0x20` with the owner character's current action.
It reloads the owner and invokes owner vtable **+0x180 for every comparison**.

For the audited Tron vtable (`0x140A81F20`), +0x180 is **0x140036F90**,
`uCharacter__getCurrentAnmchrAction`: a pure read of character `+0x1310`.

A fast path can require this exact getter target, load/call it once, and perform
the same eight comparisons. Other virtual overrides retain the original path.
No mutation or callback intervenes between comparisons when the target is this
getter. This is a per-invocation local optimization, not a cross-frame cache.

Preserve the full native return representation: it replaces the low byte of the
getter's uint result with 0/1, retaining the upper 24 action-ID bits. Null owner
returns zero. Returning a normalized uint bool is not strictly equivalent.

At most seven owner/getter pairs disappear per predicate. With 42–53 normal
combo lines, that is at most 294–371 duplicate pairs per full update, and only
for lines reaching this predicate. Its caller `0x14002AB80` rejects some lines
earlier. The savings are unmeasured and likely incremental. Time the whole
predicate, not sub-nanosecond property getters individually.

Do not hoist matcher-wide predicates across lines: successful command processing
at **0x140027380** calls `cChrCmdLimit__reset` and changes matcher success fields.
Other command steps also mutate state that later lines can observe.

## 3. Memoize selected pure quaternion decoder results

### A narrower boundary than model/animation state

The common sampler **0x14079F5D0** is not pure. It writes:

- Track work `+0x08`: cached segment start time.
- Track work `+0x10`: current packed-key cursor.

It traverses forward or backward through keys, including reset-on-large-rewind
behavior, then dispatches through the decoder table at **0x140BC0BB0**. Keep this
sampler and its cursor writes running. A resource/frame-only cache around it
would silently omit required state updates.

Two inspected decoder targets below that stateful sampler are pure output
calculations, apart from the processor floating-point environment:

| Decoder | Table type | Behavior |
|---|---:|---|
| **0x14079D900** `nMotion__samplePackedQuat14` | 6 | Decode four signed 14-bit components from each 8-byte key, hemisphere-select, interpolate and normalize |
| **0x14079E660** `nMotion__samplePackedQuat11WithRange` | 14 | Decode four packed 11-bit components using eight scale/offset floats, hemisphere-select, interpolate and normalize |

Both have no callees and write only four floats to the supplied output buffer.
The previous `sBenchmark` receiver types/names were incorrect; Ghidra signatures
now use a four-float output pointer and the actual packed-data inputs.

### Exact-output cache design to investigate

On a cache miss, run the **original native decoder** and store its 16 output
bytes. On a verified hit, copy those exact bytes. The key must contain:

- Decoder identity.
- Interpolation weight's exact float bits; never a rounded frame number.
- Packed input values, copied by value rather than stored as borrowed pointers.
  Type 6 needs 8 bytes at weight zero and 16 at nonzero weight. Type 14 needs
  6 or 12 bytes respectively.
- All eight range floats by value for type 14.
- Relevant FP state. For strict equivalence, include full input MXCSR and store
  output MXCSR as well as output bytes. A hit must reproduce raised sticky flags,
  not merely honor rounding/FTZ/DAZ. Fall back for unmasked exceptions and
  unsupported/nonfinite cases until explicitly validated.

By-value keys avoid stale-resource-pointer and same-address-reallocation hits.
Do not read the next key on a zero-weight path: the original needs only the
current key there. Use a bounded per-thread table, verify complete keys after
hashing, and avoid allocation in the inner loop. Preserve output/input alias
semantics or restrict the fast path to the normal nonaliasing stack-output calls.

Caching native outputs avoids introducing different SSE/FMA operations. However,
lookup, key-copy and FP-state costs may exceed these decoders' cost on misses;
this must be measured before implementation. Reentrancy/thread handling and any
callee-saved state remain part of the hook contract.

### Evidence of potentially reusable work, with an important limit

Using half-open `[start_frame,end_frame)` intervals, the single captured profile
segment has:

| Side | Replayed frame occurrences | Distinct frame numbers | Repeated occurrences |
|---|---:|---:|---:|
| Host | 3,043 | 1,507 | 1,536 (50.48%) |
| Client | 1,750 | 811 | 939 (53.66%) |

Thus many historical frames are replayed more than once. This is motivation to
measure exact decoder-key repetition, **not a claimed 50% decoder-cache hit rate**.
Corrected input, animation transitions, interpolation state, cache capacity and
resource changes can alter keys; repeated stock poses can also generate hits at
different frame numbers.

Saving model:

```text
net saving/frame = hits * (native decoder cost - hit-path cost)
                   - misses * added miss-path cost
                   - management cost
```

No decoder timings or key traces exist in the supplied JSON. In particular,
0.56–0.60 ms of category-8 work is a parent budget, not removable decoding cost.

### Do not pre-normalize endpoints

The inspected nonzero-weight paths interpolate **raw decoded endpoints**, then
normalize the interpolated quaternion. Their zero-weight paths normalize one
endpoint. Therefore calling the decoder at weight zero to build a normalized
endpoint cache and then interpolating those cached endpoints changes the native
calculation. A raw-endpoint cache is a separate option, but it retains the final
square root/division and requires reproducing the original decode exactly.

## 4. Cache bind-matrix-derived rotation only

Previously identified and reconfirmed:

- Simple joint evaluator **0x14052CC70**, missing-rotation-track branch.
- Bind-pose reset **0x14056E210**.

Both convert resource matrices into quaternions. Resource matrix pointer is read
from `rModel+0x80`, with 0x40-byte matrices. The conversion includes branch-selected
square roots/divides and is repeated inside joint loops.

A resource-generation/joint-index cache of the exact native rotation result could
avoid repeated conversion. Preserve the identity/additive gates, dynamic joint
translation, scale writes, blending order, and all following constraints/matrices.
The reset helper reads translation from the live joint record, so caching the
whole reset joint would be incorrect.

`rModel__convert` at **0x1405A2B70** allocates and binds the matrix tables; resource
replacement needs invalidation. Actual branch frequency and cost are unknown.
Unlike the pure decoder leaves, the conversion is inlined into larger stateful
functions, making interception and verification more invasive. Prefer the narrower
decoder experiment if it has adequate timing and reuse.

## Paths reviewed that do not justify suppression

- **Motion cursor search is already cached.** `0x14079F5D0` carries a current
  segment pointer/start time and moves from there. Replacing it with a lookup is
  not eliminating a full linear scan every sample.
- **`cMtMotion__lookupTrackByHashOrIndex` (0x1407772C0)** already has an indexed
  fast path when its index table is present. Observed callers are collision-related
  functions; no evidence establishes it as the per-joint character hotspot.
- **Collision attachment construction is already lazy.** The ensure helper tests
  `mChrF & 0x800` and builds only when missing.
- **Sphere passes are not redundant counting.** `0x14004B7E0` processes attack/
  hurt spheres; push processing handles pending correction vectors. Active sphere
  callbacks update world geometry and collision registration.
- **Sphere world updates (0x140010380/0x140010210)** validate owner liveness, obtain
  owner/joint matrices, apply local-axis flags and transform offsets. Caching an
  owner matrix across arbitrary callbacks is not justified by shared ownership.
- **Finalization (0x14004C6D0)** clears dead references and runs look/neck/other
  callbacks. It executes even on cheap reserve paths and is not presentation-only.
- **General matcher predicates cannot be cached across lines.** Success and work
  opcodes change later observable state, even before a new action starts.

## Validation and recommended order

1. **Command lookup first if a small, auditable improvement is wanted.** Add
   scoped timing for the accessor/matcher; verify every optimized result against
   native on actual resources, including empty, reordered, duplicate and missing
   ID tables and reloads. Retain exact line-state updates and compare final
   gameplay checksums. The standalone test is not a gameplay proof.
2. **For potentially larger savings, measure motion decoding.** Time the two
   decoder types and sample exact-key repetition without installing a cache.
   Control instrumentation overhead; these are high-frequency calls. Measure
   cache hit/miss cost in isolation before adding it to the game.
3. **Validate candidate hits against native**, using a separate output buffer and
   the same starting FP environment; compare all 16 output bytes and post-FP state.
   Run this in verification mode, not as the performance benchmark.
4. Test normal movement, command timing, charges, hitstop, throws, assists/tags/DHC,
   transformed/install characters, root motion and joint-attached shots. Compare
   command lines/timers, motion cursors, action state, joint matrices, collision,
   input, RNG state/draw counts and final ground-truth checksums.

Each implementation must be separate, measured under the same profile conditions,
and must preserve work rather than reduce rollback depth or skip fighters. Current
evidence supports one small measured microbenchmark win and several testable
deterministic hypotheses—not an established large native-tick speedup.
