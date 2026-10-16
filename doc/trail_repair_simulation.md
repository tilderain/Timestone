# Post-resim trail reset simulation

Offline experiment, not a runtime patch or native performance benchmark.
Source: `tools/simulate_trail_repair.cpp`.

## Native basis

Inspected `cParticleGenerator__moveParticleLineFollowOfs @ 0x140839200`
and `cParticleGenerator__calcParticleLineFollowOfs @ 0x14081B420` in Ghidra.
This is follow mode 0 selected by `moveParticleLineOfs @ 0x1408394E0`,
not a universal trail or particle layout.

For history count N at particle byte `+0x9D` and particle size S from
generator `+0x114`, the inspected path uses three Vec4 arrays:

- Derived endpoint bank 0: particle + S.
- Derived endpoint bank 1: particle + S + N * 16.
- Absolute history: particle + S + N * 32.

History shifts and receives the current particle position only when the owner
effect's substep counter is zero. The derivation subtracts the selected particle
position from absolute history. It writes one relative bank, optionally both.
Current/previous position selectors come from packed particle flags.

## Model and coverage

The standalone C++ model reproduces those scalar shift/subtraction operations.
It supplies emitter positions instead of emulating native attachment, particle
motion, or rendering. Display reconstruction linearly interpolates the two
anchored endpoint banks; this is an explicit model assumption, not an emulated
native shader. The zero-substep case derives offsets without shifting history,
to test a reset when ordinary history replacement has not occurred.

432 cases cover depths 1/4/8/12, history counts 1/8/32, zero/one/three substeps,
linear/curved/teleport paths, both starting endpoint banks and both update-old
flag values. Count zero is excluded because the inspected native shift
underflows for zero; this experiment is not a fix for malformed resources.

The proposed reset obtains one corrected attachment pose, puts both modeled
particle position endpoints at that pose, fills absolute history with that
pose, and regenerates both relative banks. It deliberately collapses the trail.

Checks verify all interpolation endpoints immediately match the corrected pose,
modeled particle-step counts are unchanged, subsequent samples never reconnect
to origin, and history eventually matches uninterrupted reference execution
after enough ordinary updates have replaced both banks and the history.

## Results

Run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie tools/simulate_trail_repair.cpp -o /tmp/opencode/simulate_trail_repair
/tmp/opencode/simulate_trail_repair
```

The non-PIE sanitizer build was used after the PIE sanitizer executable failed
with repeated `AddressSanitizer:DEADLYSIGNAL` messages in this environment.
All 432 modeled cases passed. Eight-frame linear example, eight history samples,
arbitrary world units:

| Strategy | Maximum modeled segment over interpolation endpoints/midpoint |
|---|---:|
| Skip attachments, then resume one normal update | 1123.52 |
| Reset absolute history and one derived bank, retaining stale other-bank data | 12.17 |
| Reset absolute history and both derived banks/position endpoints | 0.00 |

The complete reset discards 85.16 units of valid reference-trail extent in that
example. This is a visual restart, not equivalent historical reconstruction.
The partial-reset example starts from an existing reference trail to expose
stale old-bank offsets; it is not the same initial state as the origin example.

## Performance interpretation

For D hidden ticks, a one-time reset can replace D attachment updates with one.
The model performs 3*N+2 Vec3 assignments per particle during reset. A native
implementation of the corresponding Vec4 writes would touch approximately
16*(3*N+2) bytes per particle, excluding metadata/reads, plus attachment work
and tracking. At N=8, that is 416 bytes per particle. Larger populations can
make reset more expensive than the avoided attachment calls.

Break-even requires `(D-1) * attachmentCost > resetCost + trackingCost`.
No native timings are inferred from the host scalar model. At D=1 there is no
attachment-call saving and reset only adds work and visual loss.

## Runtime limitations

Only follow mode 0 was modeled. Cloth, shaped trails, world-space particles,
child creation/destruction, velocity/scale, parent death, owner callbacks,
camera transforms and lifecycle side effects are not covered. Each particle
needs its correct final spawn/attachment position, not blindly the parent's
origin. The reset must happen after corrected attachment/particle placement is
available and before any rendering of the affected buffers. It must not rerun
emission or age advancement merely to refresh presentation.

The existing attached-effect native-update fallback remains unchanged.
