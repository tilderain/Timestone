# Native Steam voice-chat traffic

Investigated 2026-09-13 in Ghidra, `umvc3.exe.unpacked.exe`.

## Conclusion

The native voice path does not unconditionally submit empty voice packets. It
polls Steam on an approximately 100 ms schedule while enabled, and submits only
successful, nonempty compressed capture results while transmission is enabled.
There is no application-side amplitude/voice-activity test in this path: if Steam
returns compressed data during microphone silence, that data can be transmitted.

**Deeper static tracing confirms that voice traffic can delay native gameplay
packets.** It shares the common protocol FIFO, the route send-rate allowance,
and (for fragmented voice messages) reliable ordering with gameplay messages.
A lost reliable voice end marker can prevent later input messages from being
delivered until sequence recovery. These are concrete code-level mechanisms,
not just hypothetical internet congestion. Static analysis does not establish
silent-capture sizes/frequency or which mechanism caused a particular observed
slowdown.

**Follow-up on "does it actually slow down": not established for normal silent
play or a particular match.** The conditions and their consequences are proven;
the runtime occurrence of those conditions is not. Do not present the previous
mechanism findings as a measured slowdown or as proof that silent voice traffic
is the root cause.

## Verified path

| Address | Behavior |
| --- | --- |
| `0x140792AD0` | Enabled voice update, receive callback binding for category 4, participant cleanup, calls `0x140792C50(this, 0, 1)`. |
| `0x140792C50` | Advances a millisecond deadline by 100, clamps missed deadlines to now + 100. Calls capture and playback update when due and talker state flags are nonzero. Handles recording start/stop state. |
| `0x1407930A0` | Steam user vtable `+0x38`: StartVoiceRecording. |
| `0x140793120` | Steam user vtable `+0x40`: StopVoiceRecording, clears audio buffers. Existing Ghidra name `resetWithSteamPipe` is misleading. |
| `0x1407937A0` | GetAvailableVoice (`+0x48`) must return 0/OK and nonzero available compressed bytes. GetVoice (`+0x50`) requests compressed data into a 1024-byte buffer; must return 0/OK and nonzero bytes written. `this+0x48` enables sending; `this+0x49` enables local monitoring. |
| `0x140793380` | Recipient/session filtering, then shared native send dispatcher with category 4. No audio-energy test. |
| `0x140793280` | Received voice callback, participant filtering and decode/mix dispatch. |
| `0x1407934B0` | Local playback mixing/output on a 200 ms deadline; zero-filled audio here is not evidence of network transmission. |

The capture success branch also sets the local participant talking flag `0x20`
and its expiry to now + 250 ms. This is driven by compressed capture data, not an
independent speech detector.

The send dispatcher at `0x140799800` previously had the unrelated name
`uModel__dispatchPerPlayerVisibility` and an incorrect uModel prototype. It has
now been corrected to `nNetwork__Session__sendToRecipients` with the existing
Session struct. Several transport functions similarly had incorrect Session
types; relevant ones are corrected below.

## Shared voice/input protocol: verified call paths

Session initialization (`nNetwork__Session_vfn5`, calls around `0x140799F67`)
constructs `ProtocolStandard` in slot 0 as `[P00]PROTOCOL_COMMON`, and another
instance in slot 3 as `[P03]PROTOCOL_SYSTEM`.

Voice constructor `0x140792050` initializes:

- `VoiceChat+0x34` base flags = 0.
- `VoiceChat+0x38` additional flags/priority = `0xC`.
- `VoiceChat+0x3C` session index = 0.
- `VoiceChat+0x4C` recipient selector = `0xC0`.

The capture call at `0x140793945` explicitly supplies `forceFlag10=0`.
`0x140793380` therefore normally sends flags `0x4C`, category 4, through session
slot 0. The low-level data payload is initially unreliable (`flags & 0x10 == 0`).

Gameplay input path:

```text
0x14028BC10 sNetPad__tickInputSender
  -> 0x14028A810 input message construction (including single-frame default case)
  -> aChrSelect__method_6D20 / MtPropertyList__publishField_4_or_16
       last argument 0 selects flags 0x10
  -> 0x1407725E0 / 0x1407725B0
  -> 0x1407915E0 message serialization
       session slot 0, flags |= 0x80, category 0
  -> 0x140799800 Session::sendToRecipients
```

Thus regular input messages use flags `0x90`, category 0. The above property/
character-select names are historical mislabels; the call sites visibly build
input messages from the input rings.

Both paths converge on:

```text
Session::sendToRecipients
  -> session+0x18 transport, virtual +0x38
  -> 0x14093CDA0 Transport::enqueueMessage
       protocol = transport->protocols[flags >> 16]
  -> protocol0 virtual +0x48
  -> 0x140941070 ProtocolStandard::enqueueMessage
       queue = protocol + 0x20 + peer*0x4A0
```

**Category 4 is not an independent transport channel.** Category 0 and category 4
share this FIFO and sequence state for the same peer. The category is a dispatch
tag in the record header, interpreted after sequencing.

## Mechanism 1: shared rate limiter and packet space (no loss required)

`0x14093E020`, corrected to `nNetwork__Transport__sendRoute`, checks the route's
signed send debt at `+0x45C`. It will send only when that value is less than 1.
Every outgoing assembled packet is charged through `0x14093A580`:

```cpp
// Route-relative offsets; B is the current bandwidth field at +0x43C.
debt = max(debt, -16 * B);
debt += packetBytes * 8192;
```

The elapsed-time update at `0x14093A8F0` subtracts:

```cpp
debt -= B * elapsedMs;
```

Voice consumes this same allowance as input. If its bytes leave debt positive,
the next send opportunity for gameplay waits for the debt to drain. This can
happen on a lossless connection with ample real-world bandwidth because it is
the game's own configured/adaptive limiter. For scale, at `B=65536`, another
400 accounted bytes adds 50 ms of debt; existing credit, concurrent input bytes,
bandwidth changes, and tick timing determine actual extra waiting. This is not
a claim of a fixed 50 ms penalty or of the runtime's current bandwidth.

`0x1409400F0` also scans the protocol FIFO in insertion order. It breaks when the
next selected record does not fit the remaining packet space, rather than
searching ahead for a smaller input record. Fragmented voice records have header
bit `0x40`, which bypasses the optional unreliable-drop decision. The priority
bucket scheme therefore does not isolate gameplay from large voice payloads.

## Mechanism 2: reliable voice end marker blocks later input after loss

`0x140941070` splits messages larger than `0xC0` (192 bytes) into fragments.
Irrespective of the original reliable flag, it then appends a size/end marker:

```cpp
if (originalSize > 192) {
    header.flags = category | 0x30; // includes RELIABLE bit 0x10
    header.length = 0xFE;
    append(header, bigEndian32(originalSize));
    ++queue.reliableSequenceEnd;
}
```

Verified in assembly at `0x1409412DD..0x14094136D`, including the unconditional
`OR CL,0x30` at `0x1409412FC`. For voice the marker's header byte is `0x34`.
The sequence state is per protocol/peer, not per message category.

`0x140941A20`, corrected to `ProtocolStandard::decodeFrame`, admits a record only
when its incoming sequence matches `lastReceivedReliable + 1`. The assembly at
`0x140941CAC..0x140941CB9` compares against queue `+0x446` and branches to payload
skipping on a mismatch. Only after this guard can the record reach category
dispatch. Accepted reliable records advance that receive sequence.

Consequently this is a valid execution of the native protocol:

```text
Receiver has accepted reliable sequence 40.
Voice end marker 41 is transmitted, but its packet is lost.
Later gameplay input 42 arrives while 41 has not been recovered.
Decoder expects 41, skips 42, and does not dispatch the input message.
Reliable retry/sequence recovery must fill the gap before later inputs deliver.
```

The retry/ACK machinery is in `encodeFrame`/`decodeFrame`; retry timing depends
on route state and redundancy. A later packet that also carries a successful
retry of 41 can recover immediately. This is conditional head-of-line blocking,
not a wait after every voice packet.

Important distinction: **losing only unreliable audio fragments is insufficient
to establish this stall.** If the reliable marker arrives, the receive sequence
advances even when voice reassembly fails. The problematic loss is the reliable
marker (or its continued delay), which gates later gameplay records.

## Outer Steam transport and receive processing

`MtNetObject__vfn_2_12` (send call around `0x14050A103`) invokes
`ISteamNetworking::SendP2PPacket(peer, data, size, 0, 0)`: unreliable mode, Steam
channel 0. Native reliability above is implemented by the game; it is not
Steam's reliable-send mode. There is no explicit ACK wait/retry loop in this
outer send wrapper, though it holds the object's enabled critical section.

`MtNetObject__vfn_2_10` (`0x140509A60`) drains channel 0 using
`IsP2PPacketAvailable` / `ReadP2PPacket` in a loop with no explicit per-update
packet-count budget. It holds the same object's enabled critical section while
dispatching received packets. Default MtNetObject construction enables this lock.

Receive dispatch continues through:

```text
0x14050C5D0 -> Connect_mi_vfn5 (0x1409381E0)
  -> 0x14093D8C0 -> Transport_vfn8 (0x14093D430)
  -> 0x14093D8E0 -> ProtocolStandard::decodeFrame
  -> 0x140797FF0 -> 0x140916C60 category dispatch
  -> voice callback 0x140793280 -> 0x140792F00
```

The voice subscriber is bound with deferred-dispatch flag 0 in `0x140792AD0`,
so voice decompression/mixing runs inline in receive dispatch.
`0x140792F00` calls Steam `DecompressVoice` at 11025 Hz into a `0x5622`-byte
buffer, then adds decoded samples into the voice mix buffer. It does not skip
this work at volume zero. Subsequent packet processing must wait for this call
to finish. This establishes shared processing/lock scope, but does not quantify
its cost or demonstrate that it dominates frame time.

Capture and playback servicing also runs in `sNetwork::move` (`0x1406864D0`).
The 100 ms capture timer and 200 ms playback timer schedule work; neither is an
explicit sleep/ACK wait imposed on each gameplay packet.

## Volume is not transmission mute

`sGameConfig__setVoiceChatVolume` (`0x140255180`) calls `0x140458F30`, which
reaches `0x140793A10`. This changes playback gain for sound channel `0x12345678`
and stores the gain at `VoiceChat+0xC4`. It does not clear transmit enable or
stop Steam recording. Setting voice-chat volume to zero is therefore not a
valid test of disabling outbound voice traffic.

## Follow-up measurement

### Additional static tracing: normal activation and defaults

The code is reachable from normal game initialization:

- `sMvc3Main__sMvc3Main`, call at `0x1402579B4`, obtains sNetwork and invokes
  virtual `+0x80`.
- This reaches `sNetwork_vfn16` (`0x140685A20`), constructs VoiceChat if absent,
  and initializes it through `0x140792A50`.
- Online UI state transitions call `sMvc3Guide__setupMsgHandler`
  (`0x140458ED0`), for example at `0x14025EC16` in the phase-4 branch of
  `sMvc3SceneUI__updateMatchPhaseState`.
- The helper calls `0x140458B90`, which sets the local talker transmit/init byte
  to 1 and loopback byte to 0. `0x140792C50` subsequently starts Steam recording.
- Online teardown explicitly closes the talker (`nNet__sSession::move`, state
  6). There is no basis for saying recording is enabled in every game state.
- The main-loop call through `nNet__sComponent::move` reaches `sNetwork::move`;
  capture/playback servicing is part of that normal main-thread update.

`0x140939670`, corrected from `nDraw__PassSlot__resetState` to
`nNetwork__Route__resetState`, initializes bandwidth `+0x43C` to 65536, packet
capacity `+0x44C` to 512, and send debt `+0x45C` to zero. Transport constructor
`0x14093BF30` initializes `mFastTransfer=0`, bandwidth min/max/start/target to
8192 / 2097152 / 65536 / 16384, and interval min/max to 32 / 512.

These are constructor/reset defaults, not established live-match constants.
`0x14093E890`, now `Transport::adjustRouteBandwidth`, reduces the route rate in
20% stages under adverse measured conditions and permits 12.5% increases under
favorable conditions, with bounds and cooldowns. Other configuration writes have
not been exhaustively excluded.

Charging voice bytes also does **not** automatically create extra waiting. As a
counterexample in the established limiter arithmetic, with bandwidth 65536 and
the full 16 ms credit available, an outgoing charge of 128 bytes changes debt
from `-16*65536` to zero. The debt guard still permits sending. Larger charges
may similarly be paid off before another otherwise-scheduled send. Consequently
packet byte accounting alone cannot prove a reduction in gameplay speed.

### Static boundary: silent capture output is external

`Steam::createAllInterfaces` (`0x140212E30`) requests `SteamClient017`, then
obtains `SteamUser019` and stores that interface at `0x140D50538`. Voice capture
and decompression call this externally supplied interface's virtual methods.

The capture size used by `0x140793380` comes from `GetVoice`'s output parameter;
it is not a constant chosen by UMVC3. In particular, the 1024-byte stack buffer
is a **maximum capacity**, not evidence of a 1024-byte packet every 100 ms.
The executable does not supply the implementation that decides how many bytes
silence/background noise produces. It therefore cannot establish that silent
capture exceeds the 192-byte fragmentation threshold, exhausts the rate budget,
or produces any outgoing packet at all on a given system.

The loss of a voice marker and the amount of queued gameplay input are also
runtime conditions. A protocol delivery delay need not become a visible gameplay
stall if input is delivered before it is needed. Likewise, inline audio work
has real CPU cost but its execution time relative to frame-budget slack is not
recoverable from these call sites.

Thus the strongest static result remains: **the shipped voice path is reachable
and can interfere with input delivery through the verified mechanisms. Actual
silent-play slowdown and its magnitude are unproven.**

### Measurements that would close the remaining gap

The static mechanisms above are established. To attribute a particular slowdown,
instrument capture return codes, available bytes, bytes written, transmit enable,
and calls/bytes at `0x140793380`, alongside frame time and native input wait time.
Record the fraction of voice payloads exceeding 192 bytes, route `+0x45C` debt,
and common-protocol sequence gaps involving category-4 `0xFE` markers.
Compare speech, microphone silence, disconnected microphone, and transmission
disabled. For causality, compare an otherwise identical session with just voice
submission suppressed; measure capture/playback costs separately. One successful
poll submits at most 1024 compressed bytes before routing/fanout and transport
overhead, normally at approximately 10 polls/second.

This concerns retail Steam/CMU networking. As documented in `nativenet.txt`, the
plugin's ENet rollback transport is separate; activity of this native subsystem
during plugin sessions needs independent verification.

Applied existing VoiceChat, Session, Transport, ProtocolStandard, and Route
structs to the relevant Ghidra prototypes. Renamed the capture/send, protocol
enqueue/decode, transport enqueue/tick/sendRoute, and route accounting functions;
annotated the sequencing and shared-budget findings at their entry points.
