# Provisioning state machine and fake API

Run from the project root:

```sh
node tests/luna-session/run.cjs
```

Requires Node.js and a host C++17 compiler (`g++`, or set `CXX`). Builds with
warnings treated as errors in a temporary directory, runs the checks and a
short fake launch, then removes the executables. No network, credentials,
downloaded Amazon code, browser, or Switch is needed.

On a host with the undefined-behavior sanitizer runtime installed, optionally run
`LUNANX_SANITIZE=1 node tests/luna-session/run.cjs`. The normal checks do not need
that runtime.

The fake launch prints a create at 0 ms, polls at 200/600/900 ms, and one answer
handoff to the original fake peer. This exercises the actual native state
machine; it does not perform HTTP, parse an SDP, or execute WebRTC.

Checks cover all five active job states; server-directed delays and fallback;
single-flight requests; queue updates; one answer handoff; API/transport errors;
wrong job IDs and response types; bounded payloads; request/overall deadlines;
clock regression and arithmetic overflow; cancellation before dispatch, during
create and during polling; failed/unconfirmed cancellation; and allocation
racing cancellation. Stale and duplicate results cannot revive a terminal owner.

## Reading the implementation

[`luna_session.hpp`](../../source/luna_session.hpp) defines the transport boundary.
[`luna_session.cpp`](../../source/luna_session.cpp) owns the lifecycle and timing.
Start with `nextRequest()`, then follow `complete()` to `created()` / `polled()`.
The [demo](demo.cpp) shows a complete caller without asynchronous plumbing.

Each `ProvisioningSession` is one attempt, owned by one thread. Copying and moving
are disabled so another owner cannot accidentally dispatch the same create. It owns no
sockets, workers, timer callbacks, authentication, JSON parser, or RTC peer.
The future transport marshals completions back to this owner. Keep the existing
peer alive from offer creation through answer application; create a new owner
for a new attempt. Never dispatch an old owner's callback to a replacement.

1. `start(CreateJob{bodyJson})` retains the future authenticated API's complete
   create body. Contents are opaque here; the adapter must validate JSON and
   supply correct authentication, play token, metadata, and offer.
2. Call `nextRequest()` on each owner tick. It emits one typed request with a
   correlation ID and considers it dispatched immediately. Do not call it just
   to inspect readiness and then discard the request.
3. Feed decoded replies to `complete(id, response)`, or call
   `transportFailed(id)`. A matching completion is consumed once. Matching but
   malformed replies fail the attempt; stale/unknown/expired IDs return false.
4. On `Ready`, call `takeAnswer()` once. This transfers the allocated session's
   IDs and original `signalingResponse` JSON string to the caller. The caller
   must parse/validate type/SDP and apply it to the peer that supplied the offer.
   If parsing or RTC application fails, the caller owns remote-session cleanup.
   `Ready` means allocation answered, not that ICE/media or gameplay works.

Native scheduling choices, separate from source observations:

- Monotonic, nonnegative milliseconds from a nonthrowing clock function;
  backward time fails an active attempt. Deadlines use saturating addition.
- Default request timeout 10 seconds, whole-attempt timeout 10 minutes, and
  cancellation window 20 seconds. All are configurable. The cancellation window
  covers waiting for a pending request and sending cancellation, independent of
  the original whole-attempt deadline once cancellation has begun.
- Missing first delay defaults to 1500 ms; explicit first zero is immediate.
  Missing/zero subsequent delay defaults to 1500 ms, matching the browser.
  Negative or greater-than-60-second delays fail rather than polling early;
  the maximum is configurable. Late owner ticks never create catch-up floods.
- Create JSON and answer JSON strings are limited to 65,536 bytes each;
  nonempty identifiers to 512 bytes without control characters. These are local
  bounds, not discovered Luna limits. Future HTTP parsing needs its own limits.
- Unknown statuses, inconsistent active responses carrying an answer, invalid
  queue data, or mismatched job IDs fail. No automatic transport/create retry.

## Cancellation and ownership

`cancel()` before any create is dispatched stops locally with no API request.
After dispatch, it suppresses answer delivery and waits within the cancellation
window for an in-flight create/poll. A successful create supplies the ID needed
to emit `CancelJob`. A poll failure/timeout during cancellation can still lead
to cancellation of the known job. The transport may abort a pending poll and
report `transportFailed()` to allow cancellation sooner. It must never report
an aborted create as proof that no job was created.

Lost create responses are terminal and ambiguous: there is no known job ID to
cancel, and replaying create could allocate a second game. A failed/timed-out
cancel remains unconfirmed. Repeated `cancel()` calls never extend its deadline.
`remoteWorkMayRemain` records possible/known remote work owned by this object;
it is cleared when a complete answer transfers ownership to the caller.

`CancelledJob` is a semantic adapter response, **not** a discovered Amazon JSON
schema. Successful acknowledgement sets `cancellationConfirmed` for the job
request. Its `jobCancelledBeforeAllocation` flag defaults false: an adapter may
only set it when it can establish that no game was allocated. Otherwise
`remoteWorkMayRemain` stays true. The fake API can establish this locally; an
eventual Luna adapter must verify its cancellation semantics first.

If allocation succeeds during cancellation, no answer reaches the peer.
`allocatedDuringCancellation` retains session/connection IDs for explicit cleanup,
and `remoteWorkMayRemain` stays true even after a cancellation acknowledgement.
Cancelling a ready/terminal owner returns false; there is no implicit game
termination, takeover, retry, or restart. Terminal attempts do not send more
requests. Transport abortion and remote cleanup belong to the eventual caller.

The Switch main loop now invokes this owner for the optional ZR local HTTP job
test. The [HTTP checks](../luna-session-http/README.md) exercise the actual curl
transport and Python mock routes; [device steps](../../tools/webrtc-test/README.md)
cover applying its returned answer to the existing WebRTC peer. Authenticated
Luna HTTP, browser-companion login, verified Luna cancellation semantics, and
live Luna validation remain future integration work. See the
[source map](../../tools/luna-inspect/session-auth.md) for the evidence boundary.
