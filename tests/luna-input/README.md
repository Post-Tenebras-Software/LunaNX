# Offline Luna input encoder checks

From the project root, run:

```sh
node tests/luna-input/run.cjs
```

This requires Node.js and a host C++17 compiler (`g++` by default; override with
`CXX`). It compiles the portable encoder and probe in a temporary directory,
compares their output with the committed references, and removes the temporary
build. It needs no Switch, network access, downloaded bundles, or new packages.
Use a host compiler rather than devkitA64 for this executable.

The checks cover 73 complete packets, 18 axis/trigger conversions, and rejection
of invalid normalized values, encoded axes/triggers, injection types, and
resiliency categories. Cases include empty nested messages, omitted zero scalars,
individual button masks, varint boundaries, signed sensitivity limits, timestamps,
and optional metadata. Packet references come from the original Luna generated
JavaScript writers and their matching Protobuf runtime. Conversion references
use the separately inspected browser formulas.

The same command also checks the browser decoder against all 73 packet
references, rejects malformed/truncated packets, and exercises the actual
controller sender using small libnx/RTC host stubs. These cover button mapping,
digital triggers, axis extremes, disconnect/reconnect, neutral shutdown,
backpressure, accepted-but-buffered sends, and recovery from send errors.
Face-button mapping follows Xbox positions (Switch B/A/Y/X → Xbox A/B/X/Y),
including after reconnect and backpressure. The browser display uses Switch labels.
L/Plus also forward Xbox LB/Start. Shortcut checks use simulated monotonic time
to verify one-second view and two-second exit holds, cancellation, exit priority,
one action per hold, release gating and disconnect/reconnect. Sender checks
verify neutral buttons/triggers/sticks during suppression and after backpressure,
then normal forwarding when suppression ends.
Browser handler checks use a minimal DOM and controlled clock to verify
timeout/close releases, invalid-message releases, and packet-number wrap.
These are host checks, not execution on a physical Switch or a full Chromium
WebRTC connection. The Switch build separately compiles against real libnx/RTC.

## Encoder boundary

[`source/luna_input.hpp`](../../source/luna_input.hpp) exposes three complete
`proto.dcv.input.ClientMessage` encoders:

- `encodeGamepad`: prepared controller state plus latency metadata and optional
  resiliency attributes.
- `encodeConnection`: controller connect/disconnect, with optional resiliency.
- `encodePointerModeAck`: the empty pointer-mode acknowledgement.

The encoder does not depend on libnx, WebRTC, or a generated Protobuf library.
It writes the small observed subset of the schema directly. Scalar zero values
are omitted; nested-message presence is preserved even for empty messages.
For example, controller 0 connecting with X360 injection encodes as `ca0600`.

Callers own controller sampling, button mapping, packet numbering, time
synchronization, and delivery. `convertAxis` expects normalized input with Y up;
its output is offset unsigned, with center 32767. `convertTrigger` expects 0–1.
Both reject nonfinite or out-of-range input rather than silently changing it.
`GamepadState` defaults to centered sticks. Input timing must use server-adjusted
epoch milliseconds; the encoder does not obtain or synchronize a clock.
The hardware enum names the currently relevant classes; it is not the full
upstream hardware-class enum.

The latency object always includes the message itself and populates fields 1
and 8 from the same timestamp and field 3 from the hardware class, matching the
observed gamepad builder. `ResiliencyAttributes::terminalIdentifier` corresponds
to the browser builder's `chainId` argument, which it writes to field 40.
Supplying metadata only encodes it; this does not implement the browser's WASM
resiliency algorithm or retry policy.

The Switch controller sender now uses the encoder in the local Chromium demo.
It keeps the existing reliable, ordered `lunanx-test` channel and uses the
Switch's local epoch clock. It does not supply application resiliency metadata.
This tests binary encoding and mapping; Luna's unordered `inputChannel`, time
synchronization, session negotiation, and server acceptance remain separate work.
See the [device test steps](../../tools/webrtc-test/README.md).

## Regenerating references

The archived public bundles live under the ignored
`tools/luna-inspect/bundles/` directory. With those files available, run:

```sh
node tests/luna-input/generate-reference.cjs
node tests/luna-input/run.cjs
```

The generator pins each bundle's URL and SHA-256 and refuses changed content.
It registers the archived webpack factories in a separate JavaScript context
without Node APIs, then loads only modules 65339 (Protobuf runtime), 75182
(imported network schema), and 87124 (input schema). It constructs test messages
and calls their actual `serializeBinary()` methods. Application startup modules
are never loaded. The VM context is a dependency isolation mechanism, not a
security sandbox for arbitrary untrusted code; source updates require review.

`reference.json` stores inputs, resulting bytes, and source provenance so normal
tests remain offline and independent of changing CDN assets. Review fixture
changes rather than regenerating them merely to make a failing encoder pass.
