# Gameplay Chain Resource Audit

Read-only installed-resource and Ghidra audit, 2026-10-08. No gameplay policy
changes were made. Installed assets were not authenticated against a clean depot.

## Collision Dependencies

Scanned all 509 ARC files in `nativePCx64/chr/archive`, including alternate
costumes, parameter archives and common resources. Deduplication by resource path
and decompressed SHA-256 found 103 distinct CHN variants across 38 character
names. Every CHN had a matching CST setup entry. Eight chains have direct joint
overlap with gameplay collision records in the matching owner namespace:

| Character | Chain | Nodes | Collision overlap |
|---|---|---:|---|
| Rocket Raccoon | `chain_sippo` | 5 | Attack sets 11 and 34, logical joints 131-135 |
| Felicia | `chain_tail` | 8 | Attack set 30, logical joints 185-188 |
| Shuma-Gorath | `chain_Down` | 11 | Attack and body collision |
| Shuma-Gorath | `chain_L_down` | 11 | Attack and body collision |
| Shuma-Gorath | `chain_L_up` | 11 | Attack collision |
| Shuma-Gorath | `chain_R_down` | 11 | Attack and body collision |
| Shuma-Gorath | `chain_R_up` | 11 | Attack collision |
| Shuma-Gorath | `chain_Up` | 11 | Attack collision |

All eight resources are identical across their eight installed costume archives.
Their CST entries bind to the character skeleton, not a weapon. Collision overlap
is confirmed statically; action activation, native blend/stop gating, commit versus
consumer ordering, and actual physics-induced collision changes still need runtime
verification for Felicia and Shuma. Rocket has the prior joint/capsule/hit capture.
Set numbers are collision-set IDs, not action IDs or named moves.

Ghost Rider's `chain_kusari_00` and `chain_kusari_01` bind to weapon slot 2
(`Chain_normal`), Vergil's `chain_saya` binds to slot 1 (`Saya`), and Thor's
`chain_wepon2` binds to slot 0. None overlap CLI records attached to the same weapon
slot. Their apparent character-body joint overlaps were false positives.

## Evidence And Limits

- `rChrColList__convert`, `0x14005B710`: CLI header, 8-byte index entries,
  32-byte primitive records. Owner at record +0, logical joint at +8.
- `uCharacter__loadCollisionSet`, `0x14004B260`: bank 0 constructs attack capsules;
  banks 1/2/3 construct spheres for body/push/other non-push collision.
- `rChainSetup__convert`, `0x140295F10`: CST header of 20 bytes and 84-byte CHN
  setup entries. Entry +0x48 bit 0 selects weapon skeleton; +0x4C is its slot.
- `uCharacter__buildCollisionAttachmentsFromResource`, `0x14004D460`, and
  `uCharacter__createAndRegisterChrChain`, `0x1400354D0`: enforce source selection.
- `uChrChain__enforceChainSegmentLengths`, `0x140036800`: commits listed node
  joints, not all skeletal descendants.
- Capsule history at character +0x4D10 retains previous endpoints. A dependency
  can persist across frames after a physics-driven pose was committed.
- Landing queries, active joint holds, character-specific callbacks, later
  attached-model propagation and shared RNG are additional possible dependencies.
  No CLI overlap is not proof of presentation-only behavior.
- Model symmetry overlaps were checked for character-sourced chains; no additional
  symmetry-only candidate was found. Weapon symmetry and dynamic rebinding are not
  exhaustively covered.

Reproduce the candidate inventory with `tools/audit_gameplay_chains.py ARCHIVE_DIR`.
Full generated inventory is `/tmp/opencode/gameplay_chain_audit.json`.

## Rocket Timing

The `nplive_20261008_103624` profiles now contain enabled native-tail hook timings:

| Peer | Replay frames | Calls per phase | Move total | Post-move total | Combined per tail | Combined per frame |
|---|---:|---:|---:|---:|---:|---:|
| Host | 635 | 1270 | 1.2867 ms | 17.7854 ms | 15.02 us | 30.03 us |
| Client | 444 | 888 | 0.9202 ms | 15.3311 ms | 18.30 us | 36.60 us |

Two tail callbacks occur per phase per replay frame. Seven replay frames therefore
cost approximately 0.21-0.26 ms for both tails, or 0.105-0.128 ms per tail, at these
workload averages. These are elapsed callback timings, not a worst-case CPU budget
or a solver-only benchmark. Maximum single post-move callbacks were 192.2 us on
host and 1245.5 us on client; scheduling versus deferred/native work is unresolved.
Hook timings are nested within object timings and must not be added to them.
No solver-history snapshot was added by this audit.
