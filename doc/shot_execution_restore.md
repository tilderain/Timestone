# Shot Execution Restore

2026-11-09, native audit in Ghidra port 8192, `umvc3.exe.unpacked.exe`, preferred
base `140000000`. The execution stage is tested but not called from production
frame restore. The conditional render-worker wait is enabled at restore entry.

## Scheduler Contract

`sUnit` category descriptors start at `38`, stride `30`: vptr `00`, name `08`,
flags `10`, head `18`, tail `20`, delta coefficient `28`. Head-relative access
is therefore `sUnit+50+i*30`. There is no stored category unit count or traversal
cursor to rewind: `MoveLine::getCount` (`14051B170`) walks unit `next+18`.

`addBottom` (`14051A6B0`) and `remove` (`14051B9F0`) update reciprocal links,
endpoints and category bits, not a count. Group registration (`14051BDE0`) invokes
system and unit virtual callbacks, so replay publication must not use it.
Group removal (`14051BAF0`) removes pointers from each selected array, then
clears unit mask `30`. Groups have size `18`, capacity `1C`, ownership byte `20`,
data `28`, relative to their descriptors at `sUnit+C40+i*30`.

`StageShotExecutionState` validates snapshot/thread/manager/slot identity,
retained birth IDs, body bounds, category/mask membership and list consistency
before writing topology. Future-only live shots must already be withdrawn.
Historical foreign pointers are never dereferenced or resurrected: surviving
current foreign entries retain membership and missing historical ones are omitted.
Saved cohort ordering is reinstated, followed by newly encountered foreign units.
Current native array allocations/capacities/ownership remain valid. Capacity
preparation calls only native array expansion, outside the recorder lock.

The same publication loop handles live and quarantined objects, including a
snapshot-held dead dependency that an earlier rewind has since revived. Native
flags restore lifecycle/category (`3FF`), move (`400`), fixed (`2000`) under mask
`27FF`; presentation flags remain live. `cUnit::isFix` (`140016F10`) confirms bit
13, which affects kill (`140016F20`). Quarantine accounting and `futureShot`
metadata follow the saved identity. No constructor, initialization, move, kill,
group-registration callback or destructor runs during publication.

Native lifecycle paths (`14051B4C0`) distinguish state 1 initialization, 2 active,
3 retirement and 4 disposal. A detached pending unit can remain state 1 with
category `7F`; publication does not initialize it ahead of its native traversal.
This stage does not publish shot IDs/team lists, constraint joint topology or dependent effects.
Common constraint execution state is now included in the same publication loop:
saved category/group order, lifecycle and movement flags rewind under construction
provenance checks. Proven future inline constraints are withdrawn without kill or
deletion callbacks. Graph linkage/cache repair remains a separate prepared stage.
The separate shot-manager stage now restores ID/team from the saved body along
with ordered shot lists and occupancy. It no longer assumes another stage has
already undone reflection; the CPU kernel intentionally leaves registration live.

## Quiescent Boundary

Native main tick (`1402594A0`) calls input update at `1402594BD`, before render
begin (`14025957F`) and unit move/sync/after-move (`140259670`, `1402596A0`,
`1402596AD`). Unit traversal cursors are stack locals. Category jobs are submitted
and joined by `sMain::executeJob` (`140521230`) before their pass returns.

Neither `sUnit::sync` (`14051BFF0`/`14051BF10`) nor after-move
(`14051B850`/`14051B6F0`) is a passive drain: they invoke unit virtuals, can
initialize state-1 units, and can advance gameplay. Do not call them to prepare
restore. The unit critical section is not a substitute for render completion.

`endFrameAndPresent` (`14053A250`) can return while threaded playback is still
outstanding when pacing polls completion and sees `WAIT_TIMEOUT`. Thus a worker
can survive into the next input hook even though unit jobs have completed.

`LifetimeTrace_WaitForRenderWorker` checks renderer singleton `140E179A8`, active
frame byte `38` and latched threading byte `3E`. Only when both are set does it
call `sRender::sync` (`140541030`). That branch waits on completion handle `100`
and clears `38`; it does not execute inline playback. Configured threading byte
`3D` is not sufficient. The call is before region writes and native unit locks.

This drains the verified concurrent reader, not retained draw-list ownership.
Resources/allocations still borrowed by carried draw lists must remain valid;
the wait does not clear caches, rebuild materials, or claim that all asynchronous
owners have been closed.

## Tests And Integration

Native-mock tests cover exact category/group ordering, current foreign retention,
native group capacity growth, live/quarantined/detached publication, re-quarantine
of a saved dead dependency, repeated restore without accounting drift,
presentation-bit preservation and unchanged descriptor controls. Invalid leases,
duplicate bodies/memberships, mismatched masks, array ownership, unwithdrawn shots
and stale slots reject before topology or capacity writes. The render test covers
all active/configured/latched flag combinations.

Whole-shot publication still needs coherent constraint graphs, concrete suffix
coverage and dependent-effect state. Production does not yet call storage, CPU,
shot-manager or execution stages. A passing scheduler component test is not a
live Mystic Blast repair claim.

Validation: 79 Python tests (including ASan/UBSan native extraction tests), eight
viewer tests, `make -j8`, and whitespace checks passed. Follow-up manager tests
also exercise genuinely changed live IDs/team values, not just changed list links.
