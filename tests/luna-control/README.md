# Native control codec and clock checks

From the project root:

```sh
node tests/luna-control/run.cjs
```

Requires Node.js and a host C++17 compiler (`g++`, or override `CXX`). The runner
compiles in a temporary directory and removes the build afterward. No Switch,
network, downloaded bundles, or third-party C++ library is needed. The root
Switch target compiles the codec, synchronizer, and local session owner against
devkitA64. The local WebRTC demo now invokes them on `clientMessageChannel`.

The checks compare native encoding and decoded values with all ten committed
original-writer examples in
[control-reference.json](../../tools/luna-inspect/control-reference.json).
They additionally cover full signed int64 timestamps, uint64 negotiation IDs,
UTF-8, empty nested messages/repeated strings, unknown fields, oneof ordering,
wire/type/size/count limits, prefix truncation, and close/idle replies.

A synthetic peer supplies a framed original-writer ping response. Client send
1000, server receive 1110, server send 1120, and client receive 1030 must produce
**+100 ms offset / 20 ms delay**. Controlled-clock checks also cover half-ms and
negative offsets, burst spacing, maintenance, stale samples, late polling,
cancellation, replay/unmatched/expired replies, invalid timestamps, clock jumps,
and stop/restart. No test sleeps or uses the machine's wall clock.

The runner also compiles the local session owner and feeds its actual startup
bytes to the browser peer implementation. The browser's framed replies are then
fed back into the C++ owner. Tests check startup order, display echo, known clock
anchor, close ACK and bounded fallbacks, send retries, frame/count/byte mailbox
bounds, callbacks after stop, and real page handler routing with control arriving
before the controller channel. The UI check uses a minimal DOM and fake peer;
these are host simulations, not an executed Switch/Chromium WebRTC connection.
Physical test instructions are in [the webpage notes](../../tools/webrtc-test/README.md).

## Codec API

[`luna_control.hpp`](../../source/luna_control.hpp) is independent of libnx and
WebRTC. It encodes complete, bare `BrowserClientMessage` payloads:

- `encodeFeatureConfiguration`, `encodeDisplayConfiguration`, `encodeClientContext`.
- `encodePing` for requests and synthetic responses.
- `encodeCloseAck`, `encodeResetIdleTimeout`.

Display struct defaults are Protobuf zeros/absence, not an automatic capability
declaration. The future startup caller must select negotiation ID, display,
codecs, bitrate, and features consistent with its SDP. `encodeClientContext`
always attaches metadata, even with six empty strings, matching the observed
builder. Empty repeated-string entries are retained; scalar empty strings are
omitted. `std::optional` distinguishes absent from present-empty nested messages.

`decodePayload` accepts bare bytes for tests. Incoming traffic should use
`decodeServerFrame`: it requires the 16-byte prefix, reads the little-endian
payload length at bytes 8–11, and checks the length before decoding. Other prefix
bytes are opaque; bounded trailing bytes are ignored as in the browser handler.
Outgoing messages never acquire that incoming prefix.

`Message` contains a profiling identifier and a `std::variant` body. Dispatch
with `std::get_if<Ping>`, `std::get_if<CloseConnectionRequest>`, etc. Recognized
envelope members outside the startup subset become `UnhandledMessage` with a
field number and raw body; wholly unknown fields are skipped. The last recognized
oneof member wins. Empty/profiling-only payloads can have a `monostate` body.
Decoding a display reply does not declare the game ready.

Native limits: **65,536 bytes per payload**, **65,552 per frame**, **4,096 bytes
per string**, **128 items per repeated list**. Supported strings require valid
UTF-8. Bad lengths/varints/wire types and limit violations throw
`std::invalid_argument`. Unknown nonnegative enum values fitting int32 are
retained; negative enums and deprecated Protobuf groups are unsupported. Opaque
unhandled bodies are not recursively validated as supported schemas. These
are implementation choices, not discovered server restrictions.

## Clock API and ownership

[`luna_time_sync.hpp`](../../source/luna_time_sync.hpp) takes a clock function
returning `{epochMs, steadyMs}`. Epoch time labels packets; steady time drives
scheduling/expiry. All methods belong to one thread. Dispatch received pings to
that owner before calling `handlePing`; there is no internal thread locking.

An eventual owner can use this shape:

```cpp
luna::control::ClockSynchronizer sync(readEpochAndSteadyMilliseconds);
sync.start(); // when the control channel opens

if (auto ping = sync.nextPing()) {
    try {
        sendControlBytes(luna::control::encodePing(*ping));
    } catch (...) {
        sync.cancelPing(ping->clientSendTimestampMs);
        throw;
    }
}

// After decoding an incoming frame on the owning thread:
if (auto ping = std::get_if<luna::control::Ping>(&message.body))
    sync.handlePing(*ping);

auto timestampMs = sync.serverTimeMs(); // future Luna input latency
sync.stop(); // control-channel close / session shutdown
```

`nextPing` records its returned request as pending. Cancel it if transport send
throws; an accepted-but-buffered send still counts as accepted. Poll only while
the channel is open. The synchronizer owns no sockets or background timer
threads. `stop` clears pending requests, schedule, and offset; subsequent polling
and reply handling do no work.

Observed browser policy retained:

- Eight initial pings, one second apart; maintenance every 60 seconds.
- Positive network delay only, initially at most 999 ms; keep a better or equal
  low-delay sample until stale after 25 minutes.
- Apply the observed offset formula. Store twice the offset as an integer to
  preserve half milliseconds; adjusted epoch time truncates toward zero.

Native scheduling and validation choices:

- Retain one best sample and at most eight pending requests. Replies must echo
  a pending timestamp, arrive within ten steady-clock seconds, contain positive
  server times, and have non-reversed receive/send times. Consumed replies cannot
  replay; worse/rejected samples leave the offset unchanged.
- Late polling emits at most one ping and paces the remaining burst from the
  actual poll time, avoiding catch-up floods.
- A stale sample, backward epoch/steady clock, or epoch/steady divergence over
  250 ms clears offset/pending state and starts a fresh paced burst. Smaller
  clock adjustments are tolerated.
- Clock readings/server timestamps fit the browser's exact integer range,
  0–9,007,199,254,740,991; server timestamps must be positive. Wire timestamp
  decoding itself supports all signed int64 values.
- Without an accepted sample, `serverTimeMs` returns local epoch time and
  `sample()` is empty. The future session owner decides when synchronized time
  is required; this helper does not declare a session ready.

Full source observations and remaining gaps are in
[control-startup.md](../../tools/luna-inspect/control-startup.md).

## Local integration

[`LocalControlSession`](../../source/local_control_session.hpp) owns startup and
clock state on the main thread. Network callbacks only write a mutex-protected,
bounded mailbox. Main-loop updates decode replies and send at most one control
message per update; buffered transport defers outgoing work. Recognized close
requests stop clock work, send a close ACK, and request main-thread peer cleanup
after the browser closes or a bounded fallback expires. `stop()` is terminal;
each new WebRTC test creates a new owner.

The browser peer anchors server receive time to client-send time +1000 ms and
adds its processing duration to server-send time. Thus offset + delay/2 must
equal 1000 ms, including native mailbox-dispatch delay. This is a deterministic
protocol check, not a claim about absolute server time. It echoes the display
request rather than negotiating actual Luna capabilities.

The existing streaming/controller channel and input timestamps remain unchanged.
There is no authenticated Luna connection or live server-acceptance result.
