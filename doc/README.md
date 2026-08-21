# Documentation Index

Verified against the workspace source and the Ghidra project
`ichneumon_sub/umvc3.exe.unpacked.exe` on 2026-07-23.

## Authority

Use documentation in this order when claims conflict:

1. Current source under `UMVC3Hook/code/`.
2. [`current.md`](current.md), which summarizes the implemented architecture.
3. Focused documents marked **Current reference**.
4. Reverse-engineering notes marked **Native reference**.
5. Investigations and raw captures marked **Historical** or **Raw evidence**.

Game virtual addresses refer to the analyzed unpacked executable with preferred
image base `0x140000000`. Runtime code must relocate them through `_addr()`.
Decompiler-generated names are hypotheses unless the document says they were
confirmed by instructions, callers, and field usage.

## Current References

| Document | Scope |
|---|---|
| [`current.md`](current.md) | Implemented plugin architecture and invariants |
| [`shot.md`](shot.md) | Native projectile ownership and current rollback reconstruction |
| [`scollision_rollback_snapshot.md`](scollision_rollback_snapshot.md) | Current collision snapshot policy |
| [`mprgf.md`](mprgf.md) | Confirmed `cChr::mPrgF` findings |

## Native References

These describe the unmodified game, not the active ENet rollback protocol:

- [`dti.md`](dti.md)
- [`area.md`](area.md)
- [`intro.md`](intro.md)
- [`main.md`](main.md)
- [`nativenet.txt`](nativenet.txt)
- [`native_online_saction_time_sync.md`](native_online_saction_time_sync.md)
- [`net.md`](net.md)
- [`pad.md`](pad.md)
- [`pad2.md`](pad2.md)
- [`pause.md`](pause.md)
- [`replay.md`](replay.md)
- [`sbattlesetting.md`](sbattlesetting.md)
- [`sound.md`](sound.md)
- [`sai.md`](sai.md)
- [`sunit_categories.md`](sunit_categories.md)
- [`thread.md`](thread.md)

## Historical Investigations

These preserve observations and reasoning from particular builds or captures.
They are not descriptions of the current implementation:

- [`dash_resim_root_cause.md`](dash_resim_root_cause.md)
- [`fable3`](fable3)
- [`fable3raw`](fable3raw)

Historical root-cause documents may contain claims that were disproved by a
later capture. Their headers identify the superseding material. Preserve them
as evidence; do not copy their proposed fixes without checking current source.
