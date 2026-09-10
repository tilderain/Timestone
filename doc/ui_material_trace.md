# UI Material Lifecycle Diagnostics

## Purpose

The September 6 crash on `89099fa` occurred in `uMaterialControl::trans` at
`0x14054A48E`, reached by `uBtGaugeHc::move` through an embedded
`uMaterialVjobAnimation`. The target `uUiModelBase` at `0xF4300A0` advertised
three materials, but entry zero was null. The temporary exception handler that
skipped that entry has been removed. These diagnostics do not suppress the
fault, skip native calls, repair caches, or acquire material references. Other
pre-existing material recovery/sanitizer behavior is unchanged and is traced.

## Native Ownership

Ghidra confirms these shared native paths:

| Address | Behavior |
|---|---|
| `0x140561CE0` | Replace model resource; release old resource, clear cache, allocate/zero table, publish resource/table/count, reset materials |
| `0x14055FB20` | Release each non-null material, zero each slot, free a non-inline table, then clear table pointer and count |
| `0x14055FBC0` | Release/rebuild individual material entries from the current resource |
| `0x14076F760` | Atomically decrement material reference count at `+0xC`; destroy or defer destruction when it reaches zero |
| `0x14054A430` | Material-controller traversal; target model at controller `+0x40`, enabled byte at `+0x48` |

Tables with at most three materials use inline storage at `model+0x198`.
Larger tables have a separate allocation. For this crash, investigate writes to
the inline table as well as ownership fields `+0x100..+0x118`; protecting only
the latter is insufficient if another restore region overlaps the model body.
This is an investigation target, not proof that a restore caused the crash.

## Output

On startup, HookLog reports four `UI material trace ... status=0` lines for
controller traversal, cache clear, resource replacement, and material release.
Nonzero status means that diagnostic hook did not install successfully.

The ordinary crash report now includes a `UI material lifecycle trace` section,
independent of the last 50 HookLog lines. It stores 32 events per tracked model,
up to 64 model addresses, sampling the first eight material slots. Models are
first enrolled by the exact `uUiModelBase` vtable `0x140B21DC0`; already tracked
addresses remain observable when their vtable changes during teardown/reuse.
There is no heap allocation or per-frame file/console output in the trace path.

Each event includes a global sequence, millisecond tick, thread, resimulation
flag, caller return address, model vtable/unit flags, resource/source pointers,
table/count, and sampled material pointers, vtables, and `+0xC` refcounts.
`frame` is the requested restore frame or current resimulation frame; `-1`
means no frame was supplied on the live path. Use sequence/tick for ordering
across models. Caller addresses are process addresses, not module offsets.

The last observed controller is included with its vtable, current target, and
enabled byte. It is a borrowed diagnostic address, not an ownership assertion
or an exhaustive list of all controllers referencing the model.

Model `readable` is a bitmask: bit 0 means the model header was read, bit 1
means the sampled table range was read (or the count was zero). Per-material
and controller readability are separate booleans. Failed reads are zero-filled;
do not interpret their zero fields as confirmed null pointers. Memory is sampled
with `ReadProcessMemory`, without virtual calls on potentially retired objects.

## Events

- `set-resource-enter/exit`: `arg` is the requested resource pointer.
- `clear-enter/exit`: brackets the native cache release routine, including its
  count/table clearing. The caller distinguishes destruction from replacement.
- `reset-enter`: cache state before the existing sanitizer.
- `reset-native-enter/exit`: brackets the native reset after sanitization;
  `arg` is the count of recycled non-null entries the sanitizer cleared.
- `reset-invalid-table`: the existing sanitizer zeroed an invalid table's count.
- `reset-deferred`: the existing headless character reset suppression ran.
- `release-enter`: a material release matched a pointer in a tracked model's
  last sample; `arg` identifies it. This is recorded before native release.
  A match is not proof that the model still owns that allocation. There is no
  post-release read of the released object by the release hook.
- `trans`: sampled immediately before native controller traversal. Unchanged
  states are omitted; invalid data never causes this hook to skip native code.
- `restore-before/after`: changes observed around the complete snapshot restore.
- `regions-before/after`: changes observed around the main region restore loop,
  before later collision/shot/effect reconstruction and presentation repair.
- `crash`: final best-effort sample of tracked models when writing the report.

Traversal, restore, and crash samples are recorded only when the sampled state
changes. Lifecycle events are always recorded for tracked models. Compare each
event with its preceding state, not just the presence or absence of an event.

## Reading the Next Crash

1. Find the crashing model's history and its last `trans` or `crash` sample.
2. Check whether it was already incomplete at `reset-native-exit` or
   `set-resource-exit`. That points toward failed initialization/rebuild;
   a missing exit can mean the crash happened inside the native operation.
3. Compare `reset-enter` with `reset-native-enter` to identify changes made by
   the pre-existing sanitizer rather than attributing them to native code.
4. Look for `clear-enter`, material releases, and `clear-exit` while the job
   remains bound. A subsequent active traversal of a cleared or recycled model
   motivates tracing finalization/detachment ordering, not adding another free.
5. A change first seen at `regions-after` narrows the writer to the region
   restore interval. A change only at `restore-after` points to later restore
   work. Inspect region overlap and native calls in that interval next.

The history is observational, not a write watchpoint: concurrent writes, paths
outside the hooked routines, allocator reuse, more than eight material slots,
and overwritten history can leave gaps. No change event does not prove no
temporary mutation occurred between samples. `dropped` counts lock-contention
losses and `evicted` counts replaced model histories; event totals above 32
mean older events for that address were overwritten. Try-locks avoid blocking
native execution or deadlocking the crash handler. Sampling changes timing and
adds overhead, so remove these temporary diagnostics after finding the writer.
