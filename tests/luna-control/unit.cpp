#include "luna_control.hpp"
#include "luna_time_sync.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace control = luna::control;
void check(bool condition, const char* message);
control::Bytes fromHex(const std::string& hex);

namespace
{
template <typename F> void rejected(F action)
{
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Malformed control input was accepted");
}
void varint(control::Bytes& bytes, std::uint64_t value)
{
    while (value >= 128)
    {
        bytes.push_back(static_cast<std::uint8_t>((value & 127) | 128));
        value >>= 7;
    }
    bytes.push_back(static_cast<std::uint8_t>(value));
}
control::Bytes wrap(unsigned field, const control::Bytes& body)
{
    control::Bytes result;
    varint(result, (static_cast<std::uint64_t>(field) << 3) | 2);
    varint(result, body.size());
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
control::Bytes framed(const control::Bytes& payload)
{
    control::Bytes result(16, 0xa5); // Opaque prefix bytes deliberately nonzero.
    for (unsigned i = 0; i < 4; ++i)
        result[8 + i] = static_cast<std::uint8_t>(payload.size() >> (8 * i));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
void checkCodecEdges()
{
    check(std::holds_alternative<std::monostate>(control::decodePayload(nullptr, 0).body), "Empty proto");
    rejected([] { control::decodePayload(nullptr, 1); });
    rejected([] { control::decodeServerFrame(nullptr, 16); });
    const auto response = fromHex("a2010908e80710d60818e008");
    auto frame = framed(response);
    const auto ping = std::get<control::Ping>(control::decodeServerFrame(frame).body);
    check(ping.clientSendTimestampMs == 1000 && ping.serverReceiveTimestampMs == 1110 &&
        ping.serverSendTimestampMs == 1120, "Framed original-writer ping");
    frame.push_back(0x99);
    check(std::holds_alternative<control::Ping>(control::decodeServerFrame(frame).body), "Opaque trailer");
    for (std::size_t size = 0; size < 16; ++size)
        rejected([&] { control::decodeServerFrame(frame.data(), size); });
    for (std::size_t size = 1; size < response.size(); ++size)
        rejected([&] { control::decodePayload(response.data(), size); });
    frame[8] = 0xff;
    rejected([&] { control::decodeServerFrame(frame); });
    frame.assign(control::MaxPayloadBytes + 17, 0);
    rejected([&] { control::decodeServerFrame(frame); });
    control::Bytes tooLarge(control::MaxPayloadBytes + 1, 0);
    rejected([&] { control::decodePayload(tooLarge); });
    for (const auto& hex : {"00", "a201", "a00100", "a201020a00", "1200", "a2010100",
        "0b", "0f", "a2010b08ffffffffffffffffff02", "5a06088080808010"})
        rejected([&] { control::decodePayload(fromHex(hex)); });

    // Unknown fields survive old-schema parsing without corrupting a supported event.
    auto withUnknown = fromHex("10ac02a2010908e80710d60818e008980601c10c0000000000000000c50c00000000");
    auto message = control::decodePayload(withUnknown);
    check(message.profilingMarkerIdentifier == 300 && std::holds_alternative<control::Ping>(message.body),
        "Profiling marker and skipped unknown fields");
    withUnknown.push_back(0x6a); withUnknown.push_back(0);
    message = control::decodePayload(withUnknown);
    check(std::holds_alternative<control::AckCloseConnectionRequest>(message.body), "Last oneof wins");
    const auto unhandled = std::get<control::UnhandledMessage>(
        control::decodePayload(fromHex("e20301ff")).body);
    check(unhandled.field == 60 && unhandled.payload == control::Bytes{0xff}, "Unhandled participant payload");

    const auto close = std::get<control::CloseConnectionRequest>(
        control::decodeServerFrame(framed(fromHex("520a0a047465737410071803"))).body);
    check(close.sessionId == "test" && close.connectionId == 7 && close.closingReason == 3,
        "Close request fields");
    const auto idle = std::get<control::IdleTimeoutWarning>(
        control::decodeServerFrame(framed(fromHex("5a0408071002"))).body);
    check(idle.connectionId == 7 && idle.warnTime == 2, "Idle warning fields");
    check(control::encodeCloseAck() == fromHex("6a00"), "Close ACK bytes");

    const auto empty = std::get<control::DisplayConfiguration>(
        control::decodePayload(fromHex("c20200")).body);
    check(!empty.display && !empty.captureResolution && empty.negotiationId == 0, "Proto defaults");
    check(!std::get<control::ClientContext>(control::decodePayload(fromHex("d20200")).body).browserMetadata,
        "Missing browser metadata distinct from present-empty");
    control::DisplayConfiguration display;
    display.captureResolution = control::Resolution{};
    display.display = control::Display{};
    check(control::encodeDisplayConfiguration(display) == fromHex("c2020412001a00"), "Nested presence");
    display.negotiationId = std::numeric_limits<std::uint64_t>::max();
    const auto decoded = std::get<control::DisplayConfiguration>(
        control::decodePayload(control::encodeDisplayConfiguration(display)).body);
    check(decoded.negotiationId == display.negotiationId && decoded.captureResolution.has_value(), "uint64 ID");
    display.clampingMode = static_cast<control::ClampingMode>(42);
    check(std::get<control::DisplayConfiguration>(control::decodePayload(
        control::encodeDisplayConfiguration(display)).body).clampingMode == display.clampingMode,
        "Future nonnegative enum preserved");
    display.clampingMode = static_cast<control::ClampingMode>(0x80000000u);
    rejected([&] { control::encodeDisplayConfiguration(display); });
    rejected([] { control::decodePayload(fromHex("c20206488080808008")); });

    control::Ping negative;
    negative.clientSendTimestampMs = -1;
    check(control::encodePing(negative) == fromHex("a2010b08ffffffffffffffffff01"), "int64 minus one");
    negative.clientSendTimestampMs = std::numeric_limits<std::int64_t>::min();
    check(control::encodePing(negative) == fromHex("a2010b0880808080808080808001"), "int64 minimum");
    for (const auto value : {std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(), std::int64_t{-1}, std::int64_t{0}})
    {
        const control::Ping original{value, value, value};
        const auto actual = std::get<control::Ping>(control::decodePayload(control::encodePing(original)).body);
        check(actual.clientSendTimestampMs == value && actual.serverReceiveTimestampMs == value &&
            actual.serverSendTimestampMs == value, "Signed int64 fields");
    }

    control::FeatureConfiguration text{{"", u8"🌙", std::string("a\0b", 3)}};
    const auto unicode = control::encodeFeatureConfiguration(text);
    check(unicode == fromHex("f2010d0a000a04f09f8c990a03610062"), "UTF-8 and embedded NUL bytes");
    check(std::get<control::FeatureConfiguration>(control::decodePayload(unicode).body).features == text.features,
        "Repeated empty string retained");
    for (const auto& hex : {"c080", "eda080", "f4908080", "e282", "80"})
    {
        const auto bad = fromHex(hex);
        const std::string string(bad.begin(), bad.end());
        rejected([&] { control::encodeFeatureConfiguration({{string}}); });
        rejected([&] { control::decodePayload(wrap(30, wrap(1, bad))); });
    }
    const std::string oversized(control::MaxStringBytes + 1, 'x');
    rejected([&] { control::encodeFeatureConfiguration({{oversized}}); });
    rejected([&] { control::decodePayload(wrap(30, wrap(1,
        control::Bytes(oversized.begin(), oversized.end())))); });
    control::FeatureConfiguration repeated;
    repeated.features.assign(control::MaxRepeatedItems + 1, "");
    rejected([&] { control::encodeFeatureConfiguration(repeated); });
    control::Bytes repeatedBody;
    for (std::size_t i = 0; i <= control::MaxRepeatedItems; ++i)
    {
        repeatedBody.push_back(0x0a); repeatedBody.push_back(0);
    }
    rejected([&] { control::decodePayload(wrap(30, repeatedBody)); });
    for (auto& byte : repeatedBody) if (byte == 0x0a) byte = 0x22;
    rejected([&] { control::decodePayload(wrap(40, repeatedBody)); });
    repeated.features.assign(control::MaxRepeatedItems, std::string(600, 'x'));
    rejected([&] { control::encodeFeatureConfiguration(repeated); });
}

struct FakeClock
{
    control::ClockReading now{1000, 0};
    std::size_t reads = 0;
    control::ClockReading read() { ++reads; return now; }
    void advance(std::int64_t ms) { now.epochMs += ms; now.steadyMs += ms; }
};
void initializeSample(control::ClockSynchronizer& sync, FakeClock& clock)
{
    sync.start();
    const auto ping = sync.nextPing();
    check(ping.has_value(), "Initial ping");
    clock.advance(30);
    const control::Ping response{ping->clientSendTimestampMs, ping->clientSendTimestampMs + 110,
        ping->clientSendTimestampMs + 120};
    check(sync.handlePing(response), "Initial sample accepted");
}
void checkClockMathAndPeer()
{
    FakeClock clock;
    control::ClockSynchronizer sync([&] { return clock.read(); });
    sync.start();
    const auto request = sync.nextPing();
    check(request && request->clientSendTimestampMs == 1000, "Synthetic peer request");
    // Reference is from the actual browser writer, wrapped like an incoming server packet.
    const auto payload = control::encodePing(*request);
    check(payload == fromHex("a2010308e807"), "Request on wire");
    clock.advance(30);
    const auto response = std::get<control::Ping>(control::decodeServerFrame(
        framed(fromHex("a2010908e80710d60818e008"))).body);
    check(sync.handlePing(response), "Framed peer response accepted");
    check(sync.sample()->offsetMs() == 100 && sync.sample()->delayMs == 20, "Offset and delay math");
    check(sync.serverTimeMs() == 1130, "Adjusted epoch clock");
    check(!sync.handlePing(response), "Replay rejected");
    clock.advance(969);
    check(!sync.nextPing(), "Burst not early");
    clock.advance(1);
    const auto second = sync.nextPing();
    check(second && second->clientSendTimestampMs == 2000, "Second burst ping");
    clock.advance(20);
    check(sync.handlePing({2000, 2111, 2112}), "Better sample accepted");
    check(sync.sample()->offsetMs() == 101.5 && sync.serverTimeMs() == 2121, "Positive half-ms precision");
    clock.advance(980);
    check(sync.nextPing().has_value(), "Third burst ping");
    clock.advance(40);
    check(!sync.handlePing({3000, 3110, 3120}), "Worse sample rejected");
    check(sync.sample()->offsetMs() == 101.5, "Best offset retained");
    const auto count = clock.reads;
    sync.stop();
    check(!sync.running() && sync.pendingCount() == 0 && !sync.nextPing() &&
        !sync.handlePing(response) && clock.reads == count, "Stop cancels scheduled/pending work");
    check(!sync.sample() && sync.serverTimeMs() == clock.now.epochMs, "Stopped clock is local");
    clock.now = {5000, 0};
    sync.start(); sync.nextPing(); clock.advance(20);
    check(sync.handlePing({5000, 4910, 4911}), "Negative offset sample");
    check(sync.sample()->offsetMs() == -99.5 && sync.serverTimeMs() == 4920, "Negative offset truncation");
}
void checkClockScheduling()
{
    FakeClock clock;
    control::ClockSynchronizer sync([&] { return clock.read(); });
    sync.start();
    std::optional<control::Ping> last;
    for (unsigned i = 0; i < 8; ++i)
    {
        last = sync.nextPing();
        check(last && !sync.nextPing(), "One ping per burst slot");
        if (i != 7) clock.advance(1000);
    }
    check(sync.pendingCount() == 8, "Pending burst bounded");
    clock.advance(30);
    check(sync.handlePing({last->clientSendTimestampMs, last->clientSendTimestampMs + 110,
        last->clientSendTimestampMs + 120}), "Last burst response");
    clock.advance(52969); // steady 59999, epoch 60999
    check(!sync.nextPing(), "Maintenance not early");
    clock.advance(1);
    check(sync.nextPing().has_value() && !sync.nextPing(), "One maintenance ping");
    clock.advance(60000);
    check(sync.nextPing().has_value() && !sync.nextPing(), "Next maintenance ping");
    clock.advance(control::ClockSynchronizer::StaleAfterMs - clock.now.steadyMs + 7031);
    check(!sync.sample(), "Stale sample cleared");
    check(sync.nextPing().has_value() && sync.pendingCount() == 1, "Stale restarts paced burst");
    sync.start();
    clock.advance(5000);
    check(sync.nextPing().has_value() && !sync.nextPing(), "Late polling does not flood eight sends");
    clock.advance(1000);
    check(sync.nextPing().has_value(), "Late burst remains paced");
}
void checkClockRejectionAndReset()
{
    for (const auto kind : {0, 1, 2, 3, 4, 5})
    {
        FakeClock clock;
        control::ClockSynchronizer sync([&] { return clock.read(); });
        sync.start(); const auto request = *sync.nextPing();
        clock.advance(kind == 4 ? 1000 : 20);
        control::Ping response{request.clientSendTimestampMs, 1110, 1120};
        if (kind == 0) response.serverSendTimestampMs = 1100; // reversed server times
        if (kind == 1) { response.serverReceiveTimestampMs = 0; response.serverSendTimestampMs = 0; }
        if (kind == 2) response.serverSendTimestampMs = 1140; // negative network delay
        if (kind == 3) response.serverSendTimestampMs = 1130; // zero network delay
        if (kind == 4) response.serverSendTimestampMs = response.serverReceiveTimestampMs; // delay 1000
        if (kind == 5) response.serverSendTimestampMs = control::ClockSynchronizer::MaxExactTimestamp + 1;
        check(!sync.handlePing(response) && sync.pendingCount() == 0 && !sync.sample(), "Bad sample rejected");
    }
    FakeClock clock;
    control::ClockSynchronizer sync([&] { return clock.read(); });
    sync.start(); const auto request = *sync.nextPing();
    clock.advance(20);
    check(!sync.handlePing({123456, 1110, 1120}) && sync.pendingCount() == 1, "Unsolicited reply");
    sync.cancelPing(request.clientSendTimestampMs);
    check(sync.pendingCount() == 0 && !sync.handlePing({1000, 1110, 1120}), "Cancelled send");
    sync.start(); const auto expiring = *sync.nextPing();
    clock.advance(control::ClockSynchronizer::PendingLifetimeMs + 1);
    check(!sync.handlePing({expiring.clientSendTimestampMs, expiring.clientSendTimestampMs + 1,
        expiring.clientSendTimestampMs + 2}) && sync.pendingCount() == 0, "Expired reply");

    initializeSample(sync, clock);
    clock.now.epochMs += 5000; clock.now.steadyMs += 1;
    check(sync.serverTimeMs() == clock.now.epochMs && !sync.sample() && sync.pendingCount() == 0,
        "Wall-clock step clears offset and requests");
    check(sync.nextPing().has_value(), "Clock step schedules new burst");
    initializeSample(sync, clock);
    clock.now.steadyMs -= 10;
    check(!sync.sample() && sync.nextPing().has_value(), "Steady-clock rollback resets sync");
    clock.now = {1000, 0};
    sync.start(); sync.nextPing(); clock.advance(999);
    check(sync.handlePing({1000, 1110, 1110}), "Initial 999-ms limit inclusive");
    clock.now.epochMs = -1;
    rejected([&] { sync.nextPing(); });
    rejected([] { control::ClockSynchronizer noClock(control::ClockSynchronizer::Clock{}); });
}
}

void runControlUnitTests()
{
    checkCodecEdges();
    checkClockMathAndPeer();
    checkClockScheduling();
    checkClockRejectionAndReset();
}
