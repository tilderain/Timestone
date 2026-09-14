# `UMVC3Hook/code` source-bloat audit — 2026-09-12

## Conclusion

The strongest supported finding is **1,372 nonblank, noncomment source lines
(3.8%) in specifically identified dead, unreachable, or unused paths**. This is
a measured candidate subtotal, not a tested deletion patch or an estimate of
binary size. Removing definitions also requires cleaning up their declarations,
call sites, state, and any source-extraction tests.

A reasonable first cleanup budget is **approximately 1,500–1,700 code lines
(4–5%)**, including the dormant camera/settings implementation and additional
orphaned constants and tracking. Further savings from refactoring exist, but
are not included in that estimate. There is no evidence here supporting a claim
that a large fraction, such as 20–30%, can simply be removed.

## Scope and method

- All **100 files** in `UMVC3Hook/code` were included: 46 `.cpp`, 52 `.h`,
  and two generated `.inc` files.
- Corrected baseline: **36,207 code lines**, 3,600 comment lines, 3,967 blank
  lines; **43,774 physical lines**. Command:
  `cloc UMVC3Hook/code --force-lang="C++,inc" --quiet`.
- Every file received automated structural/reference checks: function inventory,
  low-reference functions, header declarations, orphaned top-level names,
  unconditional-return bodies, long commented blocks, and normalized repeated
  12-meaningful-line windows. All 46 `.cpp` files are in the Makefile.
- Flagged implementations and surrounding call/registration sites were manually
  inspected. References were checked through application sources, and selected
  candidates were also checked against project tools/tests.
- This is a static bloat audit, not a line-by-line correctness review of every
  function or a compiler-proven whole-program reachability analysis. The scan
  is heuristic; callbacks, ABI declarations, and conditional profiling need
  special treatment. A file marked “no concrete finding” is not certified free
  of unnecessary complexity.
- Function counts below include signatures and braces and exclude comments and
  blank lines. No runtime performance or binary-size savings were measured.

## Measured cleanup candidates

Locations are relative to `UMVC3Hook/code` and refer to the working tree at the
time of this audit.

| File(s) | Candidate code lines | Evidence |
|---|---:|---|
| `diag.cpp`, `diag.h` | 666 | Obsolete diagnostic hooks, unreachable loggers, diagnostic-only structures and buffers. Details below. |
| `rollback_cleanup.cpp` | 115 | Uncalled `RemoveMtDtiUnits`, `Rollback_DetachDrStrangeEffects` and its private helper, `Rollback_RepairOrphanedCutinRenderState`; empty, unused `Rollback_RemoveStaleUiUnits`. |
| `dtieditor.cpp` | 102 | Uncalled `FindDtiByName`, `FindVtableAddr`, `ScanForInstancesOfClass`, `HasTrackedInstance`, `TriggerPropertiesForDti_Generic`, and the latter's private `FindCreatePropertyFn`. |
| `controller.cpp` | 99 | Unused keyboard conversion, old save/restore pair and private state, four unused accessors, and unconditional-return logging routines. |
| `rollback_tick.cpp` | 79 | Four patch functions at lines 565–650 immediately return before their implementation. |
| `autotest.cpp` | 70 | Uncalled `IsManualCaseSelected`, `PlaceDefenderNextToAttacker`, `DumpRuntimeCommandArray`. |
| `rollback_model_policy.cpp` | 46 | Unregistered `hkKingKobunMove`; unused `Rollback_TraceShotPolicySpawn`. |
| `chrsel_imgui.cpp` | 41 | Uncalled `RequestRosterArchives`, lines 593–633. |
| `umvc3menu.cpp`, `umvc3menu.h` | 28 | Unused `runAheadEnabled`, `ShowHelpMarker`, and `swap_endian` template/include. Camera code excluded here. |
| `rollback_snapshot.cpp` | 24 | Unused `TraverseAndAddDynamicList`, lines 395–424. |
| `launch_music.cpp` | 24 | Uncalled `LaunchMusic_Stop` and `LaunchMusic_IsPlaying`. Actual asynchronous fade/start code remains in use. |
| `rollback_hooks.cpp` | 23 | `TraceLifecycleFrame` always returns false; three guarded log blocks are unreachable. |
| `netplay.cpp` | 18 | Unused public functions `Netplay_SendMultiResim` and `Netplay_SendInput`. This does not establish that their entire packet protocols can be removed. |
| `rollback_effect.cpp` | 13 | `hkGameEffectOperatorNew` remains defined; its registration is commented out. |
| `mk10utils.cpp`, `mk10utils.h` | 13 | Unused `_adjust`, its private `start`, `baseaddr`, and `UMVC3_PREFERRED_BASE`. |
| `exception.cpp` | 11 | `Exception_UpdateTestHotkey`; its only invocation in `dllmain.cpp` is commented out. |
| **Total** | **1,372** | **3.8% of 36,207 code lines.** |

### 1. Obsolete diagnostics are the largest clear target

`diag.cpp:693` installs only the `hkInputMaskPostProcess` hook. Registrations for
nine other hooks are commented out at lines 697–732 and 745–817.

Several loggers immediately return, including:

- `Diag_LogTeamState`, line 135;
- `LogSnapshotDiff`, line 285;
- `DiagLogPreOverride`, line 342;
- `DiagLogPostOverride`, line 382;
- `DiagLogPostResim`, line 485;
- `DiagLogActDbg`, line 520.

`DiagCaptureLiveRef` is still invoked at `controller.cpp:2132`, but the reads of
`g_liveRef` are in the unreachable `DiagLogPostOverride` body. Controller reset
also clears these records and the obsolete snapshot buffers. This is a
write-only diagnostic path in the current source; its compiled runtime cost
would need measurement because optimization can eliminate dead work.

**Preserve `hkInputMaskPostProcess` and its registration.** It temporarily clears
and restores a game fade/input flag during active netplay and is not merely a
logger. Removing the whole file indiscriminately would lose functionality.

The measured 666 lines cover `diag.cpp:12–14,16–51,53–660,685–691` and
`diag.h:6–82,85`. Controller call-site cleanup is additional to that subtotal.

### 2. Old patch APIs advertise work that never happens

`Rollback_PatchSleep`, `Rollback_PatchPresent`, `Rollback_PatchSwapchain`, and
`Rollback_PatchDrawCalls` all begin with `return;`. Their call sites,
saved-byte buffers, flags, and address constants add maintenance overhead.
This differs from an intentionally installed no-op hook: these helpers neither
patch nor suppress any native operation when called.

`controller.cpp` contains the same immediate-return pattern in
`LogReceivedPacket`, `LogOldInputs`, `Controller_DumpOldInputs`, and
`Controller_LogReSimFrame`.

### 3. Additional candidates outside the subtotal

- **Dormant camera/settings path:** the three camera hook registrations in
  `dllmain.cpp:1807–1809` are commented out. `UMVC3Camera.cpp/.h`, camera/FOV
  code in `umvc3.cpp`, and camera fields in `umvc3menu.cpp/.h` are left behind.
  There is no current assignment enabling `m_bFreeCam` in application sources.
  `eSettingsManager::Init` is entirely commented out, while its surviving
  key fields feed that inactive camera path. Roughly another 150 code lines
  are plausible cleanup across this cluster, subject to removing its call
  sites as a unit.
- **Write-only DTI allocation tracking:** `InstallAllocationHook` is called
  at `dtieditor.cpp:662`, and `HookedMtDTIGetSize` writes `g_allocatedDtis` at
  line 1054. The map has no reader in application sources. This is separate
  from `g_classInstances`, which the live inspector actually uses. The hook,
  map, and hook-specific descriptor/installation machinery warrant cleanup
  or a deliberate decision to restore a consumer.
- **Orphaned constants:** examples include eight names in `chrsel_imgui.cpp`,
  old controller vibration/key-conversion addresses, and 28 top-level native
  function-address constants in `rollback_tick.cpp`. Some constants document
  native layouts, so review their documentation value before removing them.
- **Unused cleanup/profiling API:** `SnapDump_Reset` has no application caller;
  decide whether lifecycle cleanup should call it rather than automatically
  deleting it. `Profile_AddTicks` is an unused convenience wrapper, but the
  rest of the profiling facility is intentionally configurable.
- **Source-extraction test dependency:** `tools/test_autotest_selectors.py:15`
  uses the text `bool IsManualCaseSelected(` as an extraction boundary.
  Removing this unused function requires updating that test's boundary.

## Repetition worth simplifying, without calling whole features bloat

1. **Launch-menu form handling.** IP/port extraction is repeated in
   `LaunchMenu.cpp:2941`, `:2985`, `:3166`, and `:3229`. Character selection
   updates duplicate substantial work between dropdown and editable-text
   handling at lines 3030–3083. A few focused helpers can reduce repetition.
   The compact menu already delegates into shared launch infrastructure;
   it is not a second full 3,000-line implementation.
2. **Netplay mailboxes and queues.** `netplay.cpp:943–1291` repeats locking,
   pending flags, packet assignment, and ring-buffer operations. Small typed
   mailbox/ring helpers are plausible. Preserve differences in overflow
   policy, generation checks, permission checks, and delivery behavior.
3. **Hook installation.** Several rollback modules repeat the same
   `MH_CreateHook`/`MH_EnableHook`/logging structure; other modules already
   have helpers. A common helper could remove boilerplate while retaining
   meaningful per-hook messages and failure behavior.
4. **Profiling JSON.** `rollback_profile.cpp:266–434` maintains a long format
   string in parallel with its argument list. A compact field-description
   approach could reduce maintenance, but output compatibility and net source
   savings would need verification.

These support further refactoring, not a measured additional deletion count.
Splitting a large function or moving code into another file is not a line-count
reduction by itself.

## Active-code verbosity and overengineering assessment

The 3.8% subtotal is deliberately narrower than “code that could be much
simpler.” The active implementation has considerable avoidable verbosity.
Most of it is repeated plumbing and awkward state/data representation rather
than elaborate class hierarchies or a generalized framework.

### High confidence: repeated hook-installation implementation

`controller.cpp:2233–2385`, `rollback_rng.cpp:416–586`,
`rollback_effect.cpp:1202–1343`, and `rollback_runtime.cpp:899–1132` repeatedly
implement the same create/enable/status/log sequence. The project already has
local `EnableHook`, `InstallHook`, and `InstallPolicyHook` helpers, so there is
an established simpler pattern.

This is not just formatting. Some sites check enable failure, others report
only create failure, and the RNG diagnostic hooks intentionally create without
enabling. A small shared helper with an explicit activation choice can express
these policies consistently. It needs no registry framework, dependency graph,
or elaborate template system.

**Planning estimate:** roughly 250–400 net code lines across these modules,
after accounting for the shared helper and excluding already counted dead
registrations/functions. This is an estimate, not a measured patch result.

### High confidence: bespoke mailboxes for every control message

In `netplay.cpp`, each small control packet gets its own payload, pending
flags, critical section, initialization, teardown, setter, getter, service-thread
extraction, and send sequence. Examples are at lines 76–111, 306–324,
385–447, and 985–1081.

The complexity of safely communicating between threads is necessary. Repeating
the same implementation for every packet type is avoidable. A small typed
latest-value mailbox with `Put`/`Take`, plus a common ENet send helper, would
leave packet-specific validation visible and reduce the mechanical code.
Bounded FIFO queues should remain a different primitive: overwrite-latest,
drop-oldest, and reject-full behavior are not interchangeable.

**Planning estimate:** roughly 150–250 net code lines for mailbox/send
boilerplate, without a wire-protocol redesign or merging all locks.

### High confidence: data reshuffling caused by mismatched interfaces

`LaunchMenu.cpp:1021–1041` is a particularly clear example:

1. Select the locally owned team.
2. Read that team's controls.
3. Construct three temporary **two-team** arrays by copying both players.
4. Pass them to `ConnectRoster_UpdateLocalTeam`.
5. That function selects one team again and copies only that team's values.

The file already has `GetTeamSelection(team)`. A helper accepting one team
view would express the actual operation without manufacturing the other team.

The underlying problem is broader: `LaunchDialogConfig` combines roster
values with network handles, while `TrainingBootConfig`, wire packets, and
`ConnectRoster` expose similar values in different forms. For example,
`EndConnectRosterSetup` has to copy roster fields individually to avoid
restoring stale network handles. Separate roster-value state from connection
state, and provide explicit conversions at packet/native boundaries. Preserve
the distinction between personal preferences, advanced-menu preferences, and
the negotiated match roster; those are genuinely different ownership domains.

Together with the four repeated IP/port readers and duplicate dropdown/text
selection handling, this is a realistic **100–200 net code-line** simplification
target. Do not change packet layout just to make an internal assignment shorter.

### High confidence: profiling output repeats its schema by hand

`rollback_profile.cpp:266–434` expresses JSON field names/order in one large
format string and repeats the ordering in its arguments. This is a maintenance
burden: adding one measurement requires keeping several representations aligned.

A short scalar-field writer and compact field/member descriptions can keep
names beside their values, preserving the JSON structure and numeric formatting.
Avoid introducing a heavyweight serializer solely for this output.

**Planning estimate:** roughly 50–100 net code lines; the reduction in
format/argument mismatch risk matters more than the line count.

### Medium confidence: state spread across too many independent globals

`framestepper.cpp:70–118` stores synchronization, restart, opening selection,
autotest launch, and HUD state in adjacent globals. `HookedMove`, lines
549–1046, coordinates all of these. The reset assignments at lines 674–691
and 735–750 repeat most of the handshake reset while differing in seed and
restart semantics.

Similarly, `LaunchMenu.cpp` mixes menu mode, connection role, persistence
ownership, readiness, chat, and network polling state.

The simpler direction is a few concrete state records (`PeerHandshake`,
`RestartRequest`, `RosterSession`) with explicit reset/transition functions.
Local and remote handshake progress remain independent; replacing all flags
with one enum would lose meaningful state. Centralize the common reset and
make seed retention/restart mode explicit.

This is a strong maintainability concern, but **no line savings are booked**:
the first clean refactor may be similar in size. Nor is the mutual READY/RELEASE
barrier itself redundant; its comments describe a real asymmetric-start hazard.

### Strongest scope-overengineering candidate: custom Win32 dark controls

`launchmenu_dark.cpp` contains 778 code lines for custom control painting and
compatibility. `InitializeLaunchMenuDarkMode` installs up to six UXTheme hooks;
the implementation tracks theme handles in a synchronized map and handles
Windows-version-specific exports and Wine scrollbar behavior.

That is substantial engineering for a cosmetic requirement. It is the clearest
place to ask whether the desired visual consistency warrants the maintenance
cost. The custom scrollbar path alone involves tracking, drawing, lifetime,
and installation machinery at lines 31–182 and 748–797.

However, accepting native/fallback scrollbar appearance would change the visual
requirement. These lines are **not** included in behavior-preserving savings.
If exact current appearance across supported environments is required, much of
this complexity has a reason.

### Smaller examples

- `LaunchMenu.cpp:249–250` has consecutive `<100` and `<140` cases returning
  the same `{3, 7}` result; the `<100` branch is redundant.
- `framestepper.cpp:27–68` redeclares many functions already available through
  included controller/netplay headers. Keep declarations in their owning headers.
- Executable-relative INI/path construction is repeated despite
  `LaunchMenuUtils::GetSettingsPath` already existing. Preserve distinctions
  between executable-relative and DLL-relative paths when consolidating.
- `ApplyLaunchMenuDarkTheme` invokes `InitializeLaunchMenuDarkMode` at both
  lines 822 and 839 without an intervening change to the dark-mode setting.
- `rollback_cleanup.cpp` has its own `MemoryRegionCache` despite the shared
  memory-query cache. This deserves review, but cache lifetime/invalidation
  differences must be established before treating one as redundant.

### Broader sizing judgment

The four high-confidence active-code targets above suggest approximately
**550–950 additional net code lines** of behavior-preserving simplification.
Combined with the initial 1,500–1,700-line cleanup budget, that gives a rough
**2,000–2,700 lines (about 6–7.5% of the folder)** as a practical first-pass
budget. This is an engineering estimate, not a measured removable percentage
or an upper bound. State-design improvements can be worthwhile without reducing
LOC; reducing cosmetic scope could save more but changes requirements.

## Commented-out code is a separate source of clutter

Examples excluded from the code-line subtotal:

- `rollback_snapshot.cpp:458–756`: 299 physical lines of disabled collision
  discovery/diagnostic implementation;
- `rollback_snapshot.cpp:871–950`: another 80 physical lines;
- `diag.cpp:697–732,745–817`: approximately 109 physical lines of disabled
  registrations (the last block shares a line with the live function's brace);
- `umvc3menu.cpp:119–253`: a long obsolete experimental-code tail.

That is hundreds of additional lines of source clutter, but removing them
does not reduce the 36,207 code-line baseline. Native layout explanations and
restoration invariants should not be conflated with abandoned implementation.

## What the audit does not justify deleting wholesale

- Generated address/field data: `propertytable.h` contributes 1,313 code lines;
  the two `.inc` files contribute 145. These are data volume, not evidence of
  unnecessary control flow.
- The active automated test runner, inspector, crash reports, snapshot dumps,
  and profiling capability. Optional developer tools still serve a purpose.
- Active shot/effect reconstruction, collision restoration, memory validation,
  synchronization, and native compatibility hooks. Their complexity requires
  behavioral evidence before removal.
- `d3d9reset.cpp:760` is an important counterexample: its empty
  `HookWindowRedraw` intentionally suppresses native WM_PAINT presentation to
  avoid concurrent rendering. An empty function can have essential behavior
  when it replaces a native callback.
- `steam_api_compat.h` ABI/vtable declarations cannot be trimmed merely because
  individual methods are not directly called by application code.

The release Makefile already enables LTO and section garbage collection, and
also suppresses unused-function/unused-variable warnings. Source cleanup is
therefore chiefly a maintainability result; it should not be presented as a
proportional speedup or executable-size reduction.

## Complete file coverage

“No concrete finding” means the file-wide checks did not establish removable
code in this audit. “Related declarations” means declarations should be cleaned
up together with the corresponding implementation, not independently.

| File | Result |
|---|---|
| `assist_names.h` | Lookup data; no concrete finding. |
| `autotest.cpp` | Three unused helpers; selector-tokenization repetition. |
| `autotest.h` | No concrete finding. |
| `chrsel_imgui.cpp` | Unused archive-request helper and orphaned constants. |
| `concrete_snapshot.cpp` | Small repeated capture block; no substantial removable path established. |
| `concrete_snapshot.h` | No concrete finding. |
| `connect_roster.cpp` | Small shared persistence implementation; no concrete finding. |
| `connect_roster.h` | No concrete finding. |
| `controller.cpp` | Dead logging, old state-save pair, unused keyboard conversion/accessors; diagnostic capture with no live consumer. |
| `controller.h` | Related declarations; unused layout/input constants. |
| `d3d9reset.cpp` | No concrete bloat finding; intentional empty replacement hook. |
| `d3d9reset.h` | No concrete finding. |
| `diag.cpp` | Largest obsolete diagnostic cluster; retain active input-mask hook. |
| `diag.h` | Diagnostic-only types/buffers/APIs; retain hook initializer. |
| `dtieditor.cpp` | Six unused helper functions; separate write-only allocation-tracking map/hook. |
| `dtieditor.h` | No concrete finding. |
| `effect_generator_state.h` | Focused shared capture/restore helper; no concrete finding. |
| `effect_pending_retention.h` | Focused shared queue helper; no concrete finding. |
| `eNotifManager.cpp` | Active notification implementation; duplicate preprocessor define is trivial cleanup. |
| `eNotifManager.h` | No concrete finding. |
| `escmenu_imgui.cpp` | Active small menu; no concrete finding. |
| `escmenu_imgui.h` | No concrete finding. |
| `eSettingsManager.cpp` | Empty initializer; dormant camera/settings cluster. |
| `eSettingsManager.h` | Dormant camera fields and unused general fields. |
| `exception.cpp` | Uncalled test-hotkey helper; retain crash handling/reporting. |
| `exception.h` | Related declaration. |
| `framestepper.cpp` | Large active orchestration/UI; isolated orphaned globals, no large dead path established. |
| `framestepper.h` | No concrete finding. |
| `gameui.cpp` | No concrete finding. |
| `gameui.h` | No concrete finding. |
| `generated_character_tail_fields.inc` | Generated restoration data. |
| `generated_shot_tail_fields.inc` | Generated restoration data. |
| `launch_music.cpp` | Uncalled stop/query APIs; active asynchronous playback/fade. |
| `launch_music.h` | Related declarations. |
| `LaunchMenu_compact.cpp` | Uses shared launch machinery; no concrete finding. |
| `LaunchMenu_compact.h` | No concrete finding. |
| `launchmenu_controls.h` | Existing shared control helpers; no concrete finding. |
| `launchmenu_dark.cpp` | Theme/control rendering feature; no substantial dead path established. |
| `launchmenu_dark.h` | No concrete finding. |
| `launchmenu_navigation.h` | Small shared transition policy; no concrete finding. |
| `LaunchMenu_settings.cpp` | Settings/UI/native configuration; no substantial dead path established. |
| `LaunchMenu_settings.h` | No concrete finding. |
| `launchmenu_utils.cpp` | Shared DPI logic; no concrete finding. |
| `launchmenu_utils.h` | Shared path helpers; no concrete finding. |
| `LaunchMenu.cpp` | Repeated form extraction/selection handling; no wholesale dead implementation established. |
| `LaunchMenu.h` | No concrete finding. |
| `mk10utils.cpp` | Unused `_adjust` and its state. |
| `mk10utils.h` | Unused address helper/constant/declaration. |
| `netplay.cpp` | Two unused send APIs; mailbox/queue boilerplate. |
| `netplay.h` | Related declarations; retain wire-format structures. |
| `propertytable.h` | Generated lookup data. |
| `rollback_cleanup.cpp` | Uncalled cleanup routines and one empty unused API. |
| `rollback_cleanup.h` | Related declarations. |
| `rollback_diagnostics.cpp` | Active watchdog/output-directory infrastructure; no concrete finding. |
| `rollback_diagnostics.h` | No concrete finding. |
| `rollback_effect.cpp` | One unregistered allocation hook; repeated hook-install boilerplate. |
| `rollback_effect.h` | Compile-time trace option; no wholesale deletion justified. |
| `rollback_hooks.cpp` | Permanently false lifecycle trace and guarded logs; repeated installation boilerplate. |
| `rollback_hooks.h` | No concrete finding. |
| `rollback_internal.h` | Documentation-only header apart from pragma; negligible code count. |
| `rollback_memory.cpp` | No concrete finding. |
| `rollback_memory.h` | No concrete finding. |
| `rollback_model_policy.cpp` | Unregistered King Kobun trace and unused spawn trace; small repeated model-loading block. |
| `rollback_model_policy.h` | Related declaration. |
| `rollback_profile.cpp` | Verbose JSON schema/argument duplication; profiling capability is intentional. |
| `rollback_profile.h` | Unused `Profile_AddTicks` wrapper; conditional profiling support remains useful. |
| `rollback_resim.cpp` | Calls into no-op patch/log APIs can be removed with those APIs. |
| `rollback_resim.h` | No concrete finding. |
| `rollback_restore.cpp` | Large active restoration implementation; no substantial dead path established. |
| `rollback_restore.h` | No concrete finding. |
| `rollback_rng.cpp` | Repeated hook-install boilerplate; no wholesale RNG-hook deletion justified. |
| `rollback_rng.h` | No concrete finding. |
| `rollback_runtime.cpp` | Repeated hook-install boilerplate; no substantial dead path established. |
| `rollback_shot.cpp` | Active reconstruction and scheduler cleanup; no substantial dead path established. |
| `rollback_shot.h` | No concrete finding. |
| `rollback_snapdump.cpp` | Uncalled `SnapDump_Reset`; determine cleanup lifecycle intent before deletion. |
| `rollback_snapdump.h` | Related declaration. |
| `rollback_snapshot.cpp` | Unused dynamic-list helper; large commented-out collision discovery blocks. |
| `rollback_snapshot.h` | No concrete finding. |
| `rollback_tick.cpp` | Four unreachable patch bodies, orphaned address constants, duplicated category predicate. |
| `rollback_tick.h` | No concrete finding. |
| `rollback.cpp` | Small hook composition entry point; no concrete finding. |
| `rollback.h` | No-op patch API declarations belong to coordinated cleanup. |
| `savedata.cpp` | Save decoding and roster extraction; no concrete finding. |
| `savedata.h` | No concrete finding. |
| `steam_api_compat.h` | ABI declarations; unused methods alone do not establish bloat. |
| `steam_lobby.cpp` | Active callback/discovery/connection state machinery; no substantial dead path established. |
| `steam_lobby.h` | No concrete finding. |
| `steam_transport.cpp` | Active ENet/Steam bridge; no concrete finding. |
| `steam_transport.h` | No concrete finding. |
| `trainingboot.cpp` | Isolated orphaned address constant; no substantial dead path established. |
| `trainingboot.h` | No concrete finding. |
| `umvc3.cpp` | Dormant camera/FOV branch and hook; preserve active per-frame processing. |
| `umvc3.h` | Related camera declarations. |
| `UMVC3Camera.cpp` | Dormant camera hook implementation. |
| `UMVC3Camera.h` | Related camera declarations. |
| `umvc3menu.cpp` | Unused helper/global, dormant camera fields, obsolete commented-code tail. |
| `umvc3menu.h` | Unused endian helper and dormant camera fields. |
| `updater.cpp` | Active updater bootstrap; no concrete finding. |
| `updater.h` | No concrete finding. |

## Suggested cleanup order

1. Remove obsolete diagnostics as a coordinated change, preserving the live
   input-mask hook and removing its obsolete controller capture/reset/log calls.
2. Remove the four no-op patch APIs and their callers/state.
3. Remove the individually verified unused helpers and declarations; adjust the
   autotest source-extraction boundary.
4. Resolve the dormant camera/settings cluster and unread DTI tracking.
5. Refactor targeted menu/network/hook boilerplate where the replacement is
   actually simpler. Validate each functional change with the relevant existing
   tests and normal `make -j8` build.

This audit changed documentation only. No cleanup patch was applied or built.
