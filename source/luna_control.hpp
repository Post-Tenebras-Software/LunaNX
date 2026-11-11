#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace luna::control
{
using Bytes = std::vector<std::uint8_t>;

// Native resource limits, not claims about limits imposed by Luna's server.
constexpr std::size_t MaxPayloadBytes = 65536;
constexpr std::size_t MaxStringBytes = 4096;
constexpr std::size_t MaxRepeatedItems = 128;

struct Ping
{
    std::int64_t clientSendTimestampMs = 0;
    std::int64_t serverReceiveTimestampMs = 0;
    std::int64_t serverSendTimestampMs = 0;
};
struct FeatureConfiguration { std::vector<std::string> features; };
struct Resolution { std::uint32_t width = 0, height = 0; };
struct SafeZones { std::uint32_t left = 0, right = 0, top = 0, bottom = 0; };
struct Display
{
    std::optional<Resolution> resolution;
    std::uint32_t refreshRate = 0;
    std::optional<SafeZones> safeZones;
};
enum class Codec : std::uint32_t { Avc = 0, Hevc = 1, Av1 = 2 };
enum class Tier : std::uint32_t { Main = 0, High = 1 };
enum class ClampingMode : std::uint32_t { CaptureResolution = 0, None = 1 };
struct VideoCodec
{
    Codec codec = Codec::Avc;
    std::uint32_t profile = 0, level = 0;
    Tier tier = Tier::Main;
    std::uint32_t maxReferenceFrames = 0;
};
struct DisplayConfiguration
{
    std::uint64_t negotiationId = 0;
    std::optional<Resolution> captureResolution;
    std::optional<Display> display;
    std::vector<VideoCodec> codecs;
    std::vector<std::string> features;
    std::uint32_t minBitsPerSecond = 0, maxBitsPerSecond = 0, startBitsPerSecond = 0;
    ClampingMode clampingMode = ClampingMode::CaptureResolution;
};
struct BrowserMetadata
{
    std::string browserType, browserVersion, deviceModel, deviceType, osName, osVersion;
};
struct ClientContext { std::optional<BrowserMetadata> browserMetadata; };
struct CloseConnectionRequest
{
    std::string sessionId;
    std::uint32_t connectionId = 0, closingReason = 0;
};
struct IdleTimeoutWarning { std::uint32_t connectionId = 0, warnTime = 0; };
struct ResetIdleTimeoutRequest {};
struct AckCloseConnectionRequest {};

// A known envelope event outside this startup subset. Retained for dispatch;
// the codec does not implement participant/UI/application message behavior.
struct UnhandledMessage { std::uint32_t field = 0; Bytes payload; };
using Body = std::variant<std::monostate, Ping, FeatureConfiguration,
    DisplayConfiguration, ClientContext, CloseConnectionRequest, IdleTimeoutWarning,
    ResetIdleTimeoutRequest, AckCloseConnectionRequest, UnhandledMessage>;
struct Message
{
    std::uint32_t profilingMarkerIdentifier = 0;
    Body body;
};

// Bare BrowserClientMessage bytes. Strings are UTF-8. Zero scalars are omitted;
// explicit nested messages and empty repeated-string entries retain presence.
Bytes encodeFeatureConfiguration(const FeatureConfiguration& configuration);
Bytes encodeDisplayConfiguration(const DisplayConfiguration& configuration);
Bytes encodeClientContext(const BrowserMetadata& metadata = {});
Bytes encodePing(const Ping& ping);
Bytes encodeCloseAck();
Bytes encodeResetIdleTimeout();

// Throws std::invalid_argument on malformed/unsupported wire encoding or limits.
// Decode bare bytes for tests; use decodeServerFrame for incoming control traffic.
Message decodePayload(const std::uint8_t* data, std::size_t size);
inline Message decodePayload(const Bytes& bytes) { return decodePayload(bytes.data(), bytes.size()); }
Message decodeServerFrame(const std::uint8_t* data, std::size_t size);
inline Message decodeServerFrame(const Bytes& bytes) { return decodeServerFrame(bytes.data(), bytes.size()); }
}
