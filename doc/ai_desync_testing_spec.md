# AI Desync Testing Specification

**Status:** Active implementation specification  
**Scope:** UMVC3Rollback deterministic rollback desync detection and root-cause diagnosis  
**Primary consumer:** An automated AI test controller with Ghidra integration

## 1. Core Objective

Detect rollback desyncs, reproduce them reliably, and **classify the root cause** 
into one of two fundamental failure modes:

1. **Missing snapshot field**: A game object field is not captured or restored by 
   the rollback snapshot system, causing divergence after re-simulation.
2. **Incorrect shot recipe**: A projectile (shot/effect) is created with 
   different parameters or timing on the host versus client during re-simulation.

Every confirmed desync must be traced to its **first divergent frame and byte**, 
then investigated in Ghidra to determine which field or shot creation path is 
responsible.

## 2. Test Outcomes

| Outcome | Meaning |
|---|---|
| `confirmed_desync` | Snapdump comparison confirms divergence; root cause classified. |
| `stable_sync` | All cases completed; no `.usdp` artifacts or comparator reports sync. |
| `incomplete` | Suite did not complete all expected cases. |
| `launch_failed` | Wine or game processes did not start correctly. |
| `analysis_failed` | Snapdumps exist but comparison or Ghidra lookup failed. |

`incomplete`, `launch_failed`, and `analysis_failed` are never treated as 
`stable_sync`.

## 3. Test Protocol

### 3.1 Discovery Run

Launch the game with a selected character and focused command ID:

```bash
wine umvc3.exe -autotest=<character> -autotest-moves=<command-id> \
  -autotest-repeat=1 [-autotest-port=<port>]
```

- `-autotest-port=<port>` selects the autotest host's ENet port from `1` to
  `65535`. When omitted, the saved launch-menu port is used (default `7777`).
  The spawned client receives the same port automatically through `-connect`.
- The focused selector must be the numeric command ID emitted by the
  `AutoTestSuite: list=<id>` log entry, not the human-readable command name.
- For example, if the log contains
  `AutoTestSuite: list=3 ... command="SERVBOT SURPRISE"`, launch with
  `-autotest-moves=3`, not `-autotest-moves="SERVBOT SURPRISE"`.
- Command names are display labels and may be duplicated, localized, or
  changed by command-list extraction. They must not be used as the primary
  selector for reproduction.
- Record both the numeric command ID and the displayed command name in every
  test result, report, and diagnosis so the result remains human-readable while
  reproduction uses the stable ID.
- For runtime command-list entries, the authoritative selector is the pair
  `matcher_id` and `line_id` from the `AutoTestSuite: ... matcher=<id>
  line=<id>` log entry. The AI must launch and reproduce a runtime command by
  this matcher/line pair, written as `<matcher-id>:<line-id>` in
  `-autotest-moves`, not by its displayed name or by a regenerated list
  ordinal.
- The autotest TSV must contain `matcher_id` and `line_id` columns for every
  row. Preserve these values when importing a failed TSV or creating a focused
  rerun. A missing or `-1` matcher/line pair is an invalid runtime test
  selector and must be reported as `incomplete`, not treated as a successful
-   reproduction.
- Manual scripts such as `Forward Throw` and `Back Throw` are separate from
  runtime command-list entries. They legitimately use `matcher_id=-1` and
  `line_id=-1`; do not use those rows to identify runtime commands.
- Default repeat count: **1**. A single execution is sufficient to detect a
  deterministic desync.
- The autotest suite forces the **client to re-simulate every confirmed frame**, 
  providing comprehensive rollback stress without additional fault injection.

Wait for the suite to complete. The host log must contain:

```
AutoTestSuite: command-line suite complete
```

If the suite does not complete, classify as `incomplete` and retry.

### 3.2 Artifact Discovery

For autotest runs, search under the dedicated snapdump root:

```
/media/god/tempData/SteamLibrary/steamapps/common/ULTIMATE MARVEL VS. CAPCOM 3/autotest/
```

Autotest snapdumps are sorted into character-named folders below this root. The
character name comes from `-autotest=<character>`, so a Tron run is expected
under:

```
/media/god/tempData/SteamLibrary/steamapps/common/ULTIMATE MARVEL VS. CAPCOM 3/autotest/Tron/
```

Search the selected character folder first, and only consider artifacts created
after the launch timestamp. Do not use stale snapdumps from another character
or an earlier run. Within that folder, match host and client snapdumps by the
same timestamp and trigger frame:

```
desync_<TIMESTAMP>_host_f<FRAME>.usdp
desync_<TIMESTAMP>_client_f<FRAME>.usdp
```

The timestamp and trigger frame must match between host and client. If the
character folder is missing, the run produced no usable autotest artifacts;
classify the result according to the completion and artifact rules below rather
than scanning unrelated character folders.

### 3.3 Snapdump Comparison

For every matching pair, run:

```bash
python3 tools/compare_desync_dumps.py host.usdp client.usdp \
  --output report.json
```

The comparator performs:

1. **Input agreement check**: Host-local input must equal client-remote and vice 
   versa.
2. **Structure agreement check**: Region identities, sizes, vtables, and dynamic 
   counts must match.
3. **Byte agreement check**: Authoritative game state must match after 
   documented pointer/heap normalization.

Exit codes:
- `0`: All overlapping frames matched.
- `1`: Confirmed divergence detected.
- `2`: Malformed input or I/O error.

The exit code is the **authoritative desync verdict**. TSV rows and log messages 
are supporting evidence only.



## 4. Root Cause Investigation and Fix Iteration

When `compare_desync_dumps.py` exits `1`, it writes a JSON report containing:

```json
{
  "verdict": "diverged",
  "first_divergent_frame": 58,
  "first_divergent_region": "uCharacter.team0",
  "first_divergent_offset": 5180,
  "host_value": "30 01 00 00",
  "client_value": "2b 01 00 00",
  "context_before": "20 00 00 00 15 00 00 00 48 02 00 00",
  "context_after": "01 00 00 00 c0 3f 00 00 00 00 00 00"
}
```

**Field definitions:**
- `verdict`: Always `"diverged"` when exit code is `1`.
- `first_divergent_frame`: Net frame number where divergence was first detected.
- `first_divergent_region`: Memory region name (e.g., `uCharacter.team0`, 
  `sShot`, `uShotBeam.team1[3]`).
- `first_divergent_offset`: Byte offset within the region where first mismatch 
  occurred.
- `host_value`: Hex bytes from host snapshot (4-16 bytes for context).
- `client_value`: Hex bytes from client snapshot (same length as host_value).
- `context_before`: 12 bytes of hex data immediately before divergent offset.
- `context_after`: 12 bytes of hex data immediately after divergent byte(s).

The comparator may also include optional fields:
- `vtable`: Hex address of the vtable pointer for dynamic types (uShot, uEffect).
- `element_index`: Array index if divergence is within a dynamic array.
- `suggested_type`: Ghidra type hint based on value patterns (e.g., `"float"` if 
  bytes match IEEE 754 format).

### 4.1 Ghidra Field Lookup

Load the JSON and extract:
- `region` (e.g., `"uCharacter.team0"`)
- `offset` (e.g., `5180` = `0x143c`)
- `host_value` and `client_value`

Open Ghidra with the current `umvc3.exe.unpacked.exe` analysis and locate the 
corresponding structure:

- If region is `uCharacter.*`, search for `uCharacter` or `cChr` in the data 
  type manager.
- If region is `sShot`, `sAction`, `sEffect`, search for those singletons.
- If region is `uShot.*` or `uEffect.*`, identify the concrete type using the 
  vtable recorded in the snapdump.

Navigate to the byte offset within the structure. Ghidra should display:

- Field name (e.g., `mAnmSerial`, `mFrameTimer`, `mActionId`)
- Field type (e.g., `int32_t`, `float`, `uint32_t`)
- Field usage in decompiled code

**Important: Handling inheritance and struct gaps:**

The game uses C++ inheritance. A class like `cChr` inherits from base classes, 
so the offset needs to account for base class size:

```python
# Get the full structure with all fields
ghidra_structs_get(name="cChr")
# Returns: fields list with offsets, size, and any base class info

# Check for base classes
ghidra_classes_list(limit=200)
# Look for inheritance relationships (e.g., cChr inherits from uActorBase)
```

**Offset calculation with inheritance:**
```
If offset 0x143c is in cChr:
- Check if cChr has base class (e.g., uActorBase at offset 0x0)
- Base class size might be 0x100 bytes
- Field at 0x143c is actually at cChr+0x143c (absolute)
- May need to check base class struct if offset < base_size
```

**Handling gaps in struct definitions:**

Some fields may show as `field_0xNNNN` (undefined). Use multiple approaches:

```python
# Approach 1: List all defined fields to find gaps
ghidra_structs_get(name="cChr")
# Check returned fields - gaps between offsets indicate undefined regions

# Approach 2: Search for similar offsets in related classes
ghidra_structs_list(limit=200)
# Look for related classes like cChrPlayer, cChrEnemy that might have same field

# Approach 3: Decompile functions that access this offset
# Search for functions with "cChr" in name and decompile them
ghidra_functions_list(name_contains="cChr", limit=100)
ghidra_functions_decompile(name="cChr::UpdateState")
# Look for references to `this + 0x143c` in decompiled code
```

**Example with inheritance:**
```python
ghidra_structs_get(name="cChr")
# Returns:
# {
#   "name": "cChr",
#   "size": 0x6a60,
#   "fields": [
#     {"offset": 0x0, "name": "vtable", "type": "void*"},
#     {"offset": 0x8, "name": "base_uActorBase", "type": "uActorBase"},
#     {"offset": 0x100, "name": "mPlayerIndex", "type": "int32_t"},
#     ...
#     {"offset": 0x143c, "name": "mAnmSerial", "type": "int32_t"},
#     ...
#   ]
# }

# If offset 0x143c is not listed, it's in a gap
# Search base classes:
ghidra_structs_get(name="uActorBase")
# Check if field is in base class (offset < 0x100)
```

### 4.1a Trace Field Behavior in Ghidra

After identifying the divergent field, trace its usage to understand **when and 
why** it changes:

#### Step 1: Find All References (Xrefs)

Use Ghidra MCP to find all references to the field:

```python
# Get the structure with field information
ghidra_structs_get(name="uCharacter")
# This returns field offsets and types

# Find all cross-references to this offset
# Note: You'll need to calculate the absolute address as base + offset
ghidra_xrefs_list(to_addr="0x<base+0x143c>")
# Returns both read and write references
```

Alternatively, search for the field name in decompiled code:

```python
# Search all functions that reference this field
ghidra_functions_list(limit=1000)
# Then decompile functions that operate on uCharacter to find field usage
ghidra_functions_decompile(name="cChr::UpdateAnimationState")
```

Focus on **write xrefs** first, as they reveal when the field changes.

#### Step 2: Analyze Write Contexts

For each function that writes to the field, decompile and examine:

```python
ghidra_functions_decompile(name="cChr::UpdateAnimationState", max_lines=100)
```

```cpp
// Example: mAnmSerial write in character update function
void cChr::UpdateAnimationState(cChr *this) {
    if (this->mActionState == ACTION_ATTACKING) {
        this->mAnmSerial = g_sMain->mFrameTimer + this->mPlayerIndex;
        this->SpawnProjectile(this->mAnmSerial);
    }
}
```

**Extract key information:**
1. **Triggering condition**: What state must be true for this write to occur?
   - Example: `mActionState == ACTION_ATTACKING`
2. **Source of value**: Where does the new value come from?
   - Example: `g_sMain->mFrameTimer + this->mPlayerIndex`
3. **Dependent actions**: What happens immediately after this write?
   - Example: `SpawnProjectile(this->mAnmSerial)`
4. **Call graph**: What function calls this writer? Is it called every frame 
   or conditionally?

#### Step 3: Identify State Requirements

Use Ghidra's call graph and backward slicing to determine:

**Required preconditions for field to change:**
- Character must be in specific action state (e.g., `ACTION_ATTACKING`)
- Animation frame must reach trigger point (e.g., `mAnmFrame >= 5`)
- Player input must match command (e.g., `input & BTN_SPECIAL`)
- Resource must be available (e.g., `mShotCount < MAX_SHOTS`)

**Example analysis:**
```
Field: uCharacter::mAnmSerial at 0x143c
Type: int32_t
Diverges at frame 58 (host=0x130, client=0x12b)

Write xrefs:
1. cChr::PrepareThrowSequence+0x42
   - Condition: this->mActionId == 0x15 (forward throw)
   - Value: g_sMain->mFrameTimer
   - Called from: cChr::UpdateActionDispatch (every frame during throw)

2. cChr::OnShotCreationEvent+0x1a
   - Condition: AnmChr event ID == 0x200
   - Value: g_sMain->mFrameTimer
   - Called from: cChr::ProcessAnmEvents (when animation triggers shot event)

Read xrefs:
1. cChr::SpawnProjectile+0x8
   - Used to deduplicate shot spawns within 8-frame window
   - Checks: if (mAnmSerial == lastSerial) skip spawn

Analysis:
- Field is written from g_sMain->mFrameTimer, which may differ between 
  host/client during re-simulation
- Used for shot deduplication ring; different values cause same shot to 
  spawn twice or not at all
- Write happens during animation event processing, frame 58 matches Jill 
  forward throw projectile spawn timing
```

#### Step 4: Cross-Reference with Desync Evidence

Compare Ghidra findings with desync report:

```json
{
  "first_divergent_frame": 58,
  "region": "uCharacter.team0",
  "offset": "0x143c",
  "host_value": "30 01 00 00",  // 0x130 = 304
  "client_value": "2b 01 00 00"  // 0x12b = 299
}
```

**Interpretation:**
- Host `mAnmSerial` = 304, client = 299 (difference of 5 frames)
- Field is written from `g_sMain->mFrameTimer`, a monotonic frame counter
- During re-simulation, host reuses saved timer value (frame 304) while 
  client uses current timer (frame 299)
- **Root cause**: mAnmSerial is gameplay-critical for shot deduplication but 
  not restored during rollback

#### Step 5: Determine Fix Strategy

Based on usage analysis:

**Option A: Restore the field** (if it's stable game state)
- Field represents game state that should be consistent between timelines
- Example: `mActionId`, `mComboCount`, `mHitStun`
- Fix: Add to restore range in `rollback_restore.cpp`

**Option B: Normalize the source** (if field is derived from unstable state)
- Field is computed from non-deterministic source (QPC timer, frame counter)
- Example: `mAnmSerial` derived from `mFrameTimer`
- Fix: Override value during re-simulation to use net frame instead of wall-clock frame

**Decision for mAnmSerial:**
Field is used for deduplication, not actual gameplay logic. The **value 
doesn't matter as long as host and client use the same value**.

**Recommended fix:**
```cpp
// In shot spawn hook during re-simulation:
if (g_isResimulating) {
    character->mAnmSerial = g_resimulateCurrentFrame;  // Use net frame
}
```

Alternative fix:
```cpp
// In rollback_restore.cpp:
// Restore mAnmSerial to ensure consistency
memcpy(dst + 0x143c, src + 0x143c, 4);
```

Choose the simpler fix. If the field has no other xrefs besides shot creation, 
normalization is safer. If it's used in multiple systems, restore is better.

### 4.2 Fast Path: Try Obvious Fix First

Before deep behavioral analysis, assess if the field is obviously gameplay-relevant:

**Indicators of obvious gameplay fields:**
- Field name suggests state: `mActionId`, `mComboCount`, `mHitStun`, `mHealth`
- Field type is scalar: `int32_t`, `float`, `uint32_t` (not pointers)
- Field offset is far from vtable (offset > 0x100)
- Field is in character, action, or shot structures

**Fast path workflow:**

1. **Quick field identification** (30 seconds):
   ```python
   ghidra_structs_get(name="cChr")  # or try uCharacter, uActorBase
   # Find field at divergent offset
   # Check: Is it a named scalar field? → Likely safe to restore
   ```

2. **Apply restore immediately** without deep analysis:
   ```cpp
   // In rollback_restore.cpp
   // mActionId: Current action state identifier
   // Evidence: Jill Forward Throw desyncs at frame 58, offset 0x2a40
   // Type: int32_t (from Ghidra)
   {0x2a40, 0x04},
   ```

3. **Rebuild and verify** (2 minutes):
   ```bash
   make -j8
   cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
   wine umvc3.exe -autotest=Jill -autotest-moves=<command-id> -autotest-repeat=1
   ```

4. **Branch on result**:
   - **No `.usdp` or exit code 0**: Fix worked! Move to next case.
   - **Exit code 1, same field**: Fix didn't work → NOW do deep analysis
   - **Exit code 1, new field**: Original fix succeeded, iterate on new field

**When NOT to use fast path:**
- Field name is `field_0xNNNN` (undefined, needs research first)
- Field is at low offset near vtable (< 0x20, likely pointer)
- Field type is pointer or complex structure
- Field is in shot/effect structures (may need recipe normalization)
- Previous fast path attempt on this field already failed

**Time savings:**
- Fast path: ~3 minutes per field (identify + fix + verify)
- Full analysis: ~10-15 minutes per field (xrefs + decompile + trace + fix + verify)
- Use fast path for 70-80% of cases, save deep analysis for failures

### 4.3 Classification Decision Tree

#### Case 1: Field is a known captured/restored field

Check `UMVC3Hook/code/rollback_snapshot.cpp` and 
`UMVC3Hook/code/rollback_restore.cpp`:

- Search for the region name (e.g., `"uCharacter.team0"`).
- Verify whether the field's offset is included in the captured region or 
  restore range.
- If the field is **inside a captured region** but **outside a restore range**, 
  classify as:

  **Root cause: Missing field in restore path**

Example:
```cpp
// rollback_snapshot.cpp captures uCharacter bytes [0x0, 0x6a60)
// rollback_restore.cpp restores only [0x0, 0x1000) and [0x2000, 0x3000)
// Field at offset 0x143c is captured but NOT restored
// → Missing restore
```

#### Case 2: Field is a shot/effect/projectile property

If the divergent region is:
- `sShot`
- `uShot.*` (e.g., `uShotNormal`, `uShotBeam`)
- `sEffect`
- `uEffect.*`
- Shot count fields in `sShot` or character shot lists

**Immediately trace shot creation in Ghidra:**

1. **Find shot constructor**: Search for the concrete class constructor 
   (e.g., `uShotNormal::uShotNormal` or `uShotBeam::Create`).

2. **Identify creation call sites**: Use xrefs to find where the shot is spawned:
   ```cpp
   // Example shot creation xref
   uShot* cChr::SpawnProjectile(cChr *this, int shotType) {
       uShotNormal *shot = new uShotNormal();
       shot->mOwner = this;
       shot->mPosX = this->mPosX + this->mFacingDir * 50.0f;
       shot->mVelX = this->mFacingDir * 10.0f;
       shot->mRequestAnmSerial = this->mAnmSerial;  // ← Divergent field
       shot->mShotId = g_sShot->AllocateShotId();
       return shot;
   }
   ```

3. **Analyze each property assignment**:
   - **Stable properties**: Derived from saved game state (owner position, facing)
   - **Unstable properties**: Derived from non-deterministic state (frame timer, RNG)

4. **Check for deduplication logic**:
   ```cpp
   // Example deduplication check
   if (g_sShot->FindShotBySerial(shot->mRequestAnmSerial) != nullptr) {
       delete shot;
       return nullptr;  // Shot already exists, skip spawn
   }
   ```

5. **Trace backward to animation events**:
   - Shot creation is often triggered by animation keyframe events
   - Search for `AnmChr` event processing or `ProcessAnimationEvent` functions
   - Verify event timing is deterministic (frame-based, not time-based)

**Example shot desync analysis:**

Use Ghidra MCP to examine the creation site:

```python
# Find the shot creation function
ghidra_functions_decompile(name="cChr::SpawnProjectile", max_lines=50)
```

Cross-reference the divergent frame with the host/client logs:

```
Host log:
  Frame 58: cChr::ProcessAnmEvents: event_id=0x200 (spawn_projectile)
  Frame 58: cChr::SpawnProjectile: mAnmSerial=0x130 (from mFrameTimer)
  Frame 58: uShotNormal created: mRequestAnmSerial=0x130

Client log:
  Frame 58: cChr::ProcessAnmEvents: event_id=0x200 (spawn_projectile)
  Frame 58: cChr::SpawnProjectile: mAnmSerial=0x12b (from mFrameTimer)
  Frame 58: uShotNormal created: mRequestAnmSerial=0x12b
```

Check the comparator report:
```json
{
  "first_divergent_frame": 58,
  "region": "uShot.team0[0]",
  "offset": 0x211c,
  "host_value": "30 01 00 00",  // 0x130
  "client_value": "2b 01 00 00"  // 0x12b
}
```

**Ghidra analysis findings:**
- Field: `uShotNormal::mRequestAnmSerial` at offset 0x211c
- Source: Copied from `cChr::mAnmSerial` during shot constructor
- Usage: Checked by shot deduplication ring to prevent double-spawns
- Root value: `cChr::mAnmSerial` is written from `g_sMain->mFrameTimer`

**Diagnosis:**
The shot itself is created correctly (same animation event, same frame, same 
trigger), but a property (`mRequestAnmSerial`) differs because the source 
field (`cChr::mAnmSerial`) differs. The source field diverged earlier because 
it wasn't restored.

**Trace the dependency chain:**
```
uShot::mRequestAnmSerial (diverges frame 58)
  ← cChr::mAnmSerial (diverges frame 58, written frame 58)
    ← g_sMain->mFrameTimer (non-deterministic during re-simulation)
```

**Classification:**
This could be classified as either:
1. **Missing field restore**: `cChr::mAnmSerial` should be restored
2. **Incorrect shot recipe**: Shot creation should normalize `mRequestAnmSerial`

**Decision criteria:**
- If `cChr::mAnmSerial` has **multiple read xrefs** (used beyond shot creation), 
  classify as **missing field restore** and add to character restore ranges.
- If `cChr::mAnmSerial` has **only shot-related xrefs**, classify as 
  **incorrect shot recipe** and normalize during shot creation.

**Recommended fix** (shot recipe normalization):
```cpp
// In shot creation hook or constructor wrapper:
if (g_isResimulating) {
    // Override serial with deterministic frame-based value
    shot->mRequestAnmSerial = g_resimulateCurrentFrame;
}
```

Cross-reference with the host/client logs:

   - Look for `AutoTestSuite: command=X/Y matcher=<matcher-id> line=<line-id> <MoveName>` around the divergent frame.
   - Use the logged `matcher-id` and `line-id` as the runtime command identity;
     the displayed move name is diagnostic text only.
- Check for shot creation messages, AnmChr event dispatches, or projectile spawn 
  logs.
- If the shot properties (position, velocity, owner, animation serial, timing) 
  differ at the **first frame the shot exists**, classify as:

  **Root cause: Incorrect shot creation recipe**

Example:
```
Host log: Frame 58: Created shot ID 0x130 from Hadoken animation
Client log: Frame 58: Created shot ID 0x12b from Hadoken animation
Comparator: uShot.team0 offset 0x211c (mRequestAnmSerial): host=0x130, client=0x12b
→ Incorrect shot recipe (animation serial namespace collision)
```

#### Case 3: Field is propagated state

If the divergent field **changes only after an earlier divergent field**, it is 
**propagation**, not a root cause. Examples:

- Position `x/y/z` differs at frame 100, but earlier `velocity` differed at 
  frame 58.
- Animation state differs at frame 200, but earlier action ID differed at 
  frame 58.

Always trace back to the **earliest divergent frame** and classify from there.

#### Case 4: Field is visual/render state (comparator issue)

If the divergent field name or usage indicates visual-only state:

**Field name indicators:**
- Contains: "blend", "lerp", "interp", "camera", "render", "draw", "shadow", 
  "particle", "effect", "audio", "sound", "lod", "cull"
- Examples: `mBlendWeight`, `mCameraOffset`, `mLastDrawPos`, `mParticleAlpha`

**Usage indicators (check read xrefs):**
- Only called by rendering functions (names with "Draw", "Render", "Display")
- Only called by audio functions (names with "Play", "Sound", "Audio")
- Not used in hit detection, state transitions, or shot creation

**Action:**
DO NOT add restore range. This is a comparator bug.

```
Classification: visual_state_false_positive
Field: {field_name} at {offset}
Evidence: Used only in rendering code (see xrefs to Draw* functions)
Action: Report to user - comparator should exclude this region
Recommendation: Add field offset range to comparator exclusion rules
```

**Report to user and skip this field.**

#### Case 5: Field is unknown or undocumented

If Ghidra shows the field as:
- `field_0xNNNN` (no recovered name)
- Part of an unresolved embedded structure
- Beyond the documented DTI size

Classify as:

  **Root cause: Unknown field; Ghidra reverse-engineering required**

Document the byte offset, surrounding fields, and xrefs for human review.

### 4.4 Apply Tentative Fix

For **missing field restore** cases:

1. Locate the restore function in `UMVC3Hook/code/rollback_restore.cpp` (e.g., 
   `Rollback_RestoreCharacter`).
2. Identify the appropriate restore range array (e.g., `CCHR_SCALAR_RESTORE_RANGES`).
3. Add the missing field to the range array with proper documentation.

**Restore range guidelines:**
- Prefer adding to an existing range if the new field is adjacent (within 16 
  bytes of existing range boundaries).
- Create a new range for isolated fields or fields separated by known-excluded 
  regions (pointers, render-only data).
- Always document: field name, evidence (character/move/frame), host/client 
  values, and gameplay impact.
- Sort ranges by offset in ascending order for maintainability.
- Use `constexpr` arrays for compile-time validation.

**Safety checks before adding a field:**
1. Verify the field is **not a pointer** (check Ghidra type).
2. Verify the field is **not a resource handle** (texture ID, sound bank handle).
3. Verify the field is **gameplay-relevant** (affects hit detection, state 
   transitions, or shot creation).
4. Check for **alignment**: If field offset is misaligned (e.g., 3-byte offset 
   for 4-byte int), investigate whether it's part of a packed struct or bitfield.

**Distinguishing gameplay state from visual state:**

The comparator already filters out most visual-only state, but some may slip through:

**Visual state (DO NOT restore):**
- Animation blend buffers and interpolation weights
- Camera position, FOV, shake parameters
- Particle effect positions, colors, lifetimes
- UI element positions, transparency
- Shadow maps, reflection buffers
- Audio playback positions, volume levels
- Model LOD selection, culling flags

**Gameplay state (MUST restore):**
- Hitbox and hurtbox positions, sizes, active flags
- Character position, velocity, facing direction
- Action ID, state flags, frame counters
- Health, meter, combo count
- Input history buffers
- Shot/projectile positions, velocities, ownership
- Frame advantage, hitstun, blockstun timers
- RNG seed state (if used for gameplay decisions)

**How to distinguish:**

1. **Check field name**: Words like "blend", "lerp", "camera", "render", "draw" 
   indicate visual state
2. **Trace read xrefs**: If only used by rendering/audio code → visual. If used 
   by collision/logic code → gameplay
3. **Test impact**: Does divergence in this field cause gameplay differences 
   (hits land differently) or only visual differences (animation looks different)?

**If comparator reports visual field divergence:**

This is a comparator bug. The field should be added to exclusion rules, not restored:

```python
# DO NOT add restore range
# Instead, report to user:
f"Field {field_name} at {offset} appears to be visual state (used in rendering)"
f"Comparator should exclude this region. Please update comparison rules."
```

**Common false positives:**
- `mBlendWeight` - animation blending (visual)
- `mCameraTarget` - camera tracking (visual)
- `mModelAlpha` - transparency (visual)
- `mShadowMatrix` - shadow rendering (visual)
- `mLastDrawPos` - previous render position (visual)

If uncertain, check with user before restoring.

#### Example 1: Scalar Field (Single Value)

For a single 4-byte int32_t field like `mAnmSerial` at offset `0x143c`:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // ... existing ranges ...
    
    // mAnmSerial: Animation serial number for shot deduplication ring.
    // Evidence: Jill Forward Throw desyncs at frame 58 without this field.
    // Host=0x130, client=0x12b; field affects shot event namespace collision.
    {0x143c, 0x04},
    
    // ... remaining ranges ...
};
```

#### Example 2: Contiguous Scalar Range

For multiple adjacent fields like `mActionId` (4 bytes) and `mActionState` (4 
bytes) at offset `0x2a40`:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // ... existing ranges ...
    
    // mActionId and mActionState: Current action identifier and state flags.
    // Evidence: Jill Forward Throw desyncs at frame 62 after mAnmSerial fix.
    // Host mActionId=0x05, client=0x03; controls action dispatch logic.
    {0x2a40, 0x08},
    
    // ... remaining ranges ...
};
```

#### Example 3: Array or Embedded Class

For an array of structures like a 16-entry input history buffer at offset 
`0x2100`, where each entry is 0x10 bytes:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // ... existing ranges ...
    
    // Input history ring buffer: 16 entries of cChrInput (0x10 bytes each).
    // Evidence: Ryu Hadoken desyncs at frame 45; host/client have different
    // cached input sequences affecting command matcher charge-time calculation.
    {0x2100, 0x100},  // 16 * 0x10 = 0x100 bytes
    
    // ... remaining ranges ...
};
```

#### Example 4: Embedded Class Members

For a class embedded in the character structure, like `cChrAbility` at offset 
`0x1800` with known scalar fields but unknown total size, restore verified 
scalar ranges only:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // ... existing ranges ...
    
    // cChrAbility::mGaugeLevel and mAbilityFlags: Gauge state and ability bits.
    // Evidence: Phoenix Wright desyncs at frame 72; client has different gauge
    // level affecting Investigation mode availability. Restore only confirmed
    // scalar payload; skip embedded resource pointers at +0x1820..+0x1827.
    {0x1800, 0x10},  // Gauge scalars
    {0x1828, 0x08},  // Ability flags after pointer gap
    
    // ... remaining ranges ...
};
```

#### Example 5: Non-Scalar Special Case (Manual Copy)

For fields requiring special handling (e.g., pointer remapping, conditional 
restore), add manual logic in the restore function body:

```cpp
// Inside Rollback_RestoreCharacter function, after range-based restore:

// cChrTargetInfo array: 3 entries at +0x64A0, +0x6520, +0x65A0 (0x70 each).
// Evidence: Wesker Command Throw desyncs at frame 80; target lock state differs.
// Each entry has vtable/owner pointers that must remain live; restore only the
// scalar payload at +0x08..+0x6F within each entry.
for (int i = 0; i < 3; i++) {
    const uint32_t baseOffset = 0x64A0 + (i * 0x80);
    std::memcpy((uint8_t*)dst + baseOffset + 0x08,
                (uint8_t*)src + baseOffset + 0x08,
                0x68);  // Skip vtable at +0x00, restore payload +0x08..+0x6F
}
```

For **incorrect shot recipe** cases:

1. Locate the shot creation or normalization code in 
   `UMVC3Hook/code/rollback_shot.cpp` or the relevant hook.
2. Identify where the divergent property is populated (e.g., animation serial, 
   timing, owner ID).
3. Add normalization logic to ensure host and client use the same value during 
   re-simulation.

#### Example: Shot Recipe Normalization

```cpp
// In Shot_SpawnOriginal or shot factory wrapper:

// Normalize shot animation serial to net frame instead of QPC-derived stamp.
// Evidence: Jill Forward Throw desyncs at frame 58; host/client use different
// sMain+0x48 mFrameTimer values (host=0x130, client=0x12b), causing shot
// deduplication ring collisions when the same shot re-spawns within 8 frames.
if (g_isResimulating) {
    shot->mRequestAnmSerial = g_resimulateCurrentFrame;
}
```

### 4.5 Rebuild and Verify

After applying the tentative fix:

1. Rebuild the plugin:
   ```bash
   make -j8
   ```

2. Copy the new `UMVC3Hook.asi` to the game directory.

3. Re-run the exact same test case:
   ```bash
   wine umvc3.exe -autotest=<character> -autotest-moves=<command-id> -autotest-repeat=1
   ```

4. Check for `.usdp` artifacts:
   - **If no `.usdp` files are created**: The fix resolved the desync. Classify 
     as **fixed**.
   - **If `.usdp` files exist**: Run `compare_desync_dumps.py`.
     - Exit code `0`: Fix successful, desync resolved.
     - Exit code `1`: Desync persists. Check if it's the same field or a new 
       divergence.

5. If the same field still desyncs:
   - The fix was incomplete or incorrect. Re-investigate the restore range or 
     normalization logic.

6. If a **new** field desyncs:
   - The original field is fixed. Log the successful fix and repeat the 
     investigation process for the new divergent field.

### 4.6 Iterative Fix Cycle

Continue the fix-verify loop:

```
Detect desync → Investigate in Ghidra → Classify root cause → Apply fix → 
Rebuild → Test → [Fixed: done] or [New field: repeat]
```

Each iteration resolves one divergent field. Stop when:
- No `.usdp` artifacts are created, OR
- `compare_desync_dumps.py` exits `0` for the test case.

Document each fix iteration:
```json
{
  "iteration": 1,
  "divergent_field": "uCharacter::mAnmSerial at 0x143c",
  "classification": "missing_field_restore",
  "fix_applied": "Added memcpy for mAnmSerial in Rollback_RestoreCharacter",
  "verification_result": "new_divergence",
  "next_divergent_field": "uCharacter::mActionId at 0x2a40"
}
```

### 4.7 Concurrent Divergences

If multiple fields diverge at the **same frame**, prioritize investigation by:

1. **Region hierarchy**: Core game state (uCharacter, sAction) before derived 
   state (shots, effects, UI).
2. **Offset order**: Lower offsets typically represent earlier-initialized 
   fields with broader impact.
3. **Type significance**: Control flow fields (action ID, state flags) before 
   animation or cosmetic fields.

Apply fixes for all concurrent divergences in a single iteration, then verify. 
If verification still shows divergence, check whether:
- One of the fixes was incomplete.
- The divergent fields have dependencies (e.g., field A writes to field B).

### 4.8 Pointer and Heap Exclusions

The comparator normalizes certain field types before comparison:

- **Pointers to game resources** (textures, models, sound banks): Excluded 
  because heap addresses differ between instances.
- **Vtable pointers**: Normalized to base-relative offsets.
- **Dynamic allocations**: Arrays and linked lists are compared structurally 
  (count, element values) but not by heap address.

**Important:** If a divergence appears in a region marked as "excluded by 
normalization rules," verify in Ghidra whether the field is:
- A pointer (8 bytes on x64, 4 bytes on x86) → Correctly excluded.
- A scalar value (int, float) mistakenly adjacent to excluded region → Missing 
  restore.

Never modify exclusion rules to pass a test. If a scalar field is wrongly 
excluded, fix the comparator's region definitions.

### 4.9 Dynamic Type Resolution

For regions like `uShot.*` or `uEffect.*` where concrete type varies by 
instance:

1. Check the snapdump JSON for the recorded vtable address.
2. Use Ghidra MCP to look up the vtable address:
   ```python
   ghidra_memory_read(address=vtable_addr, length=8)
   # Cross-reference with known vtables to identify concrete class
   ```
3. Search Ghidra for the concrete class (e.g., `uShotBeam`, `uShotNormal`).
4. Navigate to the divergent offset within the concrete class structure.

If the vtable is unknown:
- Search for xrefs to the vtable address in Ghidra to find constructor calls.
- Identify the class from constructor symbols or nearby string literals.

### 4.9a Using Ghidra Call Graph for State Analysis

When investigating a divergent field, use Ghidra's call graph to understand 
the execution flow:

#### Forward Call Graph (What does this field affect?)

1. Find read xrefs to the divergent field
2. For each reading function, generate a forward call graph:
   ```python
   ghidra_analysis_get_callgraph(name="cChr::CheckActionTransition", max_depth=5)
   ```
3. Identify gameplay-critical paths:
   - Hit detection functions
   - State transition logic
   - Shot spawn triggers
   - Combo system updates

**Example:**
```
mActionId read by:
  cChr::CheckActionTransition
    → cChr::EnterNewAction
      → cChr::ResetComboState
      → cChr::PlayActionSound
    → cChr::UpdateHitboxes
      → cColl::RefreshCollisionBoxes
        → cColl::CheckHitAgainstOpponent ← Gameplay-critical
```

**Analysis:** `mActionId` affects hit detection through a 3-level call chain. 
Must be restored accurately.

#### Backward Call Graph (What controls this field?)

1. Find write xrefs to the divergent field
2. For each writing function, use Ghidra MCP to trace callers:
   ```python
   # Find what calls the writer function
   writer_addr = "0x405820"  # Address from previous decompilation
   ghidra_xrefs_list(to_addr=writer_addr)
   # Returns list of functions that call this writer
   ```
3. Recursively trace back to input handlers or frame update roots:
   ```python
   # Decompile each caller to understand call context
   ghidra_functions_decompile(address="0x406100")
   ```
4. Build the call chain from frame root to the writer

**Example:**
```
mActionId written by:
  cChr::SetAction (addr: 0x405820)
    ← called by cChr::ProcessInputCommands (addr: 0x406100)
      ← called by cChr::Update (addr: 0x407000)
        ← called by cMain::UpdateCharacters (addr: 0x401500)
          ← called by cMain::Update (addr: 0x401000) ← Frame root
```

**Analysis:** `mActionId` is written during input processing every frame. 
Changes are driven by player input, so determinism depends on input consistency.

#### State Dependency Analysis

For complex fields, trace the **required state** for the field to change:

1. **Locate the write site** in decompiled code:
   ```cpp
   void cChr::ProcessInputCommands(cChr *this) {
       if (this->mActionState == AS_IDLE && 
           this->mInputBuffer.MatchCommand(CMD_SPECIAL)) {
           this->mActionId = 0x15;  // Forward throw
           this->mActionState = AS_ATTACKING;
       }
   }
   ```

2. **Extract all conditions**:
   - `mActionState == AS_IDLE` (field at offset 0x2a44)
   - `mInputBuffer.MatchCommand(CMD_SPECIAL)` (depends on input history)

3. **Build state requirement tree**:
   ```
   For mActionId to change to 0x15:
     REQUIRE: mActionState == AS_IDLE
       REQUIRE: Previous action completed
       REQUIRE: mActionTimer == 0
     REQUIRE: Input matches CMD_SPECIAL
       REQUIRE: Input sequence: down, forward, special (within 16 frames)
       REQUIRE: mInputBuffer[0..15] contains valid command
   ```

4. **Check if prerequisites are restored**:
   - Is `mActionState` in restore ranges? → Check `rollback_restore.cpp`
   - Is `mInputBuffer` in restore ranges? → Check if input history is captured
   - Is `mActionTimer` in restore ranges? → Verify timer fields

5. **Identify missing dependencies**:
   If `mActionId` desyncs but is correctly restored, check if its prerequisites 
   are missing from restore ranges. The desync may be **propagating** from an 
   earlier field.

**Example propagation chain:**
```
Frame 50: mInputBuffer[5] not restored → host/client have different inputs
Frame 55: mInputBuffer mismatch causes different command match result
Frame 58: mActionId differs because command matcher had different input
Frame 62: mPosX differs because different action has different movement
```

**Resolution:** The root cause is `mInputBuffer` at frame 50, not `mPosX` at 
frame 62. Always fix the **earliest** divergent field.

### 4.10 Generate Diagnosis Report

Output a structured JSON report:

```json
{
  "desync_id": "jill-forward-throw-r003-f58",
  "character": "Jill",
  "move": "Forward Throw",
  "repeat_index": 3,
  "first_divergent_frame": 58,
  "region": "uCharacter.team0",
  "offset": 5180,
  "offset_hex": "0x143c",
  "ghidra_field": "uActorGameModel::mAnmSerial",
  "ghidra_type": "int32_t",
  "host_value": 304,
  "client_value": 299,
  "classification": "missing_field_restore",
  "evidence": {
    "captured": true,
    "restored": false,
    "restore_ranges": ["0x0-0x1000", "0x2000-0x3000"],
    "field_usage": "Animation serial for shot deduplication ring",
    "gameplay_relevant": true
  },
  "recommended_fix": "Extend uCharacter restore range to include mAnmSerial at offset 0x143c"
}
```

For shot-recipe cases:

```json
{
  "classification": "incorrect_shot_recipe",
  "evidence": {
    "shot_type": "uShotNormal",
    "divergent_property": "mRequestAnmSerial",
    "host_namespace": 304,
    "client_namespace": 299,
    "shot_creation_frame": 58,
    "likely_cause": "Host/client use different QPC-derived frame stamps; shot deduplication ring sees collision"
  },
  "recommended_fix": "Normalize sMain+0x48 mFrameTimer to net frame before shot creation"
}
```

## 5. AI Controller Workflow

### 5.1 Single-Pass Discovery and Fix Loop

For each character and move:

1. **Launch**: Run with `--repeat=1`.
2. **Validate**: Check suite completion.
3. **Discover**: Look for `.usdp` pairs created after launch.
4. **Compare**: Run `compare_desync_dumps.py`.
5. **Branch on result**:
   - Exit code `0` or no `.usdp`: **Mark as stable_sync, move to next case.**
   - Exit code `1`: **Proceed to investigation.**

### 5.2 Investigation and Fix Loop

When a desync is detected (exit code `1`):

1. **Parse** comparator JSON: Extract `region`, `offset`, `host_value`, `client_value`.

2. **Quick field lookup**: Use Ghidra MCP to identify the field:
   ```python
   ghidra_structs_get(name="cChr")  # or alternate names
   ```

3. **Assess complexity**:
   - **Simple case**: Named scalar field (int, float), far from vtable, in character struct
     → **Skip to step 4 (fast path)**
   - **Complex case**: Undefined field, pointer type, shot/effect region, or low offset
     → **Proceed with full analysis (step 3a)**

4. **Apply fix** (fast path):
   - For named scalar field: Add restore range immediately
   - Add comment with field name, offset, character/move evidence
   ```cpp
   // mActionId: Action state identifier
   // Evidence: Jill Forward Throw desyncs at frame 58, offset 0x2a40
   {0x2a40, 0x04},
   ```

5. **Rebuild**: `make -j8`.

6. **Verify**: Re-run the same test case with `--repeat=1`.

7. **Check result**:
   - No `.usdp` or exit code `0`: **Fix successful. Log and move to next case.**
   - Exit code `1` with **same field**: Fast path failed → **Go to step 3a (full analysis)**
   - Exit code `1` with **new field**: Original fix successful, repeat loop for new field.

**Step 3a: Full Behavioral Analysis** (only if fast path failed or complex case):

1. **Find write xrefs**: 
   ```python
   ghidra_functions_list(name_contains="cChr", limit=200)
   ghidra_functions_decompile(name="cChr::UpdateState")
   # Search for "this + 0x<offset>" in decompiled code
   ```

2. **Analyze write contexts**: Extract conditions, value sources, state requirements.

3. **Find read xrefs**: Understand gameplay impact.

4. **Build call graphs**: Trace dependencies with `ghidra_analysis_get_callgraph()`.

5. **Classify root cause**:
   - Check if field is captured but not restored → `missing_field_restore`
   - Check if field is in shot/effect region → `incorrect_shot_recipe`
   - Check if field source is unstable → May need normalization instead of restore
   - Otherwise → `unknown`

6. **Apply targeted fix**:
   - For missing field with stable source: Add restore range
   - For field with unstable source (timer, RNG): Normalize source during re-sim
   - For shot recipe: Add normalization in shot creation code
   - Add detailed comment with xrefs, usage analysis, and fix rationale

7. **Rebuild and verify again**.

**Key principle**: Try the simple fix first (90 seconds), only invest in deep analysis (10+ minutes) when necessary.

### 5.3 Iteration Tracking

Maintain a fix log per test case:

```json
{
  "character": "Jill",
  "move": "Forward Throw",
  "iterations": [
    {
      "iteration": 1,
      "divergent_frame": 58,
      "region": "uCharacter.team0",
      "offset": "0x143c",
      "field": "mAnmSerial",
      "classification": "missing_field_restore",
      "fix": "Added memcpy for mAnmSerial in Rollback_RestoreCharacter",
      "verification": "new_divergence"
    },
    {
      "iteration": 2,
      "divergent_frame": 62,
      "region": "uCharacter.team0",
      "offset": "0x2a40",
      "field": "mActionId",
      "classification": "missing_field_restore",
      "fix": "Extended restore range to include mActionId",
      "verification": "fixed"
    }
  ],
  "final_status": "fixed",
  "total_iterations": 2
}
```

### 5.4 Deduplication

Group findings by:
- Character
- Divergent field (region + offset)

If multiple moves on the same character produce the same divergent field, they 
share a root cause. Apply the fix once and verify against all affected moves.

**Deduplication workflow:**
1. Maintain a global registry of `(character, region, offset)` tuples.
2. Before applying a fix, check if the tuple already exists in the registry.
3. If yes, mark the current test case as "shares root cause with [other case]" 
   and skip to next case.
4. After applying a new fix, re-test all cases that share the same tuple to 
   verify the fix resolves all instances.

### 5.5 Stopping Criteria

For a given test case, stop when:
- `compare_desync_dumps.py` exits `0`, OR
- No `.usdp` artifacts are created after launch.

Both outcomes indicate the test case is now deterministic.

**Important edge cases:**
- If `.usdp` files are created but comparator exits `2` (I/O error): Retry once. 
  If error persists, classify as `analysis_failed`.
- If suite log shows "AutoTestSuite: command-line suite complete" but `.usdp` 
  files exist with matching timestamps: Run comparator. Exit code `0` means 
  late-desync detection (beyond rollback window) but re-simulation converged.
- If no completion message in log: Classify as `incomplete` even if no `.usdp` 
  files exist.

## 6. Required Artifacts

For every confirmed desync iteration, preserve:

- Host and client `.usdp` snapdumps.
- Comparator JSON report with field annotations.
- Host and client logs covering 120 frames before divergence through 
  termination.
- Ghidra field lookup results (structure name, field name, type, offset, usage 
  xrefs).
- The fix diff showing the added restore code or shot normalization.
- Verification result (fixed, new_divergence, or incomplete_fix).

For the final successful fix, preserve:
- Complete iteration log showing all intermediate fixes.
- Final `.asi` binary that passes the test.
- Summary: "Jill Forward Throw: 2 iterations, fixed mAnmSerial and mActionId."

## 7. Automation Acceptance Criteria

The AI controller is ready for unattended use when it can:

1. Launch a focused run with `--repeat=1` and validate completion.
2. Discover matching `.usdp` pairs and run `compare_desync_dumps.py`.
3. Parse the comparator exit code and JSON report.
4. Use Ghidra MCP to look up the divergent field and retrieve its name, type, 
   and usage.
5. Check snapshot/restore code coverage for the field.
6. Classify the desync as `missing_field_restore` or `incorrect_shot_recipe` 
   with ≥80% accuracy for known cases.
7. Apply a tentative fix to the restore or shot normalization code.
8. Rebuild the plugin with `make -j8`.
9. Re-run the test case and verify the fix.
10. Iterate until the desync is resolved or a new field diverges.
11. Deduplicate findings across moves and characters.
12. Produce a concise summary: "Jill Forward Throw: fixed after 2 iterations 
    (mAnmSerial, mActionId)."

## 8. Key Constraints

- **Single-run detection**: One execution per test case is sufficient to detect 
  deterministic desyncs. No repetition or confirmation campaigns needed.
- **Fix-verify loop**: After each fix, immediately rebuild and re-test. Continue 
  until the desync is resolved.
- **No fault injection**: The autotest suite already forces client re-simulation 
  every frame.
- **Snapdump comparison is mandatory**: `compare_desync_dumps.py` exit code is 
  the authoritative verdict.
- **Ghidra lookup is mandatory**: Every confirmed desync must be traced to a 
  field name and classified.
- **First divergent frame wins**: Always diagnose from the earliest divergence; 
  later diffs are propagation.
- **Two root causes only**: Missing field restore or incorrect shot recipe. 
  Unknown cases require human Ghidra reverse-engineering before classification.
- **No exclusion modification**: Never modify the comparator's pointer/heap 
  normalization rules to make a run pass.
- **Iterative fixing**: Each iteration resolves one divergent field. A new 
  divergence after a fix means the previous fix succeeded.
- **Visual state filtering**: Comparator already excludes most visual-only fields. 
  If visual field appears in comparison, report as comparator issue, don't restore.
- **User approval for invasive changes**: Always request user confirmation before:
  - Adding new gameplay hooks that intercept game functions
  - Modifying game behavior outside re-simulation paths
  - Changing shot creation, hit detection, or state transition logic
  - Any fix that requires new functionality beyond simple restore ranges

## 9. Troubleshooting Common Issues

### 9.1 Fix Applied But Same Field Still Desyncs

**Symptoms:** After adding restore range for field at offset `0x143c`, 
comparator still reports divergence at `0x143c` with different values.

**This means the fast path failed. Time for deep analysis.**

**Step 1: Verify the restore was applied correctly**
- **Restore range typo**: Check offset in code matches Ghidra offset exactly
- **Range not executed**: Add logging to confirm restore function is called:
  ```cpp
  printf("Restoring 0x143c, src=%d, dst=%d\n", 
         *(int32_t*)(src + 0x143c), *(int32_t*)(dst + 0x143c));
  ```
- **Restore happens but gets overwritten**: Field is restored correctly but game 
  code writes to it after restore completes

**Step 2: If restore is executed but field still diverges**

The field requires **normalization**, not restoration. Now do full behavioral analysis:

**Possible causes:**
1. **Field is written after restore**: Game code overwrites field during re-simulation
   ```python
   # Find all write xrefs
   ghidra_functions_list(name_contains="cChr", limit=200)
   # Decompile functions that write to this offset
   ghidra_functions_decompile(name="cChr::UpdateState")
   # Check if writes happen during frame update (after restore)
   ```

2. **Field source is non-deterministic**: Value comes from unstable source
   ```python
   # Analyze write context
   # If field = g_sMain->mFrameTimer → non-deterministic
   # If field = QPC() → non-deterministic  
   # If field = rand() → non-deterministic
   # Need to normalize source instead of restoring field
   ```

3. **Field is computed property**: Value is calculated from other fields
   ```python
   # Example: mVelocitySquared = mVelX * mVelX + mVelY * mVelY
   # Don't restore mVelocitySquared, ensure mVelX/mVelY are restored
   # Or mark mVelocitySquared as derived and recalculate after restore
   ```

4. **Field has property setter**: Direct memcpy bypasses setter logic
   ```python
   # Search for setter method
   ghidra_functions_list(name_contains="Set", limit=200)
   # If setter exists (e.g., SetActionId), may need to call it instead of memcpy
   ```

**Step 3: Apply targeted fix based on analysis**

- **If written after restore**: Move restore to later in restore sequence, or hook 
  the writer to skip during re-sim
- **If non-deterministic source**: Normalize at source:
  ```cpp
  if (g_isResimulating) {
      this->mAnmSerial = g_resimulateCurrentFrame;  // Use net frame
  }
  ```
- **If computed property**: Restore dependencies instead, or add recalculation hook
- **If has setter**: Use setter API instead of direct memcpy

**Debug approach:**
- Add logging before and after the restore memcpy to verify execution
- Log the source and destination values to confirm correct data
- Add logging to game code write sites to see if field changes after restore
- Compare host/client logs frame-by-frame around divergent frame

### 9.2 New Divergence Appears Earlier Than Original

**Symptoms:** After fixing field at frame 58, verification shows divergence at 
frame 45 in a different field.

**Interpretation:** This is **not** a new divergence. The original fix revealed 
a **pre-existing earlier divergence** that was masked by the later one.

**Action:** Treat the earlier divergence as the new root cause and iterate. The 
frame 58 fix was correct but insufficient.

### 9.3 Comparator Exits 0 But Gameplay Visibly Desyncs

**Symptoms:** No `.usdp` artifacts, comparator says "matched," but recorded 
replays show different outcomes.

**Possible causes:**
1. **Render-only divergence**: Visual state (camera, UI, particle effects) 
   differs but core gameplay state matches.
2. **Divergence outside snapshot window**: Game state diverges at frame 500, but 
   test ends at frame 480.
3. **Input recording mismatch**: Replay inputs differ from live inputs due to 
   frame timing issues.

**Debug approach:**
- Extend test duration with longer move sequences.
- Check if divergence happens during post-hit animation (beyond typical 
   rollback window).
- Compare input logs between host and client to verify input agreement.

### 9.4 Ghidra Field Lookup Returns Undefined

**Symptoms:** Offset `0x143c` in `cChr` shows as `field_0x143c` with no 
type information, or is not listed in the fields array.

**Action:**

1. **Check if offset is in base class:**
   ```python
   # Get the main structure
   ghidra_structs_get(name="cChr")
   # Note the size of any base class fields
   
   # If offset < base_size, check base class
   ghidra_structs_get(name="uActorBase")
   # Look for field at the offset within base
   ```

2. **Search related classes for similar fields:**
   ```python
   # List all structs to find related classes
   ghidra_structs_list(limit=500)
   # Check variants like cChrPlayer, cChrEnemy, uCharacter
   
   ghidra_structs_get(name="uCharacter")
   # uCharacter might be a different view of the same data
   ```

3. **Search for xrefs to the offset in decompiled code:**
   ```python
   # Find functions that operate on cChr
   ghidra_functions_list(name_contains="cChr", limit=100)
   
   # Decompile and search for "this + 0x143c" or "param_1 + 0x143c"
   ghidra_functions_decompile(name="cChr::UpdateAnimationState", max_lines=200)
   ```

4. **Infer type from usage context:**
   - Compared with 0 → likely int or enum
   - Used in float arithmetic → likely float
   - Used as array index → likely int
   - Assigned from pointer → likely pointer (do not restore)
   - Passed to sprintf/format → likely primitive type

5. **Check for gaps in field definitions:**
   ```python
   result = ghidra_structs_get(name="cChr")
   fields = result["fields"]
   
   # Find gaps between consecutive fields
   for i in range(len(fields) - 1):
       current_end = fields[i]["offset"] + get_type_size(fields[i]["type"])
       next_start = fields[i+1]["offset"]
       if next_start - current_end > 8:
           print(f"Gap: 0x{current_end:x} to 0x{next_start:x}")
   
   # Your divergent offset might be in a gap
   ```

6. **Cross-reference with similar structures:**
   If `cChr` has gaps, check if related classes have the same structure:
   ```python
   # Check inheritance hierarchy
   ghidra_classes_list(limit=200)
   # Look for classes that inherit from same base
   
   # Compare field layouts
   cchr_fields = ghidra_structs_get(name="cChr")["fields"]
   uchar_fields = ghidra_structs_get(name="uCharacter")["fields"]
   # Fields at same offset in both structs likely have same meaning
   ```

7. **Document findings and classify as `unknown`:**
   If field name cannot be determined after these steps, create a temporary 
   name based on usage:
   ```
   field_0x143c_int32_used_in_shot_spawn
   ```
   Mark for human review but proceed with fix if usage is clear.

### 9.5 Build Succeeds But Game Crashes on Launch

**Symptoms:** After applying fix, `make -j8` succeeds, but Wine crashes or game 
shows exception dialog.

**Possible causes:**
1. **Restore range overlaps pointer**: Check if added range includes offset 
   `+0x00` or `+0x08` in a class (likely vtable).
2. **Restore range exceeds allocation**: Verify field offset + size does not 
   exceed structure size from Ghidra DTI.
3. **Syntax error in range array**: Missing comma, wrong bracket, or `constexpr` 
   violation.

**Debug approach:**
- Revert the last change and verify game launches.
- Add only half the restore range to bisect the problematic bytes.
- Check compiler warnings for array initialization issues.

## 10. Example End-to-End Flow with Iterative Fixing

### Iteration 1: Initial Discovery

```bash
$ python3 tools/autotest_ai.py --character Jill --moves <command-id> \
    --repeat 1 --launch --json --manifest run1.json
```

**Result:** Exit code `1`, suite completed, `.usdp` pair found.

### Iteration 1: Comparison

```bash
$ python3 tools/compare_desync_dumps.py \
    desync_20260731_120000_host_f58.usdp \
    desync_20260731_120000_client_f58.usdp \
    --output report1.json
```

**Exit code:** `1` (diverged)

**JSON excerpt:**
```json
{
  "verdict": "diverged",
  "first_divergent_frame": 58,
  "first_divergent_region": "uCharacter.team0",
  "first_divergent_offset": 5180,
  "host_value": "30 01 00 00",
  "client_value": "2b 01 00 00"
}
```

### Iteration 1: Ghidra Lookup

```python
ghidra_datatypes_search(name="uCharacter")
# → Found: uCharacter (size 0x6a60)

ghidra_structs_get(name="uCharacter")
# → Field at offset 0x143c: name="mAnmSerial", type="int32_t"
# → Usage: Animation serial for shot deduplication
```

### Iteration 1: Classification

- **Captured:** Yes (part of uCharacter region)
- **Restored:** No (not in any restore range)
- **Classification:** `missing_field_restore`

### Iteration 1: Apply Fix

Edit `UMVC3Hook/code/rollback_restore.cpp`:

```cpp
// Restore uCharacter mAnmSerial at +0x143c (4 bytes)
// Evidence: Jill Forward Throw desyncs at frame 58 without this field
// Field affects shot deduplication ring
memcpy(dst + 0x143c, src + 0x143c, 4);
```

### Iteration 1: Rebuild and Verify

```bash
$ make -j8
$ cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
$ python3 tools/autotest_ai.py --character Jill --moves <command-id> \
    --repeat 1 --launch --json --manifest verify1.json
```

**Result:** Exit code `1`, new `.usdp` pair found.

### Iteration 2: Comparison

```bash
$ python3 tools/compare_desync_dumps.py \
    desync_20260731_120500_host_f62.usdp \
    desync_20260731_120500_client_f62.usdp \
    --output report2.json
```

**Exit code:** `1` (diverged, but at a **different frame and field**)

**JSON excerpt:**
```json
{
  "verdict": "diverged",
  "first_divergent_frame": 62,
  "first_divergent_region": "uCharacter.team0",
  "first_divergent_offset": 10816,
  "host_value": "05 00 00 00",
  "client_value": "03 00 00 00"
}
```

**Analysis:** Original field `mAnmSerial` at `0x143c` is now fixed. New field 
diverges at `0x2a40`.

### Iteration 2: Ghidra Lookup

```python
ghidra_structs_get(name="uCharacter")
# → Field at offset 0x2a40: name="mActionId", type="int32_t"
# → Usage: Current action identifier
```

### Iteration 2: Classification

- **Captured:** Yes
- **Restored:** No
- **Classification:** `missing_field_restore`

### Iteration 2: Apply Fix

Edit `UMVC3Hook/code/rollback_restore.cpp`:

```cpp
// Restore uCharacter mActionId at +0x2a40 (4 bytes)
// Evidence: Jill Forward Throw desyncs at frame 62 after mAnmSerial fix
// Field tracks current action state
memcpy(dst + 0x2a40, src + 0x2a40, 4);
```

### Iteration 2: Rebuild and Verify

```bash
$ make -j8
$ cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
$ python3 tools/autotest_ai.py --character Jill --moves <command-id> \
    --repeat 1 --launch --json --manifest verify2.json
```

**Result:** Exit code `0`, no `.usdp` artifacts created.

**Verdict:** Desync resolved after 2 iterations.

### Final Summary

```json
{
  "character": "Jill",
  "move": "Forward Throw",
  "final_status": "fixed",
  "total_iterations": 2,
  "fixes_applied": [
    {
      "iteration": 1,
      "field": "uCharacter::mAnmSerial at 0x143c",
      "classification": "missing_field_restore",
      "fix": "Added memcpy for mAnmSerial"
    },
    {
      "iteration": 2,
      "field": "uCharacter::mActionId at 0x2a40",
      "classification": "missing_field_restore",
      "fix": "Added memcpy for mActionId"
    }
  ]
}
```

## 11. Implementation Priorities

1. Integrate Ghidra MCP field lookup into the test workflow.
2. Parse comparator JSON and extract `region`, `offset`, `host_value`, 
   `client_value`.
3. Automate Ghidra structure navigation and field name retrieval.
4. Cross-reference field offset with `rollback_snapshot.cpp` and 
   `rollback_restore.cpp`.
5. Implement classification logic for missing-restore vs incorrect-shot.
6. **Generate code patches** that add the missing field restore or shot 
   normalization.
7. **Automate rebuild**: Run `make -j8` after applying each fix.
8. **Automate verification**: Re-run the same test case after rebuild.
9. **Implement iteration tracking**: Log each fix attempt and verification result.
10. Add deduplication by (character, region, offset).
11. Produce concise summary reports: "Jill Forward Throw fixed after 2 
    iterations."

## 12. Best Practices for AI Controllers

### 12.0 Efficiency Through Pragmatism

**The 80/20 rule**: 80% of desyncs are simple missing scalar fields that can be 
fixed in under 2 minutes. Reserve deep analysis for the 20% that fail the fast path.

**Fast path decision criteria:**
```python
def should_use_fast_path(field_info):
    # Use fast path if:
    return (
        field_info.get("name") and not field_info["name"].startswith("field_0x")  # Named
        and field_info.get("type") in ["int32_t", "uint32_t", "float", "int16_t", "uint16_t", "int8_t", "uint8_t"]  # Scalar
        and field_info.get("offset") > 0x20  # Not near vtable
        and "ptr" not in field_info.get("type", "").lower()  # Not pointer
        and region not in ["uShot", "uEffect", "sShot", "sEffect"]  # Not shot recipe
    )

# If any condition false → full analysis required
```

**Time budget allocation:**
- Field identification: 30 seconds (Ghidra struct lookup)
- Fast path fix: 30 seconds (add restore range with comment)
- Build: 30-60 seconds (`make -j8`)
- Verification: 60-90 seconds (launch test, wait for completion)
- **Total fast path: ~3 minutes per field**

Only if fast path fails:
- Full behavioral analysis: 10-15 minutes (xrefs, decompile, trace, analyze)
- Targeted fix: 2-5 minutes (normalization, complex restore)
- Re-verification: 2-3 minutes
- **Total with analysis: ~15-20 minutes per complex field**

**Workflow optimization:**
```
1. Batch identify all simple fields from multiple desyncs (5 min for 10 fields)
2. Apply all simple fixes in one edit session (5 min)
3. Single rebuild (1 min)
4. Verify all cases in parallel (if testing framework supports)
5. Only analyze failures individually
```

### 12.1 Efficiency

- **Batch Ghidra lookups**: If multiple desyncs are detected in the same 
  structure, retrieve the entire struct definition once rather than querying 
  individual fields:
  ```python
  # Good: Single call for entire structure
  ghidra_structs_get(name="uCharacter")
  
  # Avoid: Multiple calls for individual offsets
  # (Ghidra MCP doesn't support per-offset queries anyway)
  ```
- **Cache structure definitions**: Store Ghidra DTI results for common types 
  (uCharacter, sShot) in memory to avoid repeated API calls.
- **Parallel test execution**: When testing multiple characters, launch tests in 
  parallel and aggregate results.
- **Decompile strategically**: Use `max_lines` parameter to limit output for 
  large functions:
  ```python
  # Get first 50 lines to check function signature and early logic
  ghidra_functions_decompile(name="cChr::Update", max_lines=50)
  
  # If needed, get more context with start_line offset
  ghidra_functions_decompile(name="cChr::Update", start_line=50, max_lines=50)
  ```

### 12.2 Robustness

- **Always validate JSON schema**: Check that comparator JSON contains required 
  fields before parsing.
- **Handle partial Ghidra results**: If field name is unavailable, fall back to 
  offset-based analysis.
  ```python
  result = ghidra_structs_get(name="cChr")
  fields = result.get("fields", [])
  
  # Search for field at target offset
  target_offset = 0x143c
  field = next((f for f in fields if f["offset"] == target_offset), None)
  
  if field and field.get("name") != f"field_{target_offset:x}":
      field_name = field["name"]
  else:
      # Field undefined, search decompiled code for usage
      field_name = f"unknown_at_0x{target_offset:x}"
  ```
- **Handle struct name variations**: Try multiple struct names when initial 
  lookup fails:
  ```python
  def find_field_in_structs(offset, struct_names):
      for name in struct_names:
          result = ghidra_structs_get(name=name)
          if "error" not in result:
              for field in result.get("fields", []):
                  if field["offset"] == offset:
                      return (name, field)
      return (None, None)
  
  # Usage
  struct_name, field = find_field_in_structs(
      0x143c, 
      ["uCharacter", "cChr", "cChrPlayer", "uActorBase"]
  )
  ```
- **Search decompiled code systematically** when fields are undefined:
  ```python
  # Get all functions that might access this class
  functions = ghidra_functions_list(name_contains="cChr", limit=200)
  
  # Decompile and search for offset pattern
  offset_hex = "0x143c"
  patterns_to_search = [
      f"this + {offset_hex}",
      f"param_1 + {offset_hex}",
      f"*(int *)({offset_hex}",  # Cast patterns
      f"+ {offset_hex})",
  ]
  
  for func in functions["functions"]:
      code = ghidra_functions_decompile(name=func["name"], max_lines=500)
      if any(pattern in code.get("decompiled_code", "") for pattern in patterns_to_search):
          # Found usage, analyze context
          print(f"Field at {offset_hex} used in {func['name']}")
  ```
- **Retry transient failures**: Wine process crashes and filesystem race 
  conditions may cause spurious failures. Retry once before marking as failed.

### 12.3 Maintainability

- **Use consistent field naming**: Match Ghidra field names exactly in code 
  comments.
- **Document uncertainty**: If classification is ambiguous, note "likely 
  missing_field_restore; verify with human review."
- **Preserve audit trail**: Keep all intermediate JSON reports and logs for 
  post-analysis.
- **Remove temporary diagnostic logging**: If logging is added to diagnose a 
  crash or desync, remove it once the issue is fixed and verified. Keep only 
  logging that is intentionally part of the permanent diagnostics design.
- **Remove temporary diagnostic hooks**: Temporary hooks may be added to 
  diagnose a crash or desync, but they must be removed once the issue is fixed 
  and verified. Keep only hooks that are intentionally part of the permanent 
  implementation.

### 12.4 Safety

- **Never restore vtable pointers**: Always verify field type before adding to 
  restore range. Restoring a vtable will cause instant crash.
- **Verify structure boundaries**: Check that offset + size does not exceed DTI 
  structure size.
- **Test incrementally**: After adding multiple fields in one iteration, if 
  crash occurs, bisect to identify the problematic field.
- **Distinguish gameplay from visual state**: Check field name and usage. Don't 
  restore blend buffers, camera state, or rendering-only fields.
  ```python
  visual_keywords = ["blend", "lerp", "camera", "render", "draw", "shadow", 
                     "particle", "audio", "sound", "lod", "cull", "alpha"]
  if any(kw in field_name.lower() for kw in visual_keywords):
      # Verify usage before restoring
      check_read_xrefs_for_gameplay_impact()
  ```
- **Request approval for invasive changes**: Before adding gameplay hooks or 
  modifying game behavior, present analysis to user and wait for confirmation:
  ```python
  def requires_user_approval(fix_type):
      invasive_changes = [
          "new_gameplay_hook",
          "modify_shot_creation",
          "modify_hit_detection",
          "modify_state_transition",
          "intercept_frequently_called_function"
      ]
      return fix_type in invasive_changes
  
  if requires_user_approval(proposed_fix.type):
      present_analysis_and_wait_for_approval()
  ```
- **Prefer simple restore over hooks**: If a field can be fixed with a restore 
  range, always prefer that over adding hooks. Hooks are last resort for 
  non-deterministic sources.

### 12.5 Communication

When reporting to human operators:
- **Concise verdicts**: "Jill Forward Throw: fixed after 2 iterations (mAnmSerial, 
  mActionId)."
- **Actionable failures**: "Ryu Hadoken: unknown field at 0x3a80; xrefs suggest 
  RNG state; requires human review."
- **Progress tracking**: "Batch 1/5 complete: 12 stable, 3 fixed, 2 pending."

## 13. Quick Reference

### Ghidra Investigation Workflow
```
Fast Path (use for 80% of cases):
1. Identify field → ghidra_structs_get(name="cChr")
2. Check: Named scalar, offset > 0x20, not pointer? → Yes
3. Add restore range → Edit rollback_restore.cpp
4. Build and verify → make -j8, re-run test
5. If fixed → Done! If not → Go to full analysis

Full Analysis (only if fast path fails or complex field):
1. Find write xrefs → ghidra_functions_list(name_contains="cChr")
2. Decompile writers → ghidra_functions_decompile()
3. Trace value sources → Check for non-deterministic sources
4. Build call graph → ghidra_analysis_get_callgraph()
5. Classify root cause → Restore vs normalize vs unknown
6. Apply targeted fix → Address root cause specifically
```

### Fast Path Eligibility
```python
✅ Use fast path if:
   - Field has clear name (not field_0xNNNN)
   - Type is scalar (int32_t, float, uint32_t)
   - Offset > 0x20 (not near vtable)
   - Region is character/action (not shot/effect)

❌ Skip fast path if:
   - Field undefined (requires code search)
   - Type is pointer or struct
   - Offset < 0x20 (likely vtable/base)
   - Region is shot/effect (may need recipe fix)
   - Previous fast path on this field failed
```

### Key Ghidra MCP Commands
```python
# Structure and class investigation
ghidra_structs_list(limit=200)  # List all structs
ghidra_structs_get(name="cChr")  # Get fields for specific struct
ghidra_classes_list(limit=200)  # List classes (for inheritance)
ghidra_datatypes_search(name="uCharacter")  # Search by name

# Function analysis
ghidra_functions_list(name_contains="cChr", limit=100)
ghidra_functions_decompile(name="cChr::SpawnProjectile", max_lines=100)
ghidra_analysis_get_callgraph(name="cChr::CheckActionTransition", max_depth=5)

# Cross-references
ghidra_xrefs_list(to_addr="0x405820")  # Who calls this function?
ghidra_xrefs_list(from_addr="0x405824")  # What does this call?

# Memory and vtables
ghidra_memory_read(address="0x12a4560", length=8)
ghidra_segments_list()  # Memory layout

# Handling gaps and undefined fields
# 1. Check related structs: cChr, uCharacter, uActorBase
# 2. Decompile functions and search for "this + 0xOFFSET"
# 3. Infer type from usage patterns in decompiled code
```

### State Analysis Checklist
For each divergent field:
- [ ] Identify all write xrefs (where is it modified?)
  - Search functions: `ghidra_functions_list(name_contains="ClassName")`
  - Decompile each: `ghidra_functions_decompile(name="...")`
- [ ] Extract write conditions (what state triggers changes?)
  - Look for `if` statements guarding writes
  - Note required field values (e.g., `mActionState == 5`)
- [ ] Trace value sources (where does new value come from?)
  - Follow data flow from RHS of assignments
  - Check if source is stable (game state) or unstable (timers, RNG)
- [ ] Check read xrefs (what uses this field?)
  - Decompile reader functions
  - Assess gameplay impact (hit detection, state transitions, etc.)
- [ ] Build dependency tree (what fields must be set first?)
  - Use call graphs: `ghidra_analysis_get_callgraph(...)`
  - Trace backward with xrefs: `ghidra_xrefs_list(to_addr="...")`
- [ ] Verify prerequisites are restored (check rollback_restore.cpp)
  - `grep -n "0xOFFSET" UMVC3Hook/code/rollback_restore.cpp`
- [ ] Classify: standalone field or dependent on earlier divergence?
  - Check if this is the earliest divergent frame in comparator report

### Handling Multiple Struct Names for Same Data
The game may have multiple views of the same character data:
- `cChr` - Runtime C++ class with methods
- `uCharacter` - Data-oriented struct view
- `cChrPlayer` - Subclass for player-controlled characters
- `uActorBase` - Base class with common actor fields

**Strategy when field not found:**
```python
# Comparator reports region "uCharacter.team0"
result = ghidra_structs_get(name="uCharacter")
if "error" in result or offset not in fields:
    # Try alternate names
    alternates = ["cChr", "cChrPlayer", "cChrEnemy", "uActorBase"]
    for name in alternates:
        result = ghidra_structs_get(name=name)
        # Check if offset exists in this struct
        if field_found_at_offset(result, offset):
            break

# If still not found, list all structs and search manually
all_structs = ghidra_structs_list(limit=500)
# Look for structs with size ~0x6a60 (expected character size)
```

### Test Outcomes
| Outcome | Exit Code | `.usdp` Present | Comparator Result | Action |
|---------|-----------|-----------------|-------------------|--------|
| `stable_sync` | 0 | No | N/A | Pass, move to next case |
| `stable_sync` | 0 | Yes | Exit 0 | Pass, move to next case |
| `confirmed_desync` | 1 | Yes | Exit 1 | Investigate and fix |
| `incomplete` | Any | Any | N/A | Retry or diagnose hang |
| `launch_failed` | N/A | No | N/A | Check Wine logs |
| `analysis_failed` | Any | Yes | Exit 2 | Check comparator logs |

### Classification Quick Guide
| Divergent Region | Offset Pattern | Likely Classification |
|------------------|----------------|------------------------|
| `uCharacter.*` | Named scalar field | `missing_field_restore` |
| `uCharacter.*` | Field with "blend", "camera", "render" | `visual_state` → comparator issue |
| `sShot`, `uShot.*` | Shot properties at creation frame | `incorrect_shot_recipe` |
| `sEffect`, `uEffect.*` | Effect properties at spawn frame | `incorrect_shot_recipe` |
| `sAction` | Action state fields | `missing_field_restore` |
| Any region | Pointer-sized field (4/8 bytes at +0x00) | Likely vtable, skip |
| Any region | Unknown field name in Ghidra | `unknown` → human review |

### Visual vs Gameplay State
```python
# Visual state keywords (DO NOT restore, report as comparator issue)
visual_keywords = ["blend", "lerp", "interp", "camera", "render", "draw", 
                   "shadow", "particle", "effect_visual", "audio", "sound", 
                   "lod", "cull", "alpha", "color", "scale_render"]

# Gameplay state keywords (MUST restore if divergent)
gameplay_keywords = ["hitbox", "hurtbox", "position", "velocity", "action", 
                     "state", "health", "meter", "combo", "input", "hitstun",
                     "blockstun", "advantage", "shot", "projectile"]

# Check field name before restoring
if any(kw in field_name.lower() for kw in visual_keywords):
    verify_not_used_in_collision_or_logic()
```

### Common Restore Range Patterns
```cpp
// Single scalar
{0x143c, 0x04}  // 4-byte int/float

// Adjacent scalars (merge into one range)
{0x2a40, 0x08}  // Two 4-byte fields at 0x2a40 and 0x2a44

// Small array
{0x2100, 0x100}  // 16 entries × 0x10 bytes each

// Embedded struct with pointer gaps
{0x1800, 0x10},  // Scalar members
{0x1828, 0x08}   // Skip pointers at 0x1810-0x1827
```

### Key File Locations
| Purpose | Path |
|---------|------|
| Apply restore fixes | `UMVC3Hook/code/rollback_restore.cpp` |
| Apply shot normalization | `UMVC3Hook/code/rollback_shot.cpp` |
| Check snapshot coverage | `UMVC3Hook/code/rollback_snapshot.cpp` |
| Verify structure definitions | Ghidra: umvc3.exe.unpacked.exe analysis |
| Run comparison | `tools/compare_desync_dumps.py` |
| Launch test | `wine umvc3.exe -autotest=<char> -autotest-moves=<command-id>` |

### Critical Constraints
- ✅ Single run (`--repeat=1`) detects deterministic desyncs
- ✅ Fix one field per iteration, verify immediately
- ✅ Exit code `1` + new field = previous fix succeeded
- ✅ Always trace to earliest divergent frame
- ✅ Try fast path first (simple restore), only analyze if it fails
- ✅ Check for visual state keywords before restoring
- ❌ Never restore vtable pointers (crash risk)
- ❌ Never restore visual-only fields (blend, camera, render state)
- ❌ Never modify comparator exclusion rules to pass tests
- ❌ Never skip Ghidra lookup for confirmed desyncs
- ❌ Never add gameplay hooks without user approval
- ⚠️  Always request user confirmation for:
  - New gameplay hooks that intercept game functions
  - Changes to shot creation, hit detection, state transitions
  - Any fix beyond simple restore ranges

## 14. Complete Behavioral Tracing Example

### Scenario: Jill Forward Throw Desync

**Comparator report:**
```json
{
  "first_divergent_frame": 58,
  "first_divergent_region": "uCharacter.team0",
  "first_divergent_offset": 5180,
  "offset_hex": "0x143c",
  "host_value": "30 01 00 00",
  "client_value": "2b 01 00 00"
}
```

### Attempt 1: Fast Path (2 minutes)

**Step 1: Quick field identification** (30 seconds)

```python
ghidra_structs_get(name="uCharacter")
# Returns: uCharacter found, size 0x6a60
# Fields include offset 0x143c: mAnmSerial, type: int32_t
```

**Assessment:**
- ✅ Field has clear name: `mAnmSerial`
- ✅ Type is scalar: `int32_t`
- ✅ Offset > 0x20: `0x143c` is far from vtable
- ✅ Region is character: `uCharacter`
- **Decision: Use fast path**

**Step 2: Apply restore immediately** (30 seconds)

Edit `UMVC3Hook/code/rollback_restore.cpp`:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    {0x0000, 0x1000},
    
    // mAnmSerial: Animation serial number
    // Evidence: Jill Forward Throw desyncs at frame 58, offset 0x143c
    // Type: int32_t (from Ghidra uCharacter struct)
    {0x143c, 0x04},
    
    {0x2000, 0x3000},
};
```

**Step 3: Rebuild and verify** (90 seconds)

```bash
make -j8
cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
wine umvc3.exe -autotest=Jill -autotest-moves=<command-id> -autotest-repeat=1
```

**Result:** No `.usdp` files created → **Desync resolved!**

**Fast path succeeded. Total time: 2.5 minutes. Move to next case.**

---

### Alternative: If Fast Path Had Failed

**Suppose the verification showed:** Exit code `1`, same field still diverges.

**This means fast path failed. Now invest in full analysis.**

### Attempt 2: Full Behavioral Analysis (15 minutes)

**Step 1: Find Write Operations** (3 minutes)

Use Ghidra MCP to search for write references:

```python
# Get all functions that might write to this field
ghidra_functions_list(name_contains="cChr", limit=200)

# Decompile likely candidates
ghidra_functions_decompile(name="cChr::PrepareThrowSequence", max_lines=100)
ghidra_functions_decompile(name="cChr::OnAnimationEvent", max_lines=100)
```

**Write xref 1:** `cChr::PrepareThrowSequence+0x42`
```cpp
void cChr::PrepareThrowSequence(cChr *this) {
    this->mActionId = 0x15;
    this->mActionState = AS_ATTACKING;
    this->mAnmSerial = g_sMain->mFrameTimer;  // ← Non-deterministic!
    this->PlayAnimation("jill_throw_startup");
}
```

**Write xref 2:** `cChr::OnAnimationEvent+0x1a`
```cpp
void cChr::OnAnimationEvent(cChr *this, int eventId) {
    if (eventId == 0x200) {
        this->mAnmSerial = g_sMain->mFrameTimer;  // ← Non-deterministic!
        this->SpawnProjectile();
    }
}
```

**Key finding:** Field is written from `g_sMain->mFrameTimer` during re-simulation, 
**after restore completes**. This overwrites the restored value with a 
non-deterministic timer value.

**Step 2: Trace Value Source** (2 minutes)

```python
ghidra_structs_get(name="sMain")
# mFrameTimer at offset 0x48: Monotonic frame counter
```

**Analysis:** `mFrameTimer` continues incrementing during re-simulation. When 
rolling back from frame 304 to frame 299, host's timer stays at 304 while 
client's is at 299.

**Step 3: Find Read Operations** (2 minutes)

```python
ghidra_functions_decompile(name="cChr::SpawnProjectile", max_lines=100)
```

```cpp
uShot* cChr::SpawnProjectile(cChr *this) {
    if (g_sShot->FindInDeduplicationRing(this->mAnmSerial)) {
        return nullptr;  // Skip duplicate
    }
    
    uShotNormal *shot = new uShotNormal();
    shot->mRequestAnmSerial = this->mAnmSerial;
    shot->mOwner = this;
    g_sShot->AddToDeduplicationRing(this->mAnmSerial);
    return shot;
}
```

**Usage:** Deduplication ring for preventing double-spawns.

**Step 4: Classify Root Cause** (1 minute)

- Field is restored correctly ✅
- But then **overwritten** by game code during re-simulation ❌
- Source value is non-deterministic (`mFrameTimer`) ❌
- **Classification:** Field needs **normalization**, not just restore

**Step 5: Apply Targeted Fix** (3 minutes)

**Option A:** Hook the write sites to normalize during re-simulation:
```cpp
// In cChr::OnAnimationEvent hook
void Hook_OnAnimationEvent(cChr *this, int eventId) {
    if (eventId == 0x200) {
        if (g_isResimulating) {
            this->mAnmSerial = g_resimulateCurrentFrame;  // Deterministic
        } else {
            this->mAnmSerial = g_sMain->mFrameTimer;  // Original behavior
        }
        this->SpawnProjectile();
    }
}
```

**Option B:** Keep restore, prevent overwrites by protecting the field:
```cpp
// More invasive, requires hooking multiple write sites
```

**Decision:** Option A is cleaner. Normalize at source.

**⚠️ IMPORTANT: This requires adding new gameplay hooks - requires user approval**

Before implementing normalization hooks that change gameplay behavior, present 
the analysis to the user:

```
Analysis complete for mAnmSerial desync:

Field: uCharacter::mAnmSerial at 0x143c
Issue: Field is written from non-deterministic source (g_sMain->mFrameTimer) 
       during re-simulation, after restore completes
Impact: Shot deduplication uses different values on host/client
Root cause: Game code overwrites restored value with wall-clock timer

Proposed fix requires NEW GAMEPLAY HOOK:
  - Hook cChr::OnAnimationEvent to normalize mAnmSerial during re-simulation
  - Changes: Intercepts animation event processing (invasive)
  - Risk: May affect normal gameplay if hook logic is incorrect
  - Alternative: None identified (simple restore won't work)

Proceed with gameplay hook? [y/n]
```

**Wait for user confirmation before:**
1. Adding new function hooks that intercept gameplay code
2. Modifying game behavior during normal (non-resim) execution
3. Adding hooks to frequently-called functions (performance impact)
4. Changing shot creation, hit detection, or state transition logic
5. Any fix classified as "high risk" in safety assessment

**Non-invasive changes that don't require confirmation:**
- Adding restore ranges for scalar fields (standard operation)
- Extending existing restore ranges to include adjacent fields
- Adding comments and documentation
- Fixing typos in existing restore code

**Step 6: Rebuild and Verify** (2 minutes)

```bash
make -j8
cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
wine umvc3.exe -autotest=Jill -autotest-moves=<command-id> -autotest-repeat=1
```

**Result:** No `.usdp` files created → **Desync resolved with normalization!**

**Total time with full analysis: ~15 minutes**

---

### Lessons from This Trace

1. **Fast path first**: Always try simple restore before deep analysis (saves 80% of time)
2. **Fast path failure reveals complexity**: Field being overwritten indicated 
   normalization was needed
3. **Value source matters**: Non-deterministic source (`mFrameTimer`) required 
   different fix strategy
4. **Read xrefs confirm impact**: Deduplication usage showed field was critical
5. **Time allocation**: 2 minutes for simple case, 15 minutes for complex case
   - Most fields are simple
   - Investment in analysis only when needed

```python
# First, try the exact region name from comparator
ghidra_datatypes_search(name="uCharacter")
# Returns: uCharacter found, size 0x6a60

ghidra_structs_get(name="uCharacter")
# Returns full structure with fields list

# Navigate to offset 0x143c in the fields array
# Field: mAnmSerial
# Type: int32_t
# Comment: "Animation serial number for event deduplication"
```

**If field is not found in uCharacter, check related classes:**

```python
# The comparator might report "uCharacter" but Ghidra uses "cChr"
ghidra_structs_list(limit=200)
# Look for: cChr, cChrPlayer, cChrEnemy, uActorBase, etc.

ghidra_structs_get(name="cChr")
# Check if offset 0x143c is defined here

# If offset < base class size, check base class
ghidra_structs_get(name="uActorBase")
# Base classes contain shared fields at lower offsets
```

**Handling gaps in field definitions:**

```python
result = ghidra_structs_get(name="cChr")

# If field at 0x143c is undefined, search decompiled code
ghidra_functions_list(name_contains="cChr", limit=100)

# Decompile likely functions
ghidra_functions_decompile(name="cChr::PrepareThrowSequence", max_lines=100)
# Look for references to "this + 0x143c" or "*(int *)(this + 0x143c)"
```

### Step 2: Find Write Operations

Use Ghidra MCP to find all cross-references to the field:

```python
# First, get the base address of uCharacter instances
# You may need to search for known character addresses or use memory regions
ghidra_xrefs_list(to_addr="0x4a2c82")  # Example: known write location
```

Decompile functions that write to this field:

```python
ghidra_functions_decompile(name="cChr::PrepareThrowSequence", max_lines=50)
```

**Write xref 1:** `cChr::PrepareThrowSequence+0x42` (address: 0x4a2c82)
```cpp
void cChr::PrepareThrowSequence(cChr *this) {
    this->mActionId = 0x15;
    this->mActionState = AS_ATTACKING;
    this->mAnmSerial = g_sMain->mFrameTimer;  // ← Write location
    this->PlayAnimation("jill_throw_startup");
}
```

**Write xref 2:** `cChr::OnAnimationEvent+0x1a` (address: 0x4a3f1a)

```python
ghidra_functions_decompile(name="cChr::OnAnimationEvent", max_lines=50)
```

```cpp
void cChr::OnAnimationEvent(cChr *this, int eventId) {
    if (eventId == 0x200) {  // Spawn projectile event
        this->mAnmSerial = g_sMain->mFrameTimer;  // ← Write location
        this->SpawnProjectile();
    }
}
```

### Step 3: Trace Value Source

Both writes use `g_sMain->mFrameTimer`. Locate this field:

```python
ghidra_structs_get(name="sMain")
# Field: mFrameTimer at offset 0x48
# Type: uint32_t
# Comment: "Monotonic frame counter, incremented every game frame"
```

Check if `mFrameTimer` is deterministic during re-simulation:

```python
# Decompile the main update function
ghidra_functions_decompile(name="sMain::Update", max_lines=100)
```

**Decompiled code:**
```cpp
void sMain::Update(sMain *this) {
    this->mFrameTimer++;  // Incremented every frame
    this->UpdateCharacters();
    this->UpdateShots();
}
```

**Analysis:** `mFrameTimer` is a wall-clock counter that continues incrementing 
during re-simulation. When rolling back from frame 304 to frame 299, 
`mFrameTimer` is still 304 on host but 299 on client.

### Step 4: Find Read Operations

Use Ghidra MCP to find functions that read this field:

```python
# Search for functions that access mAnmSerial
# Since direct field xrefs may be difficult, decompile likely functions
ghidra_functions_decompile(name="cChr::SpawnProjectile", max_lines=100)
```

**Read xref 1:** `cChr::SpawnProjectile+0x8` (address: 0x4a5d08)
```cpp
uShot* cChr::SpawnProjectile(cChr *this) {
    // Check if shot with this serial was recently spawned
    if (g_sShot->FindInDeduplicationRing(this->mAnmSerial)) {
        return nullptr;  // Skip duplicate spawn
    }
    
    uShotNormal *shot = new uShotNormal();
    shot->mRequestAnmSerial = this->mAnmSerial;  // Copy serial to shot
    shot->mOwner = this;
    g_sShot->AddToDeduplicationRing(this->mAnmSerial);
    return shot;
}
```

**Analysis:** `mAnmSerial` is used to prevent spawning the same projectile 
multiple times if animation events fire redundantly. Different values between 
host/client could cause:
- Shot spawns on host but not client (desync in shot count)
- Shot properties differ (if serial is copied to shot object)

### Step 5: Build Call Graph

Use Ghidra MCP to generate call graph:

```python
ghidra_analysis_get_callgraph(name="cChr::SpawnProjectile", max_depth=3)
# This returns a graph showing what SpawnProjectile calls
```

**Result:**
```
cChr::SpawnProjectile
  → g_sShot::AddShot
    → g_sShot::ResizeArray
    → g_sShot::InitializeShot
      → uShotNormal::Initialize
  → g_sShot::AddToDeduplicationRing
  → cColl::RegisterShotHitbox
```

**Backward trace using xrefs:**
```python
# Find what calls cChr::SpawnProjectile
ghidra_xrefs_list(to_addr="0x4a5d00")
# Returns list of calling functions
```

**Called by:**
- `cChr::OnAnimationEvent` (frame-driven, animation event 0x200)
- `cChr::ProcessThrowSequence` (action-driven, throw attack sequence)

### Step 6: Determine State Requirements

For `mAnmSerial` to change during forward throw:

**Required states:**
1. Character must enter action 0x15 (forward throw)
   - Requires: Opponent in grab range
   - Requires: Player input: forward + special button
   - Field modified: `mActionId = 0x15`

2. Animation must reach frame with event 0x200
   - Requires: Animation "jill_throw_startup" playing
   - Requires: `mAnmFrame >= 5` (event trigger frame)
   - Field checked: `mAnmFrame`

3. Frame timer must advance
   - Always true (increments every frame)
   - Field modified: `g_sMain->mFrameTimer`

**Dependency chain:**
```
Player input (frame 55)
  → mActionId set to 0x15 (frame 56)
    → Animation "jill_throw_startup" plays (frame 56)
      → mAnmFrame increments (frames 56, 57, 58)
        → Event 0x200 fires (frame 58)
          → mAnmSerial = mFrameTimer (frame 58)
            → SpawnProjectile uses mAnmSerial (frame 58)
```

### Step 7: Check Restore Coverage

```bash
grep -n "0x143c" UMVC3Hook/code/rollback_restore.cpp
# No matches found
```

**Conclusion:** `mAnmSerial` at offset 0x143c is **NOT** in any restore range.

Check if surrounding fields are restored:
```cpp
// Existing restore ranges in rollback_restore.cpp:
{0x0000, 0x1000},   // Basic character state
{0x2000, 0x3000},   // Action and animation state
// Offset 0x143c is in the gap between 0x1000 and 0x2000
```

### Step 8: Classification

- Field: `uCharacter::mAnmSerial` at 0x143c
- Captured: Yes (part of uCharacter snapshot)
- Restored: No (not in any restore range)
- Gameplay impact: High (affects shot deduplication, causes shot spawn desync)
- **Classification: Missing field restore**

### Step 9: Apply Fix

Edit `UMVC3Hook/code/rollback_restore.cpp`:

```cpp
static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    {0x0000, 0x1000},
    
    // mAnmSerial: Animation event deduplication serial
    // Evidence: Jill Forward Throw desyncs at frame 58 without this restore
    // Ghidra: int32_t at uCharacter+0x143c
    // Usage: Prevents duplicate projectile spawns during animation events
    // Source: Written from g_sMain->mFrameTimer (non-deterministic)
    // Xrefs: cChr::PrepareThrowSequence+0x42, cChr::OnAnimationEvent+0x1a
    // Read by: cChr::SpawnProjectile+0x8 for deduplication check
    {0x143c, 0x04},
    
    {0x2000, 0x3000},
};
```

### Step 10: Verify Fix

```bash
make -j8
cp UMVC3Hook.asi "../ULTIMATE MARVEL VS. CAPCOM 3/"
wine umvc3.exe -autotest=Jill -autotest-moves=<command-id> -autotest-repeat=1
```

**Result:** No `.usdp` files created → Desync resolved

### Lessons from This Trace

1. **Value source matters:** Field derived from `mFrameTimer` (non-deterministic) 
   needed restoration, not normalization.
2. **Read xrefs reveal impact:** Deduplication usage showed this was 
   gameplay-critical, not cosmetic.
3. **Call graph confirmed scope:** Used only in shot spawn path, not other 
   systems, so isolated fix was safe.
4. **Gap in restore ranges:** Field fell between two restored regions, easy to 
   miss without systematic checking.

This complete trace took approximately 5-10 minutes with Ghidra MCP access and 
resulted in a targeted, documented fix.

## 15. Summary: Pragmatic Investigation Strategy

### The Two-Tier Approach

**Tier 1: Fast Path (80% of cases, ~3 minutes each)**
```
Desync detected
  → Quick Ghidra lookup (30 sec)
  → Field is named scalar? YES
  → Add restore range immediately (30 sec)
  → Build and verify (2 min)
  → Fixed? YES → Done!
```

**Tier 2: Full Analysis (20% of cases, ~15 minutes each)**
```
Fast path failed OR complex field detected
  → Find write xrefs (3 min)
  → Analyze write contexts (3 min)
  → Trace value sources (2 min)
  → Find read xrefs (2 min)
  → Classify root cause (1 min)
  → Apply targeted fix (3 min)
  → Verify (2 min)
  → Fixed? YES → Done!
```

### Decision Matrix

| Field Characteristic | Fast Path? | If Fails, Analysis Focus |
|---------------------|------------|--------------------------|
| Named scalar, offset > 0x20 | ✅ Yes | Check for overwrites after restore |
| Undefined (field_0xNNNN) | ❌ No | Search decompiled code for usage |
| Pointer type | ❌ No | Verify if vtable, resource handle |
| Shot/effect region | ❌ No | Check recipe normalization |
| Offset < 0x20 | ❌ No | Verify base class, vtable |
| Previous fast fix failed | ❌ No | Trace non-deterministic sources |

### Time Budget Guidelines

**Per field:**
- Fast path success: 2-3 minutes
- Fast path failure + analysis: 15-20 minutes
- Average (80% fast, 20% analysis): ~5 minutes per field

**Per character (assume 3 desyncs per character):**
- All simple: ~10 minutes
- Mixed (2 simple, 1 complex): ~20 minutes
- All complex: ~50 minutes

**Optimization strategies:**
1. **Batch simple fixes**: Identify all simple fields across multiple desyncs, 
   apply all fixes in one edit, single rebuild
2. **Parallel verification**: Run multiple test cases simultaneously if system 
   supports it
3. **Cache Ghidra results**: Store struct definitions in memory, avoid repeated 
   API calls
4. **Learn patterns**: After fixing 10-20 desyncs, common fields emerge 
   (mActionId, mAnmFrame, etc.)

### Success Metrics

An efficient AI controller should achieve:
- **Fast path success rate**: 75-85% (most fields are simple scalars)
- **Average time per field**: 4-6 minutes
- **Fields fixed per hour**: 10-15 with full analysis, 20+ with fast path
- **False fix rate**: < 5% (fixes that don't resolve desync)

### Final Workflow

```
for each desync:
    field = identify_field_from_comparator_json()
    
    if is_simple_scalar(field):
        apply_restore_range(field)
        if verify():
            mark_fixed()
            continue
        else:
            # Fast path failed, field is complex
            pass
    
    # Full analysis required
    write_xrefs = find_write_operations(field)
    read_xrefs = find_read_operations(field)
    source = trace_value_source(write_xrefs)
    
    if source.is_non_deterministic():
        apply_normalization_fix(source)
    elif field.is_overwritten_after_restore():
        apply_write_hook_or_late_restore(field)
    elif field.is_computed_property():
        restore_dependencies_instead(field)
    else:
        apply_complex_restore_with_safeguards(field)
    
    if verify():
        mark_fixed()
    else:
        escalate_to_human(field)
```
