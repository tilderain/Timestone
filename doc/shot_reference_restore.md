# Shot Reference Restore

2026-11-09, Ghidra port 8192, `umvc3.exe.unpacked.exe`, base `140000000`.
Native assignments, consumers, clearing and destruction were checked using
decompilation and instruction offsets. This is a common-prefix implementation,
not a completed live shot/world restore.

## Direct Pointer Copy

The following are borrowed references. Their native setters perform plain stores,
not addRef/release, and their teardown does not destroy the referenced objects.
Restore their saved values directly after establishing retained dependencies.
Ranges are hexadecimal and half-open.

| Shot bytes | Evidence |
| --- | --- |
| `40..48`, transform parent | `1400376F0` stores parent and selector; `140548730` consumes parent matrices/joints; destructor `140547450` does not release parent |
| `1308..1310` and `2030..2038`, actor owner pair | `1401CD970` writes both; preMove `1401CCBE0` clears them coherently |
| `1480..1488`, cAtkCtrl owner | Shot ctor `1401C9B00` calls store helper `140057030`; controller destructor `14000BD70` does not release owner |
| `1490..1498`, borrowed rAtkInfo | Resource binding `1401CDAE0` takes `rShot+F0` through store helper `140057020`; retain the owning shot-resource generation |
| `1E30..1E48`, cArm self and attachment parents | Self setter `140253F70`; parent assignment `1401505B9`; dead-parent pruning `140150600`; consumers `14014FB60`/`14014F960`; destructor `14014F680` does not release parents |
| `1F70..1FE0`, callback slots 0..13 | Setter `1401C9AA0` is an indexed store; slot 0 starts as self; slot 13 is `1FD8`, not `1FE0` |
| `2000..2038`, seven unit references | Plain setters `140151760`, `140151780`, `1401CDAD0`, `1401C8A60`, `140151770`; contact write `1401C6332`; preMove clears dead references without release |

The existing recovered uShot field labels around `2000` are shifted. Instruction
and property offsets establish current owner `2000`, target `2008`, parent shot
`2010`, reflection owner `2018`, last hit `2020`, request unit `2028`, actor-owner
mirror `2030`. `2038` is a float, not another pointer.

Callback helpers `1401C99F0` and `1401C9AB0` inspect/prune only slots 1..13.
Kill `1401C9A20` dispatches virtual `+78` before clearing a live selected callback;
finish helpers `1401C9910`/`1401C9890` change effect state without clearing the slot.
None acquires/releases ownership. Native alive test `140001B80` dereferences
object state at `+10`; it is not dangling-pointer validation.

## Hit History

Inline cAtkCtrl begins at `1470`. Constructor `14000BCD0` constructs three arrays
of 32 records, stride `18`, at `14C0`, `17C0`, `1AC0`. Each record is vptr `+0`,
target `+8`, count `+10`, float time `+14`.

Reset `14000D180` clears exactly the 16 payload bytes. Commit `14000D2C0` compares
target pointers, writes new targets directly and changes count/time. Clear target
`14000C6E0` and update `14000D680` can clear only the pointer, retaining counters.
Destructor `14000BD60` changes the vptr, but does not release the target.

The kernel therefore restores every record's `08..18` together, preserving live
constructed vtables. It must not run after native controller destruction. The
existing deleting-hook quarantine prevents the destructor from running, rather
than merely suppressing the final outer free.

## Interpreter

At shot `1348`, constructor `140012EE0` and destructor `140012F80` establish that
the interpreter owns neither command mirrors nor return objects. Resolver
`140013540` derives entry/descriptor/block pointers from command-resource data;
advance `140013040` moves the frame descriptor and timed block together with key
index. Bind `140013680` can execute commands, so restore does not call it.

The kernel copies mirrors `1360..1380` and type/key/cursors `1388..13B8` directly
after storage staging selects the retained owned resource references. Existing
terminal retention also protects displaced resources backing older mirrors and
interior cursors, including a null current owned binding. No snapshot addRef.

Return flags `13D0` and eight-byte value `13D8` are separate fields, not a verified
tagged scalar union. Setter `1401079F0` stores a pointer. Producers include effect
creation `1401059C2`, shot creation `140106376`/`1401063F1`, and owner selection
`140104D05`; consumers `14010698C`/`1401069E0` dereference it as a shot. Its unit
lifetime is leased separately from resource lifetimes.

All 25 parser calls to the return setter were checked: 16 write literal null and
nine write object results. Command-effect factory `140120FE0`, including Capcom
`140124630` and Marvel `140129BE0` dispatchers, returns effect/model-derived units
(including ex-attack-space and character effects) or null, not sound handles,
resource pointers or vector temporaries. Shot factories trust the resource class
DTI; this conclusion assumes native valid resource data. `140106701` clears the
slot for opcode `3_32`; it is not a shot-result store. The result stores for
`3_30/31/32` are `1401063F1`, `1401065FA`, `140106886`.

Preserve the stable interpreter owner `1380`, opaque qword `13B8`, opaque attack
word `1478`, apparent arm alignment `1DC8`, and alignment gaps. Constructor zeros
alone do not establish reference or scalar semantics.

## Retention And Limits

`VisitShotUnitReferences` enumerates only these verified unit-reference fields,
including history targets and interpreter return. Snapshot capture uses the
existing unit-dependency adoption/lease mechanism, following newly adopted shot
members by index without invalidating the traversal. Self edges need no extra
lease. Unknown native unit types use their native DTI ancestry/deleting dispatch,
not a concrete-type whitelist or address heuristic. Native dispatch remains
outside the recorder lock, under the native unit/shot capture fences.

Before any CPU write, staging checks each saved nonnull unit address against the
slot's retained identity and birth ID. It does not filter out nonlive targets or
remap pointers. The shared visitor also extends the existing future-shot borrower
collection fence to callbacks, history and interpreter returns.

This preserves addresses, not arbitrary historical object state. Unsupported
effect graphs can now be held by a shot dependency even when they are not captured
as restorable effects; their logical resource/graph mutations still need restore
coverage. Such retention must not be advertised as effect resurrection.

Hyper Mystic Blast has a separate borrowed effect at `2160`: setter `1401AB6B0`,
pruning in move `1401AB5E0`, scale/time mutation `1401AB510`. It lies outside the
common prefix and is not handled by this patch. Its destructor `1401AB3B0` does
not release that pointer. Base callback helpers do not cover it.

Intrusive lists, sUnit execution, reflection team/list/collision changes
(`1401C5ED0`) and reciprocal constraint graphs remain separate transaction work.
These stages remain unwired from live frame restore. Presentation/audio ownership
continues to stay live.

## Verification

76 Python tests (including extracted production ASan/UBSan native mocks), eight
viewer tests, `make -j8`, and `git diff --check` passed. Regressions cover all 96
history payloads/vtable preservation, exact callback restoration without liveness
filtering, unsupported-effect deletion deferral across two snapshot slots,
untracked-unit adoption, retained command mirrors/cursors after replacement, and
pre-write rejection of expired or reused target identities. The timer/pose
`11 -> 3 -> 4` regression still passes. There is no new live gameplay validation.
