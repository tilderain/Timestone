# Documentation Index

Updated on **2026-10-26** through twelve scoped native-research/documentation
passes and coordinator source/evidence reconciliation. Existing documents,
field ledgers, AI workflow specifications and raw historical transcripts are
preserved. There is no per-document word limit.

## Target And Verification

The live Marvel instance was **8193**, display name
`umvc3.exe.unpacked.exe`, project file `/umvc3xmario5.exe`, Windows
x64, preferred image base `0x140000000`.
Rediscover and check program identity each session; a port is not permanent
executable identity. Runtime hooks relocate preferred VAs with `_addr()`.

Each refreshed investigation identifies the native bodies/instructions/types
actually inspected and separates them from older evidence. Some passes also
checked specific plugin source files. Neither a source review nor an existing
Ghidra name/comment establishes runtime correctness. No game run, archive/save
revalidation, benchmark or new two-peer result is implied by the review date.
The database was inspected without changing shared types, names or signatures.

The checked game model now distinguishes common `uCharacter` (`0x68F0`) from
concrete allocation extents, live fighters from setup records, the separate
input/packet/snapshot/diagnostic rings, and native deferred retirement from
plugin reconstruction. Specific new corrections and remaining ownership gaps
are documented at their subsystem references below.

## Authority

Use evidence appropriate to the claim when descriptions conflict:

1. Current plugin source for implemented plugin behavior; native instructions,
   allocation/registration and actual consumers for unmodified-game behavior.
2. [`current.md`](current.md) for the source-reviewed architecture, followed by
   focused documents with explicit verification scope.
3. Older native/reference findings with their original confidence, concrete
   target and unresolved ownership qualifications.
4. Dated captures, benchmark results and raw transcripts as historical evidence,
   never as measurements of a later build merely because the document changed.
5. Design proposals and synthetic teaching examples as proposed work, not native
   functions, current packet schemas or measured runtime events.

Decompiler names, signatures and struct labels can be stale. Concrete alias
checks already found a property registrar named `sShot__move`, a material-loop
thunk mistaken for a separate hook boundary, and omitted ABI parameters. Preserve
address/width/receiver provenance instead of promoting labels to facts. A new
earlier mismatch is not proof that a previous patch succeeded; absent dumps or
matched excluded bytes do not certify deterministic gameplay.

## Current References

| Document | Scope |
|---|---|
| [`current.md`](current.md) | Implemented plugin architecture and invariants |
| [`shot.md`](shot.md) | Native projectile ownership and current rollback reconstruction |
| [`scollision_rollback_snapshot.md`](scollision_rollback_snapshot.md) | Current collision snapshot policy |
| [`mprgf.md`](mprgf.md) | Confirmed `cChr::mPrgF` findings |
| [`main.md`](main.md), [`thread.md`](thread.md), [`sunit_categories.md`](sunit_categories.md) | Native scheduling versus current headless/thread policy |
| [`desync_snapdump.md`](desync_snapdump.md) | Recorder gates, captured coverage, comparison and annotations |
| [`live_performance_benchmarks.md`](live_performance_benchmarks.md) | Passive collector versus future benchmark design |
| [`ini-persistence.md`](ini-persistence.md) | Transactional settings and roster-preference boundaries |

## Native References

Native references describe the unmodified engine. Where a file also discusses
the plugin, its source boundary is identified separately. Retail CMU/Steam input
and voice transport are not the custom ENet/Steam Networking Sockets bridge.

### Input And Commands

- [Pad hardware and SOCD](pad.md), [pad object/configuration distinction](pad2.md).
- [Mouse and ID input](mouse.md), [input-to-display timing](input_to_display_timing.md).
- [Command restrictions and mutable line state](command_matcher_states.md).
- [Corrected historical dash investigation](dash_resim_root_cause.md).
- [Heap-escaped command-state capture](fable3), [unaltered raw evidence](fable3raw).

### Networking And Match Setup

- [Native netpad architecture](net.md), [detailed native-net notes](nativenet.txt).
- [Native action time synchronization](native_online_saction_time_sync.md).
- [Native online predicate callers](ingameonlinechecks.md), [retail voice traffic](native_voice_chat_traffic.md).
- [Area lifecycle](area.md), [battle settings](sbattlesetting.md), [native replay](replay.md).
- [Intro flow](intro.md), [outro/restart flow](outro.md).
- [Pause gates](pause.md), [owner-local native pause proposal](netplay_pause_menu.md).
- [Opening-member synchronization](match_start_assist_sync.md).
- [Voice-language synchronization](native_character_voice_sync.md), [retail roster presets](saved_roster_presets.md).

### Fighters And CPU

- [Concrete field ledger](chrfields.md), [character ownership audit workflow](chrcheck.md).
- [Reserve updates](reservechr.md), [reserve/Rocket gating follow-up](reserve_move_gate_audit_20261013.md).
- [Program flag meanings](mprgf.md), [character replacement investigation](characterswap.md).
- [Fighter CPU scripts](cpu_scripts.md), [generic AI service distinction](sai.md).

### Projectiles And Effects

- [Shot reconstruction](shot.md), [concrete shot ledger](shotfields.md), [gameplay model inventory](shot_gameplay_models.md).
- [Native effect lifecycle](effect_lifecycle.md), [compact generator control](effect_generator_control.md).
- [Restart candidates](native_effect_projectile_restart.md), [Phoenix construction retries](phoenix_generator_retry.md).
- [Super presentation](super_presentation_resim.md), [exact effect-rewind proposal](effect_rollback_design.md).

### Collision, Constraints And Camera

- [Collision snapshot/clone policy](scollision_rollback_snapshot.md).
- [Embedded joint helpers](ccnshelper.md), [model layout](umodel.md).
- [Gameplay chain resources](gameplay_chain_audit.md), [Felicia/Shuma gating](felicia_shuma_chain_gating.md).
- [Rocket tail behavior and costs](rocket_tail_chain_performance.md).
- [Side-camera state and gameplay bounds](uactionsidecamera.md), [offline trail-repair model](trail_repair_simulation.md).

### Rendering And Materials

- [D3D9 recovery controller](d3d9_lifecycle_repair.md), [historical black-screen regression](d3d9_57d463b_regression.md).
- [Material-cache hypothesis](bandaidmaterial.md), [render/restart guards](render_crash_bandaid.md).
- [Shader attributes](shader_attribute_usage.md), [character presentation audit](character_headless_render_audit_20261015.md).
- [Material savings bounds](material_headless_savings_20261015.md).
- [Material resim audit](material_resim_skip_audit_20261016.md), [refined post-loop scope](material_post_scope_20261016.md).

### Randomness, Audio And Types

- [Global/owner RNG routing](global_rng_gameplay.md), [hitbox follow-up](global_rng_hitbox_followup.md).
- [Deadpool grenade resource-specific evidence](deadpool_grenade_rng.md).
- [Sound ownership](sound.md), [pain-voice mechanism and unimplemented fixes](pain_voice_fix.md).
- [DTI registration/layout](dti.md), [historical Ghidra operation transcript](ghidra_note).

## Performance And Snapshot Research

- [Character tick investigation](retick.md), [Spencer wrapper](spencer_move.md).
- [Native tick optimization](native_tick_optimization_20260911.md), [movement follow-up](character_movement_optimization_20260911.md).
- [Critical-section investigation](critical_section_elision.md), [historical rearchitecture findings](rearch.md).
- [Live collector](live_performance_benchmarks.md), [low-spec proposal](lowspec.md).
- [September performance review](rollback_performance_review_20260911.md), [recorded optimization results](rollback_optimization_results.md).
- [Compact snapshots](compact_snapshot.md), [snapshot necessity](resnap.md), [revised character restore audit](resnap3.txt).
- [Coverage proposal](snapshotcoverage.md), [desync recorder](desync_snapdump.md), [unproven epoch-offset hypothesis](desync_phase_offset.md).
- [Effect child work](effect_child_headless_audit.md), [filter-resource inventory](effect_filter_resource_inventory.md).
- [Effect capture procedure](effect_moveafter_capture.md), [October capture findings](effect_profile_findings_20261015.md).
- [Concrete effect targets](effect_moveafter_optimization_targets.md), [ordinary particle savings](effect_ordinary_movement_savings.md).

## Workflows And Proposals

- [Desync investigation specification](ai_desync_testing_spec.md): actual tool
  schemas, qualified verdicts, native evidence and explicitly synthetic examples.
- [Profile-guided optimization specification](ai_rollback_optimization.md).
- [Ghidra function documentation contract](ai_mass_ghidra_doc.md), [donor-field mapping contract](ai_mass_ghidra_fields.md).
- [Historical code-bloat audit](code_bloat_audit_20260912.md): estimates are not
  permission to remove current safety hooks or functionality.
- [INI persistence](ini-persistence.md), [H&H state/transport proposal](earthatk_rollback.md).

The character-tail audit parses specific sections in `chrfields.md`. Preserve
its headings and tables when editing prose, or explicitly update and test the
consumer. Shared evidence/schema changes need their own review. Missing historical
companions are identified in their documents rather than fabricated or recreated.

## Historical Investigations

These preserve observations and reasoning from particular builds or captures.
They are not descriptions of the current implementation:

- [`dash_resim_root_cause.md`](dash_resim_root_cause.md)
- [`fable3`](fable3)
- [`fable3raw`](fable3raw)

Historical root-cause documents may contain claims disproved by later captures.
Their headers identify controlling corrections. Preserve raw transcripts as
evidence, but do not copy abandoned patches or hypothetical examples into source.
Per-class documentation already absent before this refresh was not recreated;
missing class files do not authorize inventing layouts from similar names.
