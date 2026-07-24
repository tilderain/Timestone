# Re-simulation Divergence Methodology

**Status: Historical methodology from a different codebase/build.** The symbols
and offsets below are not implementation references for this repository. The
general compare-at-entry/compare-at-exit and restore-boundary bisect remains
useful. See [`dash_resim_root_cause.md`](dash_resim_root_cause.md) for the
verified current input call order and [`current.md`](current.md) for current
rollback architecture.

The symbols cited here (`build_manifest`, `resim_mode.cpp`, `diag::walk_fields`,
`hooks.cpp`, and `g_in_resim`) are not the current UMVC3Rollback implementation.
Current diagnostics live primarily in `rollback.cpp` and `diag.cpp`, using
`g_isResimulating` and NDJSON snapshots. The
general dataflow/bisect method remains useful, but the integration instructions
below must not be followed literally.

The analyzed system identified which class/region diverged, but not which field within the class. To get to why, the proposed path was per-field diff through its reflection system:
Recommended approach:
1. Hook into the per-character move (vtbl[+64] / uCharacter__move_base) — not the full sMvc3Main::move. This gives you a tight before/after window around just the character movement function.
2. Before the call: capture cChr fields via diag::walk_fields(cChr, ...) into a typed key-value map (the reflect::FieldSchema cache at reflect.h:47 already has offset/type/size for every field).
3. After the call: walk fields again, compare each field's bytes. Any field that changed is the delta produced by the character move function this tick.
4. Do this on both paths — the live tick and the resim tick (gated on g_in_resim). Compare the two deltas. If delta_live != delta_resim, you know exactly which field(s) the character move function wrote differently, and what the two values were.
Existing plumbing you'd reuse:
- diag::walk_fields() (diag_reflect.h:36) — enumerates every reflected field
- for_each_fighter() (hooks.cpp:114) — iterates all live characters
- g_in_resim flag — distinguishes original vs resim execution
- sampler_begin/sampler_tick (diag_reflect.h:75-89) — already does per-frame CSV field sampling if you want offline analysis
Result: Instead of "cChr region at offset 0x1310 diverged", you'd get "mPosY at offset +0x54 was 123.4 on live path, 567.8 on resim path" — naming the actual field and the two values. That identifies the symptom; for root cause you'd then check whether input, RNG, collision, or action state fed into that field differently.



Here's the full root cause methodology, from symptom to origin:
Phase 1: Narrow the write site
Disassemble the function to find stores to the diverging offset, then use Ghidra dataflow to trace backward from each store:
# Example: find all writes to cChr+0x54 (position Y)
rg -n "\[RSI \+ 0x5[0-9a-f]\]"  # or specific offset
For each MOV [reg + 0x54], value, trace backward through the function to identify every source read feeding into value.
Phase 2: Snapshot ALL inputs at function entry
This is the key diagnostic step. Hook the character move function entry — on both live and resim paths — and snapshot every field the function could possibly read. The quickest way is to walk all reflected fields of cChr:
for_each_fighter(chr, ...) {
    diag::walk_fields(chr, [](FieldInfo& f) {
        // record (offset, size, bytes) for every non-getter field
    });
}
Compare the two input snapshots. Every field that's already different at entry is a carrier — the divergence was introduced before this function. That's where you recurse.
Phase 3: Recurse up the dataflow chain
For each carrier field found in Phase 2, ask: what ran before this function that writes that field?
In the sMvc3Main::move execution order (see sMvc3Main_simulation_loop.md), the character move function at fencepost #30 is preceded by:
Order	Subsystem
#28	sShot moveA (kill order)
#29	sGameEffect moveA
#30	sUnit::moveCategory(cat=0)
Wait — the character move IS in sUnit::moveCategory(cat=0). So anything inside the same category walk could affect later characters. But more importantly: most cChr fields that feed into the character move function were set on the PREVIOUS frame's character move. The character move function reads its own previous output (position, velocity, action state, etc.).
So root cause chains typically look like:
Frame N, char move writes mPosY = 123.4
  ↓ (save snapshot at end of frame N)
Frame N+1, resim restores frame N state
  ↓ (restore missed some field — mSomething = 0 instead of 42)
Frame N+1, char move reads mSomething (which is wrong) → mPosY diverges
The fix is to find which field restore missed. That's in snapshot_frame / restore_frame.
Phase 4: Use the save/restore as the bisect boundary
The most efficient bisect: after identifying a carrier field that's wrong at character move entry, check whether it's wrong immediately after restore (before any resim frames run). If yes → save/restore gap. If no → it diverged during the resim replay of earlier frames.
For save/restore gaps: check snapshot_frame() (hooks.cpp:230-430) to verify the field's offset range is in the memcpy. cChr body is saved at line 419 (memcpy(c->s->cChr[i], chr, sz)) covering 0x0000..sz. If the field is within that range, the gap is in restore_frame — likely a pointer remapping or skip-list issue.
Phase 5: Field-level log comparison (the final tool)
If the input fields are identical at entry but the output diverges, the function itself ran differently. The culprit is one of:
Source	How to verify
RNG	Compare cChr+0x2D80 (per-character MtXorShift) at function entry
Global state	Compare sMain timing, sEffect RNG at +0x3C
Input	Compare cChr+0x874 (input buffer) and sNetPad game input
Collision results	cChr pushbox separation is resolved by sCollision::move (#31), which runs AFTER character move (#30) in the same tick — so collision from the previous frame's move is what feeds the current frame
Job thread order	Verify mJobThreadNumInit was forced to 1 during resim (single-threaded fast path)
Summary checklist
1. Diverging field identified          ← per-field diff via walk_fields
2. Check if wrong at fn entry          ← if yes, root cause is upstream
3. If same at entry: check RNG/input   ← RNG at cChr+0x2D80, input at cChr+0x874
4. If RNG differs: RNG not preserved   ← save/restore gap on per-char MtXorShift
5. If input differs: input replay bug  ← check sNetPad injection in do_resim_call
6. If neither: missing save field      ← check snapshot_frame covers the field
7. If all match: job-thread jitter     ← verify mJobThreadNumInit=1 during resim
