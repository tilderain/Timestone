# Desync Snapshot Dumps

Enable the code-only diagnostic switch before building:

```cpp
// UMVC3Hook/code/rollback_snapdump.h
#define ROLLBACK_SNAPDUMP_ENABLED 1
```

It is currently enabled with value `1`. Setting it to `0` removes the recorder
implementation and all integration calls during preprocessing. Such a build
performs no snapdump hashing, allocation, copying, branching, or desync file
work and has no snapdump history memory allocation.

The rollback plugin retains the last 128 packed rollback snapshots independently
of optional NDJSON logging. At the normal confirmed-checksum desync trigger, both
peers write a `desync_*_host_fN.usdp` or `desync_*_client_fN.usdp` file beside the
plugin DLL.

Each frame contains:

- The exact bytes in `RollbackSnapshotSlot::data`.
- The ordered region manifest, addresses, owner slots, expected vtables, region
  hashes, sizes, and restore flags.
- Local and remote inputs assigned to the snapshot.
- Shot, effect, collision-clone, and model-render-state counts.
- Whole-snapshot and manifest hashes.
- Stable construction-time names such as `globalRng`, `sAction.full`,
  `uCharacter.team0`, and `uCharacter.motionChannels`.

Re-simulation replaces the same absolute frame in the 128-slot history, so the
dump represents the corrected canonical timeline rather than stale prediction.
The on-disk payload uses XOR run-length encoding against the preceding frame.
At the expected 2-3 MiB per snapshot without projectiles, the in-process history
uses approximately 256-384 MiB plus dynamic shot/collision data. Each slot is
released when its absolute frame is overwritten or rollback state is reset.

The per-frame capture path performs one snapshot-byte copy and one compact
region-metadata pass. Slot storage is reused and grows in 1 MiB increments.
Snapshot hashes, per-region hashes, name expansion, manifest construction, and
XOR/RLE compression are deferred until the desync trigger has paused the match.

## Compare Peers

Run:

```sh
python3 tools/compare_desync_dumps.py host.usdp client.usdp \
  --output desync_report.json
```

The command prints a short verdict and writes a complete JSON report suitable
for inspection by a person or AI. It exits `0` when all overlapping snapshots
match, `1` for a confirmed divergence, and `2` for malformed input or I/O errors.

The comparison performs three checks in order:

1. Cross-peer input agreement: host-local must equal client-remote and vice versa.
2. Snapshot structure agreement: ordered region identities, sizes, owner slots,
   expected vtables, restore flags, and dynamic recipe counts.
3. Byte agreement after normalizing ASLR image pointers and heap pointers that
   target the same snapshot region and offset.

The report identifies the first divergent frame, region, byte offset, values,
and surrounding bytes. Later divergent frames are summarized as propagation.
Version 2 dumps carry region names directly. The comparison tool remains
compatible with version 1 dumps and infers names for known singleton and
fixed-size character regions where possible.
Unknown pointer-like values are deliberately retained as differences rather than
hidden, because they may be gameplay state or evidence of a different object graph.

## Ghidra Field Annotations

Run `ExportSnapdumpStructs.java` from the Ghidra script manager against the
current `umvc3.exe.unpacked.exe` program. It writes:

```text
ghidra_export/snapdump_structs.json
```

The export is generated directly from Ghidra's current data-type manager and
executable. It includes every current structure, recursively flattened named
fields, and recovered DTI/vtable identities. Character and shot families are
also identified through current virtual methods. It does not consume the older
property JSON or TSV audits.

`compare_desync_dumps.py` loads that path automatically when present. Exact
saved vtables select concrete character, helper, and shot types. Stable region
bindings annotate partial singleton captures such as `sReplay.frameCounter`,
`sEffect.rng`, `sShot`, `sAction`, `sCamera`, and `cChrTeam`. Each differing run
then reports overlapping Ghidra field names, types, full object offsets, and
decoded values for known scalar types.

Use a different export or disable annotations with:

```sh
python3 tools/compare_desync_dumps.py host.usdp client.usdp \
  --structs path/to/snapdump_structs.json
python3 tools/compare_desync_dumps.py host.usdp client.usdp --no-structs
```

Heap arrays such as motion blends, joint states, and command-line buffers keep
their explicit snapdump labels when no unambiguous Ghidra element structure is
known. The analyzer never guesses a concrete type from size alone.

## Verification

```sh
python3 -m unittest tools/test_compare_desync_dumps.py
make -j8
```
