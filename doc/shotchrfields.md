Yes. Besides the fixed uShot+0x1F70 self-reference, I found these issues.
High Confidence
- uChunLi+0x6900: mChargeReady
  - Gameplay-relevant charge-completion latch.
  - Read and written by uChunLi__playSfx_BAD0.
  - Not included in CCHR_SCALAR_RESTORE_RANGES.
  - mChargeTime at +0x68F0 is restored, but the completion latch it drives is not. A rollback after charge completion can therefore restore the timer without restoring whether Chun-Li is considered charged.
- uChunLi+0x68F8: mpKikokenEffect
  - Character-owned effect pointer.
  - Native Chun-Li logic checks its alive state and destroys or clears it.
  - It cannot be copied directly; it needs an effect recipe remap or validated live-pointer policy.
  - Leaving it on the future timeline can make restored charge/Kikoken logic observe the wrong effect lifecycle.
- Incorrect Chun-Li size/comparator exclusion
  - The exported schema reports uChunLi size 0x68F4.
  - Ghidra’s constructor shows the real class extends through at least 0x6907, with an indicated size of 0x6910.
  - The comparator currently excludes [0x68F4, 0x6A60) as adjacent-allocation spill.
  - That exclusion hides the real fields at +0x68F8, +0x6900, and +0x6904.
  - Therefore current Chun-Li dumps cannot report these tail divergences even though the bytes are captured.
Object Graph Issue
- sAction+0x148: current uExAtkSpace*
  - In the latest run, the host retained an ExAtkSpace while the client had already cleared it.
  - By frame 404, host mDestroyExAtkSpace at +0x150 was 1, client was 0.
  - By frame 442, host mCurExAtkSpaceType at +0x144 was 8, client was 0.
  - The pointer is deliberately left live because it owns child effects, but the reconstructed effect graph is not preserving equivalent lifecycle state.
  - This did not cause the Chun-Li action split, but it is a real synchronization defect that can affect hyper backgrounds, camera behavior, and attack-space cleanup.
Shot Candidates
- uShot+0x1F78..0x1FDF: 13 owned effect pointers
  - Intentionally not flat-restored because doing so causes stale-pointer destruction crashes.
  - Native shot lifecycle code reads this array, including uShot__getGameEffectChrAtSlot.
  - Fresh reconstruction can therefore make lifecycle decisions using a different child-effect graph.
  - The +0x1F70 fix addresses the demonstrated Kikosho failure, but the remaining slots still require semantic reconstruction if another shot depends on them.
- uShot+0x1F50: runtime mId
  - The `20260802_220647` capture confirms this is gameplay-relevant allocator identity. Both peers spawned Kikosho IDs 16/17, but rollback reconstruction changed only the client shots to 18/19 while retaining the saved manager cursor.
  - The mismatch changed target-consumption state at frame 4703, then propagated through P2 assist 2's mDSpd/mDAcl into a one-unit position difference at frame 4704.
  - It is distinct from mRequestAnmSerial at +0x211C, which is used by the character shot-deduplication ring.
  - Restore this four-byte scalar explicitly after preserving the surrounding fresh-owned cAtkCtrl/cArm block.
Probably Derived
- uCharacter+0x6A5 and +0x6B5
  - First differing bytes in the dump, inside the first uModel::Motion record.
  - They differed only during frames 217–220 and then reconverged.
  - No causal propagation to the Kikosho failure was found.
  - Most likely model/motion evaluation flags, but their exact semantics remain unidentified.
- uCharacter+0x12F0
  - Large repeated differences across several character-region aliases.
  - Appears associated with toon/model coordinate state.
  - Strongly presentation-derived and not connected to the action transition.
mCmpS32 itself is already restored by {0x1350, 0x98}. Its divergence was a correctly recomputed symptom of the missing Kikosho shot, not an omitted scalar.
