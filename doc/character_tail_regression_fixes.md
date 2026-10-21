# Character Tail Regression Fixes

This implementation ledger supersedes conflicting ownership and coverage claims
in `chrfields.md`. It consolidates the three 12-agent native audits. Static
writer/consumer evidence is not runtime replay validation.

## Scalar And Vector Corrections

Offsets and sizes are hexadecimal. These are early restores; the existing
reviewed `68F0` late-copy convention is unchanged.

| Character | Added/corrected payload | Representative native evidence |
|---|---|---|
| Ryu | `6914/4` directional mode | `140090BB0`, `1400C5F90` |
| Wesker | `68F0/0C` cached position | `1400EEF50` |
| Dante | `68F8/1`, `6920/2` install/effect/command latches | `140070A40`, `140071BF0`, `140071B90` |
| Deadpool | `68F8/4`, `6900/30`, `6930/1` flags, three position records, voice latch | `140074EB0`, `140074E90`, `140074470` |
| Strange | `6928/6`, `6948/4` elapsed counter, command latches, voice clock | `14007A660`, `14007AB70`, `14007AB80`, `14007AD00` |
| StrangeSh | `692C/2` inherited command latches | setup `14007DC40` |
| Felicia / FeliciaC | `68F0/1` / `68F8/2` install and movement latches | `14007FBE0`, `1400804D0` |
| Frank | `69E0/6`, `6A00/0C` experience/ability history and position | `140081A79`, `140081B82`, `140083B50` |
| Hawkeye | `68F4/4` mutable movement-range minimum | `1400C8A50`, `14008E990` |
| Strider | `6910/0C` cached position | `140092E70` |
| Hulk | `6904/4` animation checkpoint | `140095C70` |
| Iron Fist | `6910/0C` cached position | `140097D40` |
| Iron Man | `6910/0C`, `6928/1` position and command latch | `14009A590`, `140099E20` |
| Jill | `6908/1` move-active edge latch | `14009CEB0` |
| Hsien-Ko | `68FC/4`, `6914/4` reserved Anki and pending action | `1400D46A0`, `1400A1980` |
| Morrigan / MorriganSh | `68F8/1` / `6E90/1` command/continuation latches | `1400AC470`, `1400AE4A0` |
| Wright | `6940/2` UI and voice latches, not claimed as combat causes | `1400AF1A0`, `1400B1850` |
| Phoenix | `6930/1` resurrection-used latch | `1400BC873`, `140031FDB` |
| Firebrand | `68F8/4` animation checkpoint | `1400BEC90` |
| Sentinel | `6908/2` command and move-active latches | `1400CAAE0`, `1400CA230` |
| Storm | `6908/2`, `690C/11`, `6920/0E` gates, flight, RNG clock, target movement | `1400D7240`, `1400D76D0`, `1400D8100` |
| Super-Skrull | `68F0/0C` complete XYZ instead of only X | `1400DB9F0` |
| Thor | `690C/1` command latch | `1400E09C0`, `1400DFEC0` |
| Vergil | `6910/1` DT latch | `1400E7DD0`, `14002B46C` |
| Wolverine | `6900/0C` cached position | `1400F0F60` |
| X-23 | `68F8/1` command latch | `1400F33D0`, `1400F2EA0` |
| Zombie | `6900/0C`, `6910/4` previous-parent position/action | `1400FA9E0`, `1400FAF90` |
| Viper | `68F8/1` effect spawn-attempt latch | `14006D670` |

The native helper named `MtArray__emptyBuffer` at `14000BC10` only clears
the fourth vector lane. It did not establish heap ownership for the cached
positions above. Conversely, **Hsien-Ko `6918/8` is Linlin's identity**
(`1400A1B20`); the old scalar copy was removed. Its sound cRef begins at `6920`.

Vergil's `68F0/12` sword bank ends at `6902` exclusive; it does not include
`6910`. Earlier statements to the contrary were arithmetic errors.

Shuma's material selector `6928/4`, animation flags/clocks `7494/10`, and repeat
byte `74A8` also rewind. Controller ID/frame/slot payloads rewind only with
matching parent and animation handle bindings. Owners and StringHandles remain
live. This is presentation-state restoration, not a demonstrated combat fix.

## Exact Reference Slots

The following slots use saved spawn-sequence identity and authoritative null,
not an arbitrary owned shot of the same class. They repair at the existing
post-reconstruction and late reference stages.

| Character | Slots | Shot family |
|---|---|---|
| Amaterasu | `6908` | One Spark |
| Spider-Man | `6900` | Web Move |
| Trish | `68F8`, `6900` | Barrier |
| Phoenix | `68F8`, `6900`, `6908` | TK Bind / Healing Field |
| MODOK | `6910`, `6918` | Light Wall / Jamming Bomb |
| Dormammu | `6900` | Fire Carpet |
| Shuma | `68F8`, `6900`, `6908`, `6910`, `6918`, `6920` | Mystic Stare Bomb |

The Shuma replacement setter is not used during repair: it retires the current
occupant, which could destroy a newly reconstructed bomb.

Helper owner slots (Wright/Mayoi, Frank photo helpers, Zero/ZeroSh,
Morrigan/MorriganSh, Tron/Kobun, Hsien-Ko/Linlin, FirebrandSh's source) restore
null or a captured surviving identity validated against the current team or
owning slot and vtable. Missing historical identities are reported and cleared;
they are not resurrected by raw address. This is **not generation-aware helper
recreation**. Normally registered Mayoi and shadows are team-chain members and
already receive full character capture.

## Auxiliary Actors

Kobun and Linlin are `14D0`-byte actor models, not `uCharacter` instances.
Snapshots now discover them through the fighter's actual owner slot, capture
their actor bodies and motion/joint work, and mark work regions as dependent on
the validated actor. A replaced actor prevents dereferencing its old work-owner
addresses. The separate portable kernel restores actor playback and movement
history while preserving resource pointers, parent identity and intrusive links.

Kobun movement includes both float-pair/byte movement records at `1470` and
`147C`, tuning/history `1490..14CB`, and completion byte `14CD`. Linlin also
restores its active byte `14CE`. No character constraints/ability offsets are
applied to these actors. Pose and visibility are reapplied after reconstruction
because King Kobun setup changes its retained helper. External work allocations
must still match; arbitrary replacements are not reconstructed.

King Kobun fallback lookup now searches category **9**, filters by the owning
fighter, and respects saved-null absence. Same-address reuse without a recorded
allocation generation remains a limitation.

## Character Effects

The existing short-factory recipe path now tracks generic short effects for
explicitly reviewed character owner slots, including Storm's three effects.
Capture records current attachment/joint, control state (including Storm `294`),
flags `298`, effect timeline, exact factory arguments and spawn identity. Recipe
resources are addRef'd for snapshot lifetime and released on reuse/free.

Only tracked effects are excluded from live persistence and reconstructed.
Owner slots restore historical null or their exact reconstructed effect. Dead
occupied effects use the existing native retirement path rather than becoming
live emissions. Factory RNG side effects remain bracketed by the existing
reconstruction RNG restoration.

Custom subclass factories with non-null type arguments are not newly enabled:
their concrete tails were not fully audited. Such retained graphs are not
rebound to an unproven replacement identity. Complete custom-effect, indexed
auxiliary-effect, particle-history and allocation-generation reconstruction
remain outside these verified policies.

## Validation

Portable sanitizer tests cover scalar widths, corrected ownership spans,
auxiliary actor bounds/owners, indexed reference mappings and Shuma controller
binding checks. Mock-native lifecycle tests exercise the actual short-factory
capture/restore code, dead occupancy, resource retention, Storm control state,
exact shot-slot identities, saved null and owner-qualified category-9 lookup.
They do not emulate native animation, helper destruction/reallocation or effect
generator behavior. Native replay validation is still required.

Validation run: `ROLLBACK_CHARACTER_RESTORE_TEST=1 make -j8` passes (80 generated
ranges, 108 verified ranges, 66 preservation spans). Six targeted Python/native
harness tests pass across `test_character_tail_references`,
`test_rocket_effect_lifecycle`, `test_rocket_tail_policy`,
`test_restored_shot_pose`, and `test_effect_filter_skip`. `git diff --check`
passes. The broader `test_effect_attachment_skip` fails its validation-call
assertion on both HEAD and this worktree: its expected attachment guard is
already commented out in the hook. That unrelated behavior was not changed.

No blind copies were added for Hawkeye `6914`, Rocket `6928`, Hsien-Ko's leading
unknown tail bytes, Strange `693C/0C`, or Frank's configuration tables. Likewise,
the suspected Chris/Strider factory `mUsrF` phase mismatch remains unproven and
was not addressed by a speculative late write.
