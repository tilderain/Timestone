# Time sync (frame advantage)

Written 2026-11-18. Built and simulated; **not yet run between two PCs**.

## The problem

A game that runs ahead of the other receives every remote input late and rolls
back for it; the game behind receives input early and never rolls back. That is
the one-sided rollback. Nothing brought the two back together: in fixed frame
timing `sMain::move` (`0x140521DF0`) carries a late frame over into the next
one, but anything longer than a frame is dropped (`mRealCounter` is clamped to
`mIdealCounter`), and so is every wait for input or rollback budget. Each such
hitch stayed as a permanent offset between the two games.

The old estimate (`g_localFrameAdvantage`) could not be used to fix it:

- it took the newest *input* frame received as the peer's frame, which is the
  peer's frame plus its input delay;
- it added the full round trip instead of one way;
- it was sampled once a frame on whole frames, so it jittered by a frame;
- the wait it worked out was passed to `Sleep` as milliseconds, and commented
  out.

## Measurement

Every input packet is stamped as it leaves (`netplay.cpp`, service thread, so
resends are stamped afresh) with the sender's frame clock: the frame being
played and how far into it the game is, in 1/64 frame. A game waiting for input
stays at the end of its frame. The stamp lives in `InputPacket::flags`
(`INPUT_FLAG_CLOCK`, 7 bits of phase, 23 bits of frame); the packet's size is
unchanged, and a peer that does not set the flag is simply not synchronised
with.

The service thread also records when each packet arrives. The receiver compares
the sender's clock with its own at that instant (`SampleTimeSync`). Smoothed,
that is its *lead*: how far its clock is past the frames it is being sent,
clock offset plus network delay. Each side sends its lead in
`InputPacket::frame_advantage` (1/256 frame).

    frame advantage = (local lead - remote lead) / 2

The network delay is in both leads and cancels, so no ping estimate is used at
all. With an uneven route (one direction slower) the advantage is zero where
remote input reaches both games equally late, not where the clocks are equal;
that is the point that makes both roll back the same amount.

## Correction

The side with the advantage lengthens its frames (`hkMainMove`,
`rollback_hooks.cpp`): before the native pacing call it moves
`sMain::mIdealCounter` (`+0x68`) on by a fraction of a frame. In fixed frame
timing (`mFrameWait`, `+0x58`) `sMain::move` sleeps until `mRealCounter`
reaches it, and one frame is one second's worth of counter ticks on both, so
the frame is longer by exactly that fraction. No frame is held or dropped.

    stretch = clamp((advantage - 0.25) * 0.012, 0, 0.05)

That is at most 5% (0.8 ms on a frame), and under 1% for an advantage of one
frame. A four-frame offset is gone in about three seconds. The side behind
does nothing. With no packets for half a second there is no correction.

Other levers in `sMain` were left alone. `mGlobalSpeed` scales the simulation
delta, which the mod forces to 1.0 anyway. `mFps` would slow the pacing just as
smoothly, but it also feeds the frame timer and its other readers were not
audited. The ideal counter was only seen used by the pacing loop itself and by
`sMain::setFrameWait` (`0x140522420`), which resets it.

With VSync on the display paces the game, so the stretch shows up as an
occasional repeated frame instead of a longer one.

## Display

Expanded PC PERFORMANCE window: `Frame advantage +0.42 f`, with `ahead,
slowing 0.2%`, `ahead`, `behind, opp. slows` or `in sync`.
