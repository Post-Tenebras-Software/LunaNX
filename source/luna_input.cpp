#include "luna_input.hpp"

#include <cmath>
#include <stdexcept>

namespace luna::input
{
namespace
{
void appendVarint(Bytes& output, std::uint64_t value)
{
    while (value >= 0x80)
    {
        output.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendScalar(Bytes& output, std::uint32_t field, std::uint64_t value)
{
    if (value == 0)
        return;
    appendVarint(output, static_cast<std::uint64_t>(field) << 3);
    appendVarint(output, value);
}

void appendMessage(Bytes& output, std::uint32_t field, const Bytes& body)
{
    appendVarint(output, (static_cast<std::uint64_t>(field) << 3) | 2);
    appendVarint(output, body.size());
    output.insert(output.end(), body.begin(), body.end());
}

std::uint32_t zigzag(std::int32_t value)
{
    // Unsigned shifts also handle INT32_MIN without signed overflow.
    return (static_cast<std::uint32_t>(value) << 1) ^
        (0u - static_cast<std::uint32_t>(value < 0));
}

void appendResiliency(Bytes& output,
    const std::optional<ResiliencyAttributes>& attributes)
{
    if (!attributes)
        return;
    if (static_cast<std::uint32_t>(attributes->category) > 2)
        throw std::invalid_argument("Unknown input resiliency category");
    Bytes body;
    appendScalar(body, 10, static_cast<std::uint32_t>(attributes->category));
    appendScalar(body, 20, attributes->eventId);
    appendScalar(body, 30, attributes->keyframeId);
    appendScalar(body, 40, attributes->terminalIdentifier);
    appendScalar(body, 50, attributes->retransmitCount);
    appendMessage(output, 1, body);
}

void validateType(GamepadType type)
{
    if (type != GamepadType::X360 && type != GamepadType::DS4)
        throw std::invalid_argument("Unknown gamepad injection type");
}
}

std::uint32_t convertAxis(double value)
{
    if (!std::isfinite(value) || value < -1 || value > 1)
        throw std::invalid_argument("Gamepad axis must be finite and in [-1, 1]");
    return static_cast<std::uint32_t>(std::floor(32767 * value) + 32767);
}

std::uint32_t convertTrigger(double value)
{
    if (!std::isfinite(value) || value < 0 || value > 1)
        throw std::invalid_argument("Gamepad trigger must be finite and in [0, 1]");
    return static_cast<std::uint32_t>(std::floor(255 * value + 0.5));
}

Bytes encodeGamepad(const GamepadState& state, const InputTiming& timing,
    const std::optional<ResiliencyAttributes>& resiliency)
{
    validateType(state.type);
    if (state.leftTrigger > 255 || state.rightTrigger > 255)
        throw std::invalid_argument("Encoded trigger must be in [0, 255]");
    if (state.leftX > 65534 || state.leftY > 65534 ||
        state.rightX > 65534 || state.rightY > 65534)
        throw std::invalid_argument("Encoded axis must be in [0, 65534]");

    Bytes body;
    appendScalar(body, 1, state.deviceId);
    appendScalar(body, 2, state.packetNumber);
    appendScalar(body, 3, state.buttons);
    appendScalar(body, 4, state.leftTrigger);
    appendScalar(body, 5, state.rightTrigger);
    appendScalar(body, 6, state.leftX);
    appendScalar(body, 7, state.leftY);
    appendScalar(body, 8, state.rightX);
    appendScalar(body, 9, state.rightY);
    appendScalar(body, 11, static_cast<std::uint32_t>(state.type));

    Bytes latency;
    // int64 uses two's-complement varints, unlike the sint32 sensitivities.
    const auto timestamp = static_cast<std::uint64_t>(timing.timestampMs);
    appendScalar(latency, 1, timestamp);
    appendScalar(latency, 3, static_cast<std::uint32_t>(timing.hardwareClass));
    appendScalar(latency, 8, timestamp);
    appendMessage(body, 12, latency);
    appendScalar(body, 13, zigzag(state.leftSensitivity));
    appendScalar(body, 14, zigzag(state.rightSensitivity));

    Bytes output;
    appendResiliency(output, resiliency);
    appendMessage(output, 104, body);
    return output;
}

Bytes encodeConnection(std::uint32_t deviceId, bool connected, GamepadType type,
    const std::optional<ResiliencyAttributes>& resiliency)
{
    validateType(type);
    Bytes body;
    appendScalar(body, 1, deviceId);
    appendScalar(body, 2, connected ? 0 : 1);
    appendScalar(body, 10, static_cast<std::uint32_t>(type));
    Bytes output;
    appendResiliency(output, resiliency);
    appendMessage(output, 105, body);
    return output;
}

Bytes encodePointerModeAck()
{
    Bytes output;
    appendMessage(output, 36, {});
    return output;
}
}
