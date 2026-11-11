#include "luna_control.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace control = luna::control;
void runControlUnitTests();

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template <typename T> T read()
{
    T value{};
    if (!(std::cin >> value)) throw std::runtime_error("Malformed probe command");
    return value;
}
control::Bytes fromHex(const std::string& hex)
{
    check(hex.size() % 2 == 0, "Odd hex length");
    const auto digit = [](char c) -> unsigned
    {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("Invalid hex digit");
    };
    control::Bytes result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(16 * digit(hex[i]) + digit(hex[i + 1])));
    return result;
}
std::string readString()
{
    const auto hex = read<std::string>();
    if (hex == "-") return {};
    const auto bytes = fromHex(hex);
    return std::string(bytes.begin(), bytes.end());
}
std::vector<std::string> readStrings()
{
    const auto count = read<unsigned>();
    check(count <= control::MaxRepeatedItems, "Too many probe strings");
    std::vector<std::string> result;
    for (unsigned i = 0; i < count; ++i) result.push_back(readString());
    return result;
}

void checkDisplay(const control::DisplayConfiguration& actual, const control::DisplayConfiguration& expected)
{
    check(actual.negotiationId == expected.negotiationId, "Display negotiation ID");
    check(actual.display.has_value() && expected.display.has_value(), "Display presence");
    const auto& a = *actual.display;
    const auto& e = *expected.display;
    check(a.resolution.has_value() && a.safeZones.has_value(), "Display nested presence");
    check(a.resolution->width == e.resolution->width && a.resolution->height == e.resolution->height,
        "Display resolution");
    check(a.refreshRate == e.refreshRate, "Display refresh rate");
    check(a.safeZones->left == e.safeZones->left && a.safeZones->right == e.safeZones->right &&
        a.safeZones->top == e.safeZones->top && a.safeZones->bottom == e.safeZones->bottom, "Safe zones");
    check(actual.codecs.size() == expected.codecs.size(), "Codec count");
    for (std::size_t i = 0; i < actual.codecs.size(); ++i)
    {
        const auto& x = actual.codecs[i];
        const auto& y = expected.codecs[i];
        check(x.codec == y.codec && x.profile == y.profile && x.level == y.level && x.tier == y.tier &&
            x.maxReferenceFrames == y.maxReferenceFrames, "Codec fields");
    }
    check(actual.features == expected.features, "Display features");
    check(actual.minBitsPerSecond == expected.minBitsPerSecond &&
        actual.maxBitsPerSecond == expected.maxBitsPerSecond &&
        actual.startBitsPerSecond == expected.startBitsPerSecond &&
        actual.clampingMode == expected.clampingMode, "Display bitrate/clamping");
}

int main()
{
    try
    {
        runControlUnitTests();
        std::string kind;
        std::size_t count = 0;
        while (std::cin >> kind)
        {
            const auto expected = fromHex(read<std::string>());
            control::Bytes encoded;
            if (kind == "feature")
            {
                const control::FeatureConfiguration config{readStrings()};
                encoded = control::encodeFeatureConfiguration(config);
                check(std::get<control::FeatureConfiguration>(control::decodePayload(expected).body).features ==
                    config.features, "Decoded feature list");
            }
            else if (kind == "ping")
            {
                const control::Ping ping{read<std::int64_t>(), read<std::int64_t>(), read<std::int64_t>()};
                encoded = control::encodePing(ping);
                const auto decoded = std::get<control::Ping>(control::decodePayload(expected).body);
                check(decoded.clientSendTimestampMs == ping.clientSendTimestampMs &&
                    decoded.serverReceiveTimestampMs == ping.serverReceiveTimestampMs &&
                    decoded.serverSendTimestampMs == ping.serverSendTimestampMs, "Decoded ping fields");
            }
            else if (kind == "context")
            {
                const control::BrowserMetadata metadata{readString(), readString(), readString(),
                    readString(), readString(), readString()};
                encoded = control::encodeClientContext(metadata);
                const auto decoded = std::get<control::ClientContext>(control::decodePayload(expected).body);
                check(decoded.browserMetadata.has_value(), "Browser metadata presence");
                const auto& m = *decoded.browserMetadata;
                check(m.browserType == metadata.browserType && m.browserVersion == metadata.browserVersion &&
                    m.deviceModel == metadata.deviceModel && m.deviceType == metadata.deviceType &&
                    m.osName == metadata.osName && m.osVersion == metadata.osVersion, "Decoded metadata");
            }
            else if (kind == "display")
            {
                control::DisplayConfiguration config;
                config.negotiationId = read<std::uint64_t>();
                const control::Resolution resolution{read<std::uint32_t>(), read<std::uint32_t>()};
                const auto refresh = read<std::uint32_t>();
                const control::SafeZones zones{read<std::uint32_t>(), read<std::uint32_t>(),
                    read<std::uint32_t>(), read<std::uint32_t>()};
                config.display = control::Display{resolution, refresh, zones};
                config.minBitsPerSecond = read<std::uint32_t>();
                config.maxBitsPerSecond = read<std::uint32_t>();
                config.startBitsPerSecond = read<std::uint32_t>();
                config.clampingMode = static_cast<control::ClampingMode>(read<std::uint32_t>());
                const auto codecs = read<unsigned>();
                check(codecs <= control::MaxRepeatedItems, "Too many probe codecs");
                for (unsigned i = 0; i < codecs; ++i)
                    config.codecs.push_back({static_cast<control::Codec>(read<std::uint32_t>()),
                        read<std::uint32_t>(), read<std::uint32_t>(),
                        static_cast<control::Tier>(read<std::uint32_t>()), read<std::uint32_t>()});
                config.features = readStrings();
                encoded = control::encodeDisplayConfiguration(config);
                checkDisplay(std::get<control::DisplayConfiguration>(control::decodePayload(expected).body), config);
            }
            else if (kind == "ackClose")
            {
                encoded = control::encodeCloseAck();
                check(std::holds_alternative<control::AckCloseConnectionRequest>(
                    control::decodePayload(expected).body), "Close ACK type");
            }
            else if (kind == "resetIdle")
            {
                encoded = control::encodeResetIdleTimeout();
                check(std::holds_alternative<control::ResetIdleTimeoutRequest>(
                    control::decodePayload(expected).body), "Idle reset type");
            }
            else throw std::runtime_error("Unknown probe command");
            check(encoded == expected, "Native bytes differ from original writer");
            ++count;
        }
        std::cout << "PASS: " << count << " original-writer control examples encode/decode; "
            "framing, malformed input, synthetic peer, clock scheduling and lifecycle checks passed.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
