# Direct ENet pump experiment

The current experiment follows GUXT's default single-threaded receive design:

```
live game boundary / input wait
  -> enet_host_service(timeout = 0, at most 64 events)
  -> validate
  -> feature receiver
  -> enet_packet_destroy
```

`Controller_ReceiveNetplayInput` writes confirmed history directly.
`AutoTest_ReceiveNetplayFrame` writes the existing suite report history directly.
Other messages dispatch into feature-owned settings or actions. Restart, desync,
snapdump, and suite-exit actions received inside an input wait are applied at the
next live move boundary. There are no application transport queues, per-tag
indexes, packet pools, or packet handoff allocations. ENet still allocates and
buffers its own packets normally.

## Ownership and scheduling

- The first live `Netplay_Pump` binds its game-thread owner. Other threads cannot
  pump, and nested pump calls from a receiver are ignored.
- Input waits explicitly pump. Paused moves pump before the skip/present branch.
- Re-simulation does not pump: control callbacks must not alter live state
  halfway through a restore/replay.
- UI/render sends and shutdown serialize ENet access with one SRW lock.
  Callbacks execute after releasing it, so a receiver can send a response.
- Sends are immediate and flushed; no worker scheduling delay is introduced.
- The watchdog remains independent, polling at 100 ms. Moving it onto the game
  thread would make it unable to diagnose that thread freezing.
- Steam's independent transport bridge remains unchanged.
- RTT-based ENet packet-throttle deceleration is disabled for this fixed-rate
  input stream. The fast launch handshake followed by frame-paced ACKs can
  otherwise drive the throttle to zero until its 5-second baseline refresh.
  ENet bandwidth limits and reliable delivery are still respected.

## Measurement and validation

The first local A/B benchmark used the saved pooled mailbox version and the
same controller receiver for both paths, 200,000 packets per run, three runs:

| Receive-side work | Pooled handoff | Direct dispatch |
|---|---:|---:|
| Validation + handoff (if any) + controller history update | 109–116 ns/input | 52–54 ns/input |
| Reserved application payload pool | 32 MiB | None |

These are Linux native microbenchmarks with mocked Win32 locks, ENet calls, and
game-side dependencies. They exclude socket service, scheduling, native rollback
work, and real logging. They do **not** establish improved Windows/Wine frame
times or end-to-end input latency. Immediate flushing may also trade batching
throughput for responsiveness; it needs in-game measurement.

`python3 tools/test_netplay_direct.py` exercises production dispatch and the
controller receiver against mocks, plus a real vendored-ENet UDP loopback test.
For sanitizer runs:

```
NETPLAY_TEST_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie' python3 tools/test_netplay_direct.py
```

The ENet regression test also models a 1 ms launch baseline followed by 17 ms
ACK samples: default throttle falls from 32 to 0 after 16 samples, while the
configured peer stays at its bandwidth-allowed limit across 300 delayed samples.
This reproduces input suppression, not the live game's reported FPS dip.
Periodic network logs now include input counts, RTT and throttle; deliberate
time-sync sleeps are logged at most once per second to distinguish the causes.

### Startup timesync regression

The supplied host/client logs showed full input throttle (`32/32`) but different
startup-contaminated RTTs: about 645 ms on the host versus 312 ms on the client.
The old pacing formula added each local RTT in frames before comparing the two
advertised offsets. Those RTTs alone introduced a 20-frame difference, accounting
for about 10 ms of the client's logged 11 ms sleep at frame 60.

Timesync now advertises and compares `remote input tail - local frame` directly.
Common transit lag/input delay cancels in that comparison; independently smoothed
RTTs are not pacing inputs. The existing 40-frame averaging and real-lead pacing
remain. A production-code regression test checks 600 frames with stale RTTs,
different common offsets, and actual leads in both directions.

Restart both peers on the updated build: the packet layout is unchanged, but
`frame_advantage` now carries the raw offset rather than an RTT-adjusted value.
The RTT display may still settle slowly; it no longer creates timesync sleeps.

## Remaining live checks

Deferred remote actions interrupt both confirmed-input and prediction-budget
waits. The move hook presents without advancing the incomplete gameplay frame,
then applies the action at the next live boundary. Desync capture retains the
first pending trigger separately from the final pause/unpause state, so a trigger
followed by unpause in one pump does not lose the capture notification.
`python3 tools/test_netplay_remote_actions.py` tests the production receiver,
wait loops, boundary guard and action application with game dependencies stubbed.

- Two-peer startup, initial settings, opening-member handshake, and gameplay.
- Delay-only waits and rollback-budget exhaustion recover when input arrives.
- Pause/desync dismissal, full/training restart, and roster changes.
- Autotest start/report/completion ordering and long resimulation bursts.
- Direct UDP and Steam connections; leave/reconnect/exit behavior.
- Loading gaps: native blocking loads cannot service ENet. Startup and roster/
  match restart use a 60-second timeout until the fighting handshake restores
  the normal timeout. Stalls longer than that remain a known limitation.

User-provided live logs identified the startup timesync regression above. The
updated pacing fix still needs a live two-peer confirmation.
