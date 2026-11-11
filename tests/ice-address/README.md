# Numeric ICE address regression

```sh
python3 tests/ice-address/run.py
```

Compiles the real vendored `rtc::Candidate` and libjuice candidate/address code
against a simulated resolver that returns a PTR hostname even when numeric
output was requested. It makes no DNS requests, opens no sockets, and does not
contact Amazon.

The negative control demonstrates the original chain: a numeric remote
candidate resolves successfully in libdatachannel, gets serialized with a
hostname, and is accepted by libjuice with `resolved.len == 0`. Sending to that
empty destination is consistent with the Switch log's `EDESTADDRREQ` (121).
This is a controlled reproduction of the suspected conversion issue, not a
measurement of the actual Horizon resolver.

The fixed executable uses the same `--wrap=getnameinfo` link option as LunaNX.
`source/numeric_address.c` handles numeric-host/numeric-service queries with
`inet_ntop` and `ntohs`, bypassing the system name resolver. Requests for actual
names/services retain the original resolver. The tests verify:

- The real Candidate → libjuice chain retains the original IP and port and a
  nonzero destination length, while preserving candidate metadata.
- Public/reflexive candidate creation and address logging stay numeric too.
- IPv6 formatting/scope IDs, bounded buffers, truncated addresses, service-only
  requests, and named-lookup fallback.

The NRO additionally retains its resolved answer candidates, verifies their
addresses are numeric, and prints each validated remote address/port before
applying the answer. No ICE password, fingerprint or account token is printed
by this diagnostic.

The first real Luna create/poll/answer handoff succeeded on Josh's Switch.
Its media connection stopped at this ICE/address failure. Acceptance of the
patched path on the Switch still needs a new device run.
